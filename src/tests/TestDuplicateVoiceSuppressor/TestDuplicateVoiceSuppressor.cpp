// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DuplicateVoiceSuppressor.h"

#include <atomic>
#include <thread>
#include <vector>

#include <QObject>
#include <QtTest>

// Fake IAcousticOracle used to test DuplicateVoiceSuppressor's gating logic without
// linking Opus. See docs/dev/DuplicateVoiceSuppressionPlan.md Phase 2.
class FakeAcousticOracle : public IAcousticOracle {
public:
	bool shouldConfirm               = true;
	int submitCount                  = 0;
	int isLikelySameSourceCallCount = 0;
	std::vector< std::uint32_t > submittedSessions;

	bool isLikelySameSource(std::uint32_t, std::uint32_t, std::int64_t) override {
		++isLikelySameSourceCallCount;
		return shouldConfirm;
	}
	void submitPacket(std::uint32_t, Mumble::Protocol::AudioCodec, const unsigned char *, std::size_t,
					   std::int64_t) override;
	void forgetSession(std::uint32_t) override {}
	double lastCorrelationScore() const override { return shouldConfirm ? 1.0 : 0.0; }
};

void FakeAcousticOracle::submitPacket(std::uint32_t sessionID, Mumble::Protocol::AudioCodec,
									   const unsigned char *, std::size_t, std::int64_t) {
	++submitCount;
	submittedSessions.push_back(sessionID);
}

class TestDuplicateVoiceSuppressor : public QObject {
	Q_OBJECT

private:
	using Decision = DuplicateVoiceSuppressor::Decision;

	static const unsigned char *dummyPayload() {
		static const unsigned char data[] = { 0 };
		return data;
	}

