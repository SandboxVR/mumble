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
	std::lock_guard< std::mutex > lock(m_mutex);
	m_lastCorrelationScore = -1.0;

	auto itA = m_history.find(sessionA);
	auto itB = m_history.find(sessionB);
	if (itA == m_history.end() || itB == m_history.end()) {
		return false; // fail open: not enough data to confirm -> caller should not veto Phase 1
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

	// Linearize ring buffers into contiguous vectors covering the valid tail.
	auto linearize = [](const DecodedAudioHistory &history) {
		std::vector< float > out(history.validSamples);
		std::size_t start =
			(history.writeIndex + HISTORY_CAPACITY_SAMPLES - history.validSamples) % HISTORY_CAPACITY_SAMPLES;
		for (std::size_t i = 0; i < history.validSamples; ++i) {
			out[i] = history.ringBuffer[(start + i) % HISTORY_CAPACITY_SAMPLES];
		}
		return out;
	};

	std::vector< float > samplesA = linearize(a);
	std::vector< float > samplesB = linearize(b);

	double score            = normalizedCrossCorrelationPeak(samplesA, samplesB, MAX_LAG_SAMPLES);
	m_lastCorrelationScore  = score;
	return score >= CORRELATION_THRESHOLD;
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
}

double DuplicateAudioCorrelator::lastCorrelationScore() const {
	std::lock_guard< std::mutex > lock(m_mutex);
	return m_lastCorrelationScore;
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
