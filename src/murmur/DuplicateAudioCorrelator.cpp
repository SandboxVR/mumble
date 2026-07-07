// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DuplicateAudioCorrelator.h"

#include <opus.h>

#include <algorithm>
#include <cmath>

DuplicateAudioCorrelator::DuplicateAudioCorrelator() = default;

DuplicateAudioCorrelator::~DuplicateAudioCorrelator() {
	std::lock_guard< std::mutex > lock(m_mutex);
	for (auto &entry : m_history) {
		if (entry.second.decoder) {
			opus_decoder_destroy(entry.second.decoder);
		}
	}
}

void DuplicateAudioCorrelator::setConfig(const Config &config) {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_config.correlationThreshold = std::max(0.0, config.correlationThreshold);
	m_config.verdictCacheMilliseconds = std::max< std::int64_t >(0, config.verdictCacheMilliseconds);
	m_cachedVerdicts.clear();
}

DuplicateAudioCorrelator::Config DuplicateAudioCorrelator::config() const {
	std::lock_guard< std::mutex > lock(m_mutex);
	return m_config;
}

void DuplicateAudioCorrelator::submitPacket(std::uint32_t sessionID, Mumble::Protocol::AudioCodec codec,
											 const unsigned char *payload, std::size_t payloadSize,
											 std::int64_t timestampMilliseconds) {
	if (codec != Mumble::Protocol::AudioCodec::Opus || payload == nullptr || payloadSize == 0) {
		// Phase 2 only supports Opus decoding. Non-Opus packets are silently skipped;
		// isLikelySameSource() will fail open (not enough data) for sessions using them.
		return;
	}

	std::lock_guard< std::mutex > lock(m_mutex);

	DecodedAudioHistory &history = m_history[sessionID]; // default-constructs on first use
	if (!history.decoder) {
		int err = 0;
		// Mono: voice is always encoded mono by the client
		// (AudioInput.cpp: opus_encoder_create(SAMPLE_RATE, 1, ...)). Do NOT use the
		// client's playback-side forced-stereo decoder config (AudioOutputSpeech.cpp) --
		// that upmixes for output mixing and is irrelevant/wrong here.
		history.decoder = opus_decoder_create(SAMPLE_RATE_HZ, 1, &err);
		if (err != OPUS_OK || !history.decoder) {
			history.decoder = nullptr;
			return;
		}
		history.ringBuffer.assign(HISTORY_CAPACITY_SAMPLES, 0.0f);
		history.writeIndex   = 0;
		history.validSamples = 0;
	}

	if (history.validSamples > 0
		&& (timestampMilliseconds < history.lastPacketTimestampMilliseconds
			|| timestampMilliseconds - history.lastPacketTimestampMilliseconds > MAX_SUBMISSION_GAP_MS)) {
		opus_decoder_ctl(history.decoder, OPUS_RESET_STATE);
		history.writeIndex   = 0;
		history.validSamples = 0;
	}

	float decodeScratch[MAX_SAMPLES_PER_PACKET];
	int decodedSamples = opus_decode_float(history.decoder, payload, static_cast< int >(payloadSize),
											decodeScratch, MAX_SAMPLES_PER_PACKET, 0);
	if (decodedSamples <= 0) {
		// Decode error (negative) or 0 samples -- do not touch the ring buffer/history
		// state on error; leave prior valid history in place rather than corrupting it.
		return;
	}

	for (int i = 0; i < decodedSamples; ++i) {
		history.ringBuffer[history.writeIndex] = decodeScratch[i];
		history.writeIndex                     = (history.writeIndex + 1) % HISTORY_CAPACITY_SAMPLES;
	}
	history.validSamples = std::min(history.validSamples + static_cast< std::size_t >(decodedSamples),
									 HISTORY_CAPACITY_SAMPLES);
	history.lastPacketTimestampMilliseconds = timestampMilliseconds;
}

