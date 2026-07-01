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
	static constexpr double CORRELATION_THRESHOLD = 0.75;

private:
	// Mono float PCM samples at 48kHz, rolling ring buffer of recently decoded audio.
	struct DecodedAudioHistory {
		OpusDecoder *decoder = nullptr;
		std::vector< float > ringBuffer; // fixed capacity, see HISTORY_CAPACITY_SAMPLES
		std::size_t writeIndex           = 0;
		std::size_t validSamples         = 0; // samples written so far, saturates at capacity
		std::int64_t lastPacketTimestampMilliseconds = 0;
	};

	static constexpr int SAMPLE_RATE_HZ            = 48000;
	static constexpr int MAX_SAMPLES_PER_PACKET    = SAMPLE_RATE_HZ * 60 / 1000; // 60ms max Opus frame, mono
	// How much decoded history we keep per session to correlate against: 100ms @ 48kHz mono.
	// Large enough to cover several packets (typically 10-20ms each) and the lag search
	// window below, small enough to keep memory + correlation cost per pair trivially cheap.
	static constexpr std::size_t HISTORY_CAPACITY_SAMPLES = SAMPLE_RATE_HZ / 10; // 4800 samples = 100ms
	// Lag search window: mic-distance + network jitter is expected to put the true peak
	// within a few ms; search a bit wider to be safe. +/- 15ms @ 48kHz.
	static constexpr int MAX_LAG_SAMPLES = SAMPLE_RATE_HZ * 15 / 1000; // 720 samples
	// Minimum overlapping history (post-lag-alignment) required before we trust a
	// correlation result at all; below this we return "not confirmed" (fail open) rather
	// than a spurious high/low score from too little data.
	static constexpr std::size_t MIN_SAMPLES_FOR_CORRELATION = SAMPLE_RATE_HZ * 20 / 1000; // 20ms
	// History older than this is not "current" enough to trust; comfortably larger than
	// Phase 1's OVERLAP_WINDOW_MS (120ms) plus this class's own history depth (100ms).
	static constexpr std::int64_t MAX_HISTORY_AGE_MS = 250;

	mutable std::mutex m_mutex; // guards m_history and m_lastCorrelationScore
	std::unordered_map< std::uint32_t, DecodedAudioHistory > m_history;
	double m_lastCorrelationScore = -1.0;

	static double normalizedCrossCorrelationPeak(const std::vector< float > &a, const std::vector< float > &b,
												  int maxLagSamples);
};

#endif // MUMBLE_MURMUR_DUPLICATEAUDIOCORRELATOR_H_
