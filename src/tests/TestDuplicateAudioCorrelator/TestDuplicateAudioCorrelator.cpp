// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DuplicateAudioCorrelator.h"

#include <opus.h>

#include <QObject>
#include <QtTest>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {
constexpr int SAMPLE_RATE_HZ  = 48000;
constexpr std::size_t FRAME_SAMPLES = 960; // 20ms @ 48kHz mono

std::vector< float > generateTone(std::size_t sampleCount, double freqHzA, double freqHzB) {
	std::vector< float > samples(sampleCount);
	for (std::size_t i = 0; i < sampleCount; ++i) {
		double t  = static_cast< double >(i) / SAMPLE_RATE_HZ;
		samples[i] = static_cast< float >(0.3 * std::sin(2.0 * M_PI * freqHzA * t)
										   + 0.3 * std::sin(2.0 * M_PI * freqHzB * t));
	}
	return samples;
}

std::vector< float > generateNoise(std::size_t sampleCount, unsigned int seed) {
	std::mt19937 rng(seed);
	std::uniform_real_distribution< float > dist(-0.3f, 0.3f);
	std::vector< float > samples(sampleCount);
	for (std::size_t i = 0; i < sampleCount; ++i) {
		samples[i] = dist(rng);
	}
	return samples;
}
} // namespace

class TestDuplicateAudioCorrelator : public QObject {
	Q_OBJECT

private:
	// Encodes pcm as a sequence of 20ms Opus frames and feeds them into the correlator for
	// the given session, starting at startTimestampMs and advancing 20ms per frame. Writes
	// the timestamp of the last submitted frame to outLastTimestampMs. QFAIL expands to a
	// bare `return;`, so this must be void (an out-parameter), not return a value.
	void feedFrames(DuplicateAudioCorrelator &correlator, std::uint32_t sessionID, const std::vector< float > &pcm,
					std::int64_t startTimestampMs, std::int64_t &outLastTimestampMs) {
		outLastTimestampMs = startTimestampMs;

		int err              = 0;
		OpusEncoder *encoder = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
		if (err != OPUS_OK || !encoder) {
			QFAIL("Failed to create Opus encoder for test fixture");
		}

		unsigned char encoded[4000];
		std::int64_t timestamp = startTimestampMs;

		for (std::size_t offset = 0; offset + FRAME_SAMPLES <= pcm.size(); offset += FRAME_SAMPLES) {
			int encodedBytes = opus_encode_float(encoder, pcm.data() + offset, static_cast< int >(FRAME_SAMPLES),
												  encoded, sizeof(encoded));
			if (encodedBytes <= 0) {
				opus_encoder_destroy(encoder);
				QFAIL("Failed to encode synthetic test frame");
			}

			correlator.submitPacket(sessionID, Mumble::Protocol::AudioCodec::Opus, encoded,
									 static_cast< std::size_t >(encodedBytes), timestamp);
			outLastTimestampMs = timestamp;
			timestamp += 20;
		}

		opus_encoder_destroy(encoder);
	}

private slots:
	void test_sameSourceWithSmallLagIsConfirmed() {
		DuplicateAudioCorrelator correlator;

		const std::size_t lagSamples   = 240; // 5ms -- plausible mic-distance/jitter offset
		const std::size_t totalSamples = 19200; // 400ms, exact multiple of FRAME_SAMPLES

		std::vector< float > fullSignal = generateTone(totalSamples + lagSamples, 220.0, 440.0);
		std::vector< float > signalA(fullSignal.begin(), fullSignal.begin() + totalSamples);
		std::vector< float > signalB(fullSignal.begin() + lagSamples, fullSignal.begin() + lagSamples + totalSamples);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signalA, 0, tsA);
		feedFrames(correlator, 2, signalB, 0, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() >= DuplicateAudioCorrelator::CORRELATION_THRESHOLD);
	}

	void test_differentSpeakersAreNotConfirmed() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200; // 400ms

		std::vector< float > signalA = generateTone(totalSamples, 220.0, 440.0);
		std::vector< float > signalB = generateNoise(totalSamples, 12345);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signalA, 0, tsA);
		feedFrames(correlator, 2, signalB, 0, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(!correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() < DuplicateAudioCorrelator::CORRELATION_THRESHOLD);
	}

	void test_notEnoughDataFailsOpen() {
		DuplicateAudioCorrelator correlator;
		QVERIFY(!correlator.isLikelySameSource(1, 2, 0));
	}

	void test_forgetSessionReleasesState() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200;
		std::vector< float > signal    = generateTone(totalSamples, 220.0, 440.0);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signal, 0, tsA);
		feedFrames(correlator, 2, signal, 0, tsB);
		std::int64_t now = std::max(tsA, tsB);

		QVERIFY(correlator.isLikelySameSource(1, 2, now));

		correlator.forgetSession(1);
		QVERIFY(!correlator.isLikelySameSource(1, 2, now));
	}

	void test_staleHistoryIsNotConfirmed() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200;
		std::vector< float > signal    = generateTone(totalSamples, 220.0, 440.0);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signal, 0, tsA);
		feedFrames(correlator, 2, signal, 0, tsB);

		std::int64_t now = std::max(tsA, tsB) + 1000; // well beyond MAX_HISTORY_AGE_MS (250ms)
		QVERIFY(!correlator.isLikelySameSource(1, 2, now));
	}
};

QTEST_MAIN(TestDuplicateAudioCorrelator)
#include "TestDuplicateAudioCorrelator.moc"