bool DuplicateAudioCorrelator::isLikelySameSource(std::uint32_t sessionA, std::uint32_t sessionB,
												   std::int64_t nowMilliseconds) {
	HistorySnapshot snapshotA;
	HistorySnapshot snapshotB;
	Config config;

	const std::uint64_t cacheKey = pairKey(sessionA, sessionB);

	{
		std::lock_guard< std::mutex > lock(m_mutex);
		m_lastCorrelationScore = -1.0;
		config = m_config;

		auto cacheIt = m_cachedVerdicts.find(cacheKey);
		if (cacheIt != m_cachedVerdicts.end()
			&& nowMilliseconds - cacheIt->second.timestampMilliseconds <= config.verdictCacheMilliseconds) {
			m_lastCorrelationScore = cacheIt->second.score;
			return cacheIt->second.confirmed;
		}

		auto itA = m_history.find(sessionA);
		auto itB = m_history.find(sessionB);
		if (itA == m_history.end() || itB == m_history.end()) {
			return false;
		}

		const DecodedAudioHistory &a = itA->second;
		const DecodedAudioHistory &b = itB->second;

		if (nowMilliseconds - a.lastPacketTimestampMilliseconds > MAX_HISTORY_AGE_MS
			|| nowMilliseconds - b.lastPacketTimestampMilliseconds > MAX_HISTORY_AGE_MS) {
			return false;
		}

		if (a.validSamples < MIN_SAMPLES_FOR_CORRELATION || b.validSamples < MIN_SAMPLES_FOR_CORRELATION) {
			return false;
		}

		snapshotA.samples = linearize(a);
		snapshotA.lastPacketTimestampMilliseconds = a.lastPacketTimestampMilliseconds;
		snapshotB.samples = linearize(b);
		snapshotB.lastPacketTimestampMilliseconds = b.lastPacketTimestampMilliseconds;
	}

	std::vector< double > envelopeA = logEnergyEnvelope(snapshotA.samples);
	std::vector< double > envelopeB = logEnergyEnvelope(snapshotB.samples);

	if (envelopeA.size() < static_cast< std::size_t >(MIN_ENVELOPE_FRAMES_FOR_CORRELATION)
		|| envelopeB.size() < static_cast< std::size_t >(MIN_ENVELOPE_FRAMES_FOR_CORRELATION)) {
		return false;
	}

	const std::int64_t startA = snapshotA.lastPacketTimestampMilliseconds
								- static_cast< std::int64_t >((envelopeA.size() - 1) * ENVELOPE_HOP_MS);
	const std::int64_t startB = snapshotB.lastPacketTimestampMilliseconds
								- static_cast< std::int64_t >((envelopeB.size() - 1) * ENVELOPE_HOP_MS);
	const int expectedLagEnvelopeHops =
		static_cast< int >(std::llround(static_cast< double >(startA - startB) / ENVELOPE_HOP_MS));

	const double score = normalizedEnvelopeCorrelationPeak(envelopeA, envelopeB, expectedLagEnvelopeHops,
														   MAX_RESIDUAL_LAG_ENVELOPE_HOPS);
	const bool confirmed = score >= config.correlationThreshold;

	{
		std::lock_guard< std::mutex > lock(m_mutex);
		m_lastCorrelationScore = score;
		CachedVerdict verdict;
		verdict.timestampMilliseconds = nowMilliseconds;
		verdict.score = score;
		verdict.confirmed = confirmed;
		m_cachedVerdicts[cacheKey] = verdict;
	}

	return confirmed;
}

void DuplicateAudioCorrelator::forgetSession(std::uint32_t sessionID) {
	std::lock_guard< std::mutex > lock(m_mutex);
	auto it = m_history.find(sessionID);
	if (it != m_history.end()) {
		if (it->second.decoder) {
			opus_decoder_destroy(it->second.decoder);
		}
		m_history.erase(it);
	}

	for (auto cacheIt = m_cachedVerdicts.begin(); cacheIt != m_cachedVerdicts.end();) {
		const std::uint32_t first = static_cast< std::uint32_t >(cacheIt->first >> 32);
		const std::uint32_t second = static_cast< std::uint32_t >(cacheIt->first & 0xffffffffu);
		if (first == sessionID || second == sessionID) {
			cacheIt = m_cachedVerdicts.erase(cacheIt);
		} else {
			++cacheIt;
		}
	}
}

double DuplicateAudioCorrelator::lastCorrelationScore() const {
	std::lock_guard< std::mutex > lock(m_mutex);
	return m_lastCorrelationScore;
}

std::uint64_t DuplicateAudioCorrelator::pairKey(std::uint32_t sessionA, std::uint32_t sessionB) {
	const std::uint32_t first = std::min(sessionA, sessionB);
	const std::uint32_t second = std::max(sessionA, sessionB);
	return (static_cast< std::uint64_t >(first) << 32) | second;
}

std::vector< float > DuplicateAudioCorrelator::linearize(const DecodedAudioHistory &history) {
	std::vector< float > out(history.validSamples);
	std::size_t start =
		(history.writeIndex + HISTORY_CAPACITY_SAMPLES - history.validSamples) % HISTORY_CAPACITY_SAMPLES;
	for (std::size_t i = 0; i < history.validSamples; ++i) {
		out[i] = history.ringBuffer[(start + i) % HISTORY_CAPACITY_SAMPLES];
	}
	return out;
}

