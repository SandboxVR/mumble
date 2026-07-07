// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MURMUR_DUPLICATEAUDIOCORRELATOR_H_
#define MUMBLE_MURMUR_DUPLICATEAUDIOCORRELATOR_H_

#include "IAcousticOracle.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

struct OpusDecoder;

// Real, Opus-backed implementation of IAcousticOracle. Decodes mono float PCM per
// session on demand (gated -- only for sessions submitted via submitPacket, which
// DuplicateVoiceSuppressor only calls for packets already flagged as overlap candidates
// by the Phase 1 heuristic) and cross-correlates a short rolling buffer of recently
// decoded PCM between two sessions to judge "same acoustic event" vs. "two different
// speakers".
//
// processMsg() can run concurrently from two threads (the dedicated UDP voice thread and
// the main thread's TCP-tunnel path), both holding only a shared read lock on
// qrwlVoiceThread. This class therefore guards its own state with an internal mutex
// rather than relying on that lock.
class DuplicateAudioCorrelator : public IAcousticOracle {
public:
	DuplicateAudioCorrelator();
	~DuplicateAudioCorrelator() override;

	DuplicateAudioCorrelator(const DuplicateAudioCorrelator &) = delete;
	DuplicateAudioCorrelator &operator=(const DuplicateAudioCorrelator &) = delete;

	bool isLikelySameSource(std::uint32_t sessionA, std::uint32_t sessionB,
							 std::int64_t nowMilliseconds) override;
	void submitPacket(std::uint32_t sessionID, Mumble::Protocol::AudioCodec codec,
					   const unsigned char *payload, std::size_t payloadSize,
					   std::int64_t timestampMilliseconds) override;
	void forgetSession(std::uint32_t sessionID) override;
	double lastCorrelationScore() const override;

	// Initial threshold; starting point pending real-audio tuning (see
	// docs/dev/DuplicateVoiceSuppressionPlan.md open questions). Declared here, not buried
	// in the .cpp, so follow-up tuning work can find and change it without touching logic.
	static constexpr double DEFAULT_CORRELATION_THRESHOLD = 0.65;
	static constexpr std::int64_t DEFAULT_VERDICT_CACHE_MS = 150;

	struct Config {
		double correlationThreshold = DEFAULT_CORRELATION_THRESHOLD;
		std::int64_t verdictCacheMilliseconds = DEFAULT_VERDICT_CACHE_MS;
	};

	void setConfig(const Config &config);
	Config config() const;

private:
	// Mono float PCM samples at 48kHz, rolling ring buffer of recently decoded audio.
	struct DecodedAudioHistory {
		OpusDecoder *decoder = nullptr;
		std::vector< float > ringBuffer; // fixed capacity, see HISTORY_CAPACITY_SAMPLES
		std::size_t writeIndex           = 0;
		std::size_t validSamples         = 0; // samples written so far, saturates at capacity
		std::int64_t lastPacketTimestampMilliseconds = 0;
	};

	struct HistorySnapshot {
		std::vector< float > samples;
		std::int64_t lastPacketTimestampMilliseconds = 0;
	};

	struct CachedVerdict {
		std::int64_t timestampMilliseconds = 0;
		double score = -1.0;
		bool confirmed = false;
	};

	static constexpr int SAMPLE_RATE_HZ            = 48000;
	static constexpr int MAX_SAMPLES_PER_PACKET    = SAMPLE_RATE_HZ * 60 / 1000; // 60ms max Opus frame, mono
	static constexpr int ENVELOPE_HOP_MS          = 5;
	static constexpr std::size_t ENVELOPE_HOP_SAMPLES = SAMPLE_RATE_HZ * ENVELOPE_HOP_MS / 1000; // 240 samples
	// Keep enough decoded history for a >=200ms envelope overlap plus residual lag search.
	static constexpr std::size_t HISTORY_CAPACITY_SAMPLES = SAMPLE_RATE_HZ * 300 / 1000; // 300ms
	static constexpr int MAX_RESIDUAL_LAG_ENVELOPE_HOPS = 20 / ENVELOPE_HOP_MS; // +/-20ms
	// Minimum overlapping history (post-lag-alignment) required before we trust a
	// correlation result at all; below this we return "not confirmed" (fail open) rather
	// than a spurious high/low score from too little data.
	static constexpr std::size_t MIN_SAMPLES_FOR_CORRELATION = SAMPLE_RATE_HZ * 200 / 1000; // 200ms
	static constexpr int MIN_ENVELOPE_FRAMES_FOR_CORRELATION =
		static_cast< int >(MIN_SAMPLES_FOR_CORRELATION / ENVELOPE_HOP_SAMPLES);
	// History older than this is not "current" enough to trust; comfortably larger than
	// Phase 1's OVERLAP_WINDOW_MS (120ms) plus the suppressor's 250ms capture window.
	static constexpr std::int64_t MAX_HISTORY_AGE_MS = 250;
	static constexpr std::int64_t MAX_SUBMISSION_GAP_MS = 80;

	mutable std::mutex m_mutex; // guards m_history, m_cachedVerdicts, and m_lastCorrelationScore
	std::unordered_map< std::uint32_t, DecodedAudioHistory > m_history;
	std::unordered_map< std::uint64_t, CachedVerdict > m_cachedVerdicts;
	double m_lastCorrelationScore = -1.0;
	Config m_config;

	static std::uint64_t pairKey(std::uint32_t sessionA, std::uint32_t sessionB);
	static std::vector< float > linearize(const DecodedAudioHistory &history);
	static std::vector< double > logEnergyEnvelope(const std::vector< float > &samples);
	static double normalizedEnvelopeCorrelationPeak(const std::vector< double > &a,
													const std::vector< double > &b,
													int expectedLagEnvelopeHops,
													int maxResidualLagEnvelopeHops);
	static double normalizedCrossCorrelationPeak(const std::vector< float > &a, const std::vector< float > &b,
												  int maxLagSamples);
};

#endif // MUMBLE_MURMUR_DUPLICATEAUDIOCORRELATOR_H_