	static DuplicateVoiceSuppressor::PacketMetadata packet(std::uint32_t sessionID, unsigned int channelID,
														   std::int64_t timestampMilliseconds, std::uint64_t frameNumber,
														   std::size_t payloadSize) {
		DuplicateVoiceSuppressor::PacketMetadata metadata;
		metadata.sessionID             = sessionID;
		metadata.channelID              = channelID;
		metadata.timestampMilliseconds  = timestampMilliseconds;
		metadata.codec                  = Mumble::Protocol::AudioCodec::Opus;
		metadata.payloadSize            = payloadSize;
		metadata.frameNumber            = frameNumber;
		metadata.payloadData            = dummyPayload();
		return metadata;
	}

private slots:
	void test_isolatedPacketsAreForwardedByDefault() {
		DuplicateVoiceSuppressor suppressor;

		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 40)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 300, 0, 20)).decision),
				 static_cast< int >(Decision::NoDecision));
	}

	void test_weakerOverlappingStreamIsSuppressedAfterHysteresis() {
		DuplicateVoiceSuppressor suppressor;

		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 20, 1, 102)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 30, 1, 34)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 40, 2, 101)).decision),
				 static_cast< int >(Decision::NoDecision));

		DuplicateVoiceSuppressor::Result result = suppressor.shouldForwardVoicePacket(packet(2, 7, 50, 2, 36));
		QCOMPARE(static_cast< int >(result.decision), static_cast< int >(Decision::Suppress));
		QVERIFY(result.hasDetails);
		QCOMPARE(result.details.keptSession, static_cast< std::uint32_t >(1));
		QCOMPARE(result.details.suppressedSession, static_cast< std::uint32_t >(2));
	}

	void test_configurableWeakFrameThresholdSuppressesEarlier() {
		DuplicateVoiceSuppressor suppressor;
		DuplicateVoiceSuppressor::Config config;
		config.requiredWeakFrames = 1;
		suppressor.setConfig(config);

		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100)).decision),
				 static_cast< int >(Decision::NoDecision));

		DuplicateVoiceSuppressor::Result result = suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		QCOMPARE(static_cast< int >(result.decision), static_cast< int >(Decision::Suppress));
		QVERIFY(result.hasDetails);
	}

	void test_configurableOverlapWindowPreventsCandidate() {
		DuplicateVoiceSuppressor suppressor;
		DuplicateVoiceSuppressor::Config config;
		config.overlapWindowMilliseconds = 5;
		suppressor.setConfig(config);

		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100)).decision),
				 static_cast< int >(Decision::NoDecision));

		DuplicateVoiceSuppressor::Result result = suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		QCOMPARE(static_cast< int >(result.decision), static_cast< int >(Decision::NoDecision));
		QVERIFY(!result.hasDetails);
		QVERIFY(!result.overlapDetected);
	}

	void test_releaseCompleteReportsUnmuteDetails() {
		DuplicateVoiceSuppressor suppressor;

		suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 20, 1, 102));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 30, 1, 34));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 40, 2, 101));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 50, 2, 36)).decision),
				 static_cast< int >(Decision::Suppress));

		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 60, 3, 200)).decision),
				 static_cast< int >(Decision::Suppress));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 70, 4, 200)).decision),
				 static_cast< int >(Decision::Suppress));
		DuplicateVoiceSuppressor::Result released = suppressor.shouldForwardVoicePacket(packet(2, 7, 80, 5, 200));
		QCOMPARE(static_cast< int >(released.decision), static_cast< int >(Decision::Forward));
		QVERIFY(released.hasDetails);
		QVERIFY(released.details.releaseComplete);
		QCOMPARE(released.details.keptSession, static_cast< std::uint32_t >(1));
		QCOMPARE(released.details.suppressedSession, static_cast< std::uint32_t >(2));
	}

	void test_differentChannelsDoNotSuppress() {
		DuplicateVoiceSuppressor suppressor;

		for (std::uint64_t frame = 0; frame < 5; ++frame) {
			QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, frame * 20, frame, 100)).decision),
					 static_cast< int >(Decision::NoDecision));
			QCOMPARE(static_cast< int >(
						 suppressor.shouldForwardVoicePacket(packet(2, 8, frame * 20 + 10, frame, 30)).decision),
					 static_cast< int >(Decision::NoDecision));
		}
	}

	void test_prioritySpeakerIsNeverSuppressed() {
		DuplicateVoiceSuppressor suppressor;

		for (std::uint64_t frame = 0; frame < 5; ++frame) {
			suppressor.shouldForwardVoicePacket(packet(1, 7, frame * 20, frame, 100));

			DuplicateVoiceSuppressor::PacketMetadata priority = packet(2, 7, frame * 20 + 10, frame, 20);
			priority.isPrioritySpeaker                       = true;
			QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(priority).decision),
					 static_cast< int >(Decision::NoDecision));
		}
	}

	void test_prioritySpeakerCanCauseNonPriorityDuplicateSuppression() {
		DuplicateVoiceSuppressor suppressor;

		for (std::uint64_t frame = 0; frame < 3; ++frame) {
			DuplicateVoiceSuppressor::PacketMetadata priority = packet(1, 7, frame * 20, frame, 20);
			priority.isPrioritySpeaker                       = true;
			QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(priority).decision),
					 static_cast< int >(Decision::NoDecision));

			const Decision expected = frame < 2 ? Decision::NoDecision : Decision::Suppress;
			QCOMPARE(static_cast< int >(
						 suppressor.shouldForwardVoicePacket(packet(2, 7, frame * 20 + 10, frame, 90)).decision),
					 static_cast< int >(expected));
		}
	}

	void test_whisperOrDirectPacketsBypassSuppression() {
		DuplicateVoiceSuppressor suppressor;

		for (std::uint64_t frame = 0; frame < 5; ++frame) {
			QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, frame * 20, frame, 100)).decision),
					 static_cast< int >(Decision::NoDecision));

			DuplicateVoiceSuppressor::PacketMetadata whisper = packet(2, 7, frame * 20 + 10, frame, 25);
			whisper.isWhisperOrDirect                       = true;
			QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(whisper).decision),
					 static_cast< int >(Decision::NoDecision));
		}
	}

	// Same sequence as test_weakerOverlappingStreamIsSuppressedAfterHysteresis, replayed
	// with no oracle wired at all -- confirms acoustic confirmation being unused leaves
	// Phase 1 behavior byte-for-byte unchanged.
	void test_acousticConfirmationDisabledByDefaultMatchesPhase1() {
		DuplicateVoiceSuppressor suppressor;

		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 20, 1, 102)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(2, 7, 30, 1, 34)).decision),
				 static_cast< int >(Decision::NoDecision));
		QCOMPARE(static_cast< int >(suppressor.shouldForwardVoicePacket(packet(1, 7, 40, 2, 101)).decision),
				 static_cast< int >(Decision::NoDecision));

		DuplicateVoiceSuppressor::Result result = suppressor.shouldForwardVoicePacket(packet(2, 7, 50, 2, 36));
		QCOMPARE(static_cast< int >(result.decision), static_cast< int >(Decision::Suppress));
	}

	void test_acousticConfirmationVetoesSuppressionWhenOracleDisagrees() {
		DuplicateVoiceSuppressor suppressor;
		FakeAcousticOracle oracle;
		oracle.shouldConfirm = false;
		suppressor.setAcousticOracle(&oracle);
		suppressor.setAcousticConfirmationEnabled(true);

		suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 20, 1, 102));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 30, 1, 34));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 40, 2, 101));

		DuplicateVoiceSuppressor::Result result = suppressor.shouldForwardVoicePacket(packet(2, 7, 50, 2, 36));
		QCOMPARE(static_cast< int >(result.decision), static_cast< int >(Decision::NoDecision));
		QVERIFY(oracle.isLikelySameSourceCallCount > 0);

		// Gate 2 vetoing gate 1 must still be reported (for real-time logging) even though
		// the packet itself is not suppressed.
		QVERIFY(result.hasDetails);
		QVERIFY(result.details.metadataGateTriggered);
		QVERIFY(result.details.acousticGateRan);
		QVERIFY(!result.details.acousticGateConfirmed);
		QCOMPARE(result.details.suppressedSession, static_cast< std::uint32_t >(2));
	}

	void test_acousticConfirmationAllowsSuppressionWhenOracleAgrees() {
		DuplicateVoiceSuppressor suppressor;
		FakeAcousticOracle oracle;
		oracle.shouldConfirm = true;
		suppressor.setAcousticOracle(&oracle);
		suppressor.setAcousticConfirmationEnabled(true);

		suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 20, 1, 102));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 30, 1, 34));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 40, 2, 101));

		DuplicateVoiceSuppressor::Result result = suppressor.shouldForwardVoicePacket(packet(2, 7, 50, 2, 36));
		QCOMPARE(static_cast< int >(result.decision), static_cast< int >(Decision::Suppress));
		QVERIFY(oracle.isLikelySameSourceCallCount > 0);
	}

	void test_acousticOracleOnlyConsultedOnCandidateOverlap() {
		DuplicateVoiceSuppressor suppressor;
		FakeAcousticOracle oracle;
		suppressor.setAcousticOracle(&oracle);
		suppressor.setAcousticConfirmationEnabled(true);

		// Isolated, non-overlapping packets (300ms apart, well beyond OVERLAP_WINDOW_MS) --
		// the oracle must not be touched at all for these.
		suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 40));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 300, 0, 20));
		QCOMPARE(oracle.submitCount, 0);
		QCOMPARE(oracle.isLikelySameSourceCallCount, 0);

		// Now feed an overlapping sequence that reaches the weak-frame suppression
		// threshold -- only then should the oracle be consulted.
		suppressor.shouldForwardVoicePacket(packet(1, 7, 1000, 1, 100));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 1010, 1, 35));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 1020, 2, 102));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 1030, 2, 34));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 1040, 3, 101));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 1050, 3, 36));

		QVERIFY(oracle.submitCount > 0);
		QCOMPARE(oracle.isLikelySameSourceCallCount, 1);
	}

	void test_acousticCaptureContinuesBrieflyAfterOverlap() {
		DuplicateVoiceSuppressor suppressor;
		FakeAcousticOracle oracle;
		suppressor.setAcousticOracle(&oracle);
		suppressor.setAcousticConfirmationEnabled(true);

		suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		const int submitCountAfterOverlap = oracle.submitCount;

		suppressor.shouldForwardVoicePacket(packet(1, 7, 200, 1, 100));
		QVERIFY(oracle.submitCount > submitCountAfterOverlap);
		QCOMPARE(oracle.submittedSessions.back(), static_cast< std::uint32_t >(1));

		const int submitCountInsideWindow = oracle.submitCount;
		suppressor.shouldForwardVoicePacket(packet(1, 7, 300, 2, 100));
		QCOMPARE(oracle.submitCount, submitCountInsideWindow);
	}

	void test_vbrJitteredNearTieReachesAcousticCandidate() {
		DuplicateVoiceSuppressor suppressor;
		FakeAcousticOracle oracle;
		oracle.shouldConfirm = false;
		suppressor.setAcousticOracle(&oracle);
		suppressor.setAcousticConfirmationEnabled(true);

		const std::size_t sessionOnePayloads[] = { 118, 94, 116, 96, 112, 98, 114, 95 };
		const std::size_t sessionTwoPayloads[] = { 96, 118, 94, 116, 98, 112, 95, 114 };

		bool reachedCandidate = false;
		for (std::uint64_t frame = 0; frame < 8; ++frame) {
			DuplicateVoiceSuppressor::Result first = suppressor.shouldForwardVoicePacket(
				packet(1, 7, static_cast< std::int64_t >(frame * 20), frame, sessionOnePayloads[frame]));
			DuplicateVoiceSuppressor::Result second = suppressor.shouldForwardVoicePacket(
				packet(2, 7, static_cast< std::int64_t >(frame * 20 + 10), frame, sessionTwoPayloads[frame]));

			reachedCandidate = reachedCandidate || (first.hasDetails && first.details.metadataGateTriggered)
							   || (second.hasDetails && second.details.metadataGateTriggered);
			if (reachedCandidate) {
				break;
			}
		}

		QVERIFY(reachedCandidate);
		QVERIFY(oracle.isLikelySameSourceCallCount > 0);
	}

	void test_nearTieRequiresAcousticConfirmation() {
		DuplicateVoiceSuppressor suppressor;

		for (std::uint64_t frame = 0; frame < 8; ++frame) {
			QCOMPARE(static_cast< int >(
						 suppressor.shouldForwardVoicePacket(packet(1, 7, frame * 20, frame, 100)).decision),
					 static_cast< int >(Decision::NoDecision));
			QCOMPARE(static_cast< int >(
						 suppressor.shouldForwardVoicePacket(packet(2, 7, frame * 20 + 10, frame, 103)).decision),
					 static_cast< int >(Decision::NoDecision));
		}
	}

	void test_forgetSessionClearsSuppressionStateForSessionReuse() {
		DuplicateVoiceSuppressor suppressor;

		suppressor.shouldForwardVoicePacket(packet(1, 7, 0, 0, 100));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 10, 0, 35));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 20, 1, 102));
		suppressor.shouldForwardVoicePacket(packet(2, 7, 30, 1, 34));
		suppressor.shouldForwardVoicePacket(packet(1, 7, 40, 2, 101));

		DuplicateVoiceSuppressor::Result suppressed = suppressor.shouldForwardVoicePacket(packet(2, 7, 50, 2, 36));
		QCOMPARE(static_cast< int >(suppressed.decision), static_cast< int >(Decision::Suppress));

		suppressor.forgetSession(2);

		DuplicateVoiceSuppressor::Result reused = suppressor.shouldForwardVoicePacket(packet(2, 7, 60, 0, 36));
		QCOMPARE(static_cast< int >(reused.decision), static_cast< int >(Decision::NoDecision));
	}

	void test_threadedSmokeDoesNotCorruptSuppressorState() {
		DuplicateVoiceSuppressor suppressor;
		std::atomic< bool > start(false);

		auto hammer = [&suppressor, &start](std::uint32_t sessionID, std::size_t payloadSize,
											std::int64_t timestampOffset) {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}

			for (std::uint64_t frame = 0; frame < 2000; ++frame) {
				suppressor.shouldForwardVoicePacket(
					packet(sessionID, 7, static_cast< std::int64_t >(frame * 20) + timestampOffset, frame, payloadSize));
			}
		};

		std::thread first(hammer, 1, 100, 0);
		std::thread second(hammer, 2, 35, 10);

		start.store(true, std::memory_order_release);
		first.join();
		second.join();

		QVERIFY(true);
	}
};

QTEST_MAIN(TestDuplicateVoiceSuppressor)
#include "TestDuplicateVoiceSuppressor.moc"