std::vector< double > DuplicateAudioCorrelator::logEnergyEnvelope(const std::vector< float > &samples) {
	const std::size_t frameCount = samples.size() / ENVELOPE_HOP_SAMPLES;
	std::vector< double > envelope(frameCount);

	for (std::size_t frame = 0; frame < frameCount; ++frame) {
		double sumSquares = 0.0;
		const std::size_t start = frame * ENVELOPE_HOP_SAMPLES;
		for (std::size_t i = 0; i < ENVELOPE_HOP_SAMPLES; ++i) {
			const double sample = samples[start + i];
			sumSquares += sample * sample;
		}

		const double meanSquare = sumSquares / static_cast< double >(ENVELOPE_HOP_SAMPLES);
		envelope[frame] = std::log(std::max(meanSquare, 1e-10));
	}

	return envelope;
}

double DuplicateAudioCorrelator::normalizedEnvelopeCorrelationPeak(const std::vector< double > &a,
																	const std::vector< double > &b,
																	int expectedLagEnvelopeHops,
																	int maxResidualLagEnvelopeHops) {
	double bestScore = 0.0;

	const int lenA = static_cast< int >(a.size());
	const int lenB = static_cast< int >(b.size());

	for (int lag = expectedLagEnvelopeHops - maxResidualLagEnvelopeHops;
		 lag <= expectedLagEnvelopeHops + maxResidualLagEnvelopeHops; ++lag) {
		int start      = std::max(0, -lag);
		int end        = std::min(lenA, lenB - lag);
		int overlapLen = end - start;
		if (overlapLen < MIN_ENVELOPE_FRAMES_FOR_CORRELATION) {
			continue;
		}

		double sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
		for (int i = start; i < end; ++i) {
			const double va = a[static_cast< std::size_t >(i)];
			const double vb = b[static_cast< std::size_t >(i + lag)];
			sumA += va;
			sumB += vb;
			sumAA += va * va;
			sumBB += vb * vb;
			sumAB += va * vb;
		}

		const double n = static_cast< double >(overlapLen);
		const double covariance = sumAB - (sumA * sumB) / n;
		const double varA = sumAA - (sumA * sumA) / n;
		const double varB = sumBB - (sumB * sumB) / n;
		const double denom = std::sqrt(std::max(varA, 0.0) * std::max(varB, 0.0));

		if (denom < 1e-9) {
			continue;
		}

		bestScore = std::max(bestScore, covariance / denom);
	}

	return bestScore;
}

double DuplicateAudioCorrelator::normalizedCrossCorrelationPeak(const std::vector< float > &a,
																  const std::vector< float > &b,
																  int maxLagSamples) {
	// Normalized (Pearson-style) cross-correlation, searched over lags in
	// [-maxLagSamples, +maxLagSamples]. For each lag, compute correlation over the
	// overlapping region; return the maximum |correlation| across all searched lags. Using
	// |correlation| (not signed) because a duplicate mic pickup should be in-phase, but a
	// sign convention slip shouldn't silently halve sensitivity -- if this needs to be
	// signed-only after real-audio tuning, that's a one-line change (drop the fabs).
	double bestScore = 0.0;

	const int lenA = static_cast< int >(a.size());
	const int lenB = static_cast< int >(b.size());

	for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
		// a[i] aligns with b[i + lag]
		int start      = std::max(0, -lag);
		int end        = std::min(lenA, lenB - lag);
		int overlapLen = end - start;
		if (overlapLen < static_cast< int >(MIN_SAMPLES_FOR_CORRELATION)) {
			continue;
		}

		double sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
		for (int i = start; i < end; ++i) {
			double va = a[static_cast< std::size_t >(i)];
			double vb = b[static_cast< std::size_t >(i + lag)];
			sumA += va;
			sumB += vb;
			sumAA += va * va;
			sumBB += vb * vb;
			sumAB += va * vb;
		}

		double n          = static_cast< double >(overlapLen);
		double covariance = sumAB - (sumA * sumB) / n;
		double varA       = sumAA - (sumA * sumA) / n;
		double varB       = sumBB - (sumB * sumB) / n;
		double denom      = std::sqrt(std::max(varA, 0.0) * std::max(varB, 0.0));

		if (denom < 1e-9) {
			// Near-silence on one or both sides -- undefined correlation, skip (do not
			// treat silence-vs-silence as a confident "same source" match).
			continue;
		}

		double correlation = covariance / denom;
		bestScore           = std::max(bestScore, std::fabs(correlation));
	}

	return bestScore;
}
