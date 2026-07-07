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
constexpr std::size_t TEN_MS_FRAME_SAMPLES = 480;

std::vector< float > generateSpeechShapedNoise(std::size_t sampleCount, unsigned int seed) {
	std::mt19937 rng(seed);
	std::uniform_real_distribution< float > dist(-1.0f, 1.0f);
	std::uniform_real_distribution< double > phaseDist(0.0, 2.0 * M_PI);
	const double phaseA = phaseDist(rng);
	const double phaseB = phaseDist(rng);

	std::vector< float > samples(sampleCount);
	float filtered = 0.0f;
	for (std::size_t i = 0; i < sampleCount; ++i) {
		const double t = static_cast< double >(i) / SAMPLE_RATE_HZ;
		const double syllable =
			0.35 + 0.35 * std::sin(2.0 * M_PI * 3.1 * t + phaseA)
			+ 0.20 * std::sin(2.0 * M_PI * 6.7 * t + phaseB);
		const double envelope = std::max(0.04, syllable);

		filtered = 0.96f * filtered + 0.04f * dist(rng);
		samples[i] = static_cast< float >(0.6 * envelope * filtered);
	}
	return samples;
}

std::vector< float > applyStaticGainAndGate(const std::vector< float > &samples) {
	std::vector< float > out(samples.size());
	for (std::size_t i = 0; i < samples.size(); ++i) {
		const double t = static_cast< double >(i) / SAMPLE_RATE_HZ;
		const bool gateOpen = std::sin(2.0 * M_PI * 3.1 * t + 0.35) > -0.45;
		const double gain = gateOpen ? 0.42 : 0.06;
		out[i] = static_cast< float >(samples[i] * gain);
	}
	return out;
}
} // namespace

class TestDuplicateAudioCorrelator : public QObject {
	Q_OBJECT

private:
	// Encodes pcm as a sequence of Opus frames and feeds them into the correlator for
	// the given session, starting at startTimestampMs and advancing by frame duration. Writes
	// the timestamp of the last submitted frame to outLastTimestampMs. QFAIL expands to a
	// bare `return;`, so this must be void (an out-parameter), not return a value.
	void feedFrames(DuplicateAudioCorrelator &correlator, std::uint32_t sessionID, const std::vector< float > &pcm,
					std::int64_t startTimestampMs, std::int64_t &outLastTimestampMs,
					std::size_t frameSamples = FRAME_SAMPLES) {
		outLastTimestampMs = startTimestampMs;

		int err              = 0;
		OpusEncoder *encoder = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
		if (err != OPUS_OK || !encoder) {
			QFAIL("Failed to create Opus encoder for test fixture");
		}

		unsigned char encoded[4000];
		const std::int64_t timestampStepMs =
			static_cast< std::int64_t >(frameSamples * 1000 / SAMPLE_RATE_HZ);
		std::int64_t timestamp = startTimestampMs + timestampStepMs;

		for (std::size_t offset = 0; offset + frameSamples <= pcm.size(); offset += frameSamples) {
			int encodedBytes = opus_encode_float(encoder, pcm.data() + offset, static_cast< int >(frameSamples),
												  encoded, sizeof(encoded));
			if (encodedBytes <= 0) {
				opus_encoder_destroy(encoder);
				QFAIL("Failed to encode synthetic test frame");
			}

			correlator.submitPacket(sessionID, Mumble::Protocol::AudioCodec::Opus, encoded,
									 static_cast< std::size_t >(encodedBytes), timestamp);
			outLastTimestampMs = timestamp;
			timestamp += timestampStepMs;
		}

		opus_encoder_destroy(encoder);
	}

private slots:
	void test_sameSourceWithSmallLagIsConfirmed() {
		DuplicateAudioCorrelator correlator;

		const std::size_t lagSamples   = 240; // 5ms -- plausible mic-distance/jitter offset
		const std::size_t totalSamples = 19200; // 400ms, exact multiple of FRAME_SAMPLES

		std::vector< float > fullSignal = generateSpeechShapedNoise(totalSamples + lagSamples, 7);
		std::vector< float > signalA(fullSignal.begin(), fullSignal.begin() + totalSamples);
		std::vector< float > signalB(fullSignal.begin() + lagSamples, fullSignal.begin() + lagSamples + totalSamples);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signalA, 0, tsA);
		feedFrames(correlator, 2, signalB, 0, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() >= DuplicateAudioCorrelator::DEFAULT_CORRELATION_THRESHOLD);
	}

	void test_configurableCorrelationThresholdCanVetoMatch() {
		DuplicateAudioCorrelator correlator;
		DuplicateAudioCorrelator::Config config;
		config.correlationThreshold = 2.0;
		config.verdictCacheMilliseconds = 0;
		correlator.setConfig(config);

		const std::size_t totalSamples = 19200;
		std::vector< float > signal = generateSpeechShapedNoise(totalSamples, 9);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signal, 0, tsA);
		feedFrames(correlator, 2, signal, 0, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(!correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() < config.correlationThreshold);
		QCOMPARE(correlator.config().verdictCacheMilliseconds, static_cast< std::int64_t >(0));
	}

	void test_differentSpeakersAreNotConfirmed() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200; // 400ms

		std::vector< float > signalA = generateSpeechShapedNoise(totalSamples, 11);
		std::vector< float > signalB = generateSpeechShapedNoise(totalSamples, 12345);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signalA, 0, tsA);
		feedFrames(correlator, 2, signalB, 0, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(!correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() < DuplicateAudioCorrelator::DEFAULT_CORRELATION_THRESHOLD);
	}

	void test_unequalHistoryLengthsAreEndAligned() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200; // 400ms
		const std::size_t shorterSamples = 12480; // 260ms
		std::vector< float > signal = generateSpeechShapedNoise(totalSamples, 21);
		std::vector< float > signalTail(signal.end() - shorterSamples, signal.end());

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signal, 0, tsA);
		feedFrames(correlator, 2, signalTail, 140, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() >= DuplicateAudioCorrelator::DEFAULT_CORRELATION_THRESHOLD);
	}

	void test_arrivalTimeSkewBeyondOldWaveformLagIsConfirmed() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200; // 400ms
		const std::size_t skewSamples = SAMPLE_RATE_HZ * 40 / 1000;
		std::vector< float > fullSignal = generateSpeechShapedNoise(totalSamples + skewSamples, 31);
		std::vector< float > signalA(fullSignal.begin(), fullSignal.begin() + totalSamples);
		std::vector< float > signalB(fullSignal.begin() + skewSamples, fullSignal.begin() + skewSamples + totalSamples);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signalA, 0, tsA);
		feedFrames(correlator, 2, signalB, 40, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() >= DuplicateAudioCorrelator::DEFAULT_CORRELATION_THRESHOLD);
	}

	void test_staticGainAndTimeVaryingGateStillConfirm() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200; // 400ms
		const std::size_t skewSamples = SAMPLE_RATE_HZ * 20 / 1000;
		std::vector< float > fullSignal = generateSpeechShapedNoise(totalSamples + skewSamples, 41);
		std::vector< float > signalA(fullSignal.begin(), fullSignal.begin() + totalSamples);
		std::vector< float > signalB(fullSignal.begin() + skewSamples, fullSignal.begin() + skewSamples + totalSamples);
		signalB = applyStaticGainAndGate(signalB);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signalA, 0, tsA);
		feedFrames(correlator, 2, signalB, 20, tsB);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() >= DuplicateAudioCorrelator::DEFAULT_CORRELATION_THRESHOLD);
	}

	void test_differentFrameSizesStillConfirm() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200; // 400ms
		std::vector< float > signal = generateSpeechShapedNoise(totalSamples, 51);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signal, 0, tsA, FRAME_SAMPLES);
		feedFrames(correlator, 2, signal, 0, tsB, TEN_MS_FRAME_SAMPLES);

		std::int64_t now = std::max(tsA, tsB);
		QVERIFY(correlator.isLikelySameSource(1, 2, now));
		QVERIFY(correlator.lastCorrelationScore() >= DuplicateAudioCorrelator::DEFAULT_CORRELATION_THRESHOLD);
	}

	void test_notEnoughDataFailsOpen() {
		DuplicateAudioCorrelator correlator;
		QVERIFY(!correlator.isLikelySameSource(1, 2, 0));
	}

	void test_forgetSessionReleasesState() {
		DuplicateAudioCorrelator correlator;

		const std::size_t totalSamples = 19200;
		std::vector< float > signal    = generateSpeechShapedNoise(totalSamples, 61);

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
		std::vector< float > signal    = generateSpeechShapedNoise(totalSamples, 71);

		std::int64_t tsA = 0, tsB = 0;
		feedFrames(correlator, 1, signal, 0, tsA);
		feedFrames(correlator, 2, signal, 0, tsB);

		std::int64_t now = std::max(tsA, tsB) + 1000; // well beyond MAX_HISTORY_AGE_MS (250ms)
		QVERIFY(!correlator.isLikelySameSource(1, 2, now));
	}
};

QTEST_MAIN(TestDuplicateAudioCorrelator)
#include "TestDuplicateAudioCorrelator.moc"
