// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DuplicateVoiceSuppressor.h"

#include <QObject>
#include <QtTest>

class TestDuplicateVoiceSuppressor : public QObject {
	Q_OBJECT

private:
	using Decision = DuplicateVoiceSuppressor::Decision;

	DuplicateVoiceSuppressor::PacketMetadata packet(std::uint32_t sessionID, unsigned int channelID,
													std::int64_t timestampMilliseconds, std::uint64_t frameNumber,
													std::size_t payloadSize) const {
		DuplicateVoiceSuppressor::PacketMetadata metadata;
		metadata.sessionID             = sessionID;
		metadata.channelID              = channelID;
		metadata.timestampMilliseconds  = timestampMilliseconds;
		metadata.codec                  = Mumble::Protocol::AudioCodec::Opus;
		metadata.payloadSize            = payloadSize;
		metadata.frameNumber            = frameNumber;
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
};

QTEST_MAIN(TestDuplicateVoiceSuppressor)
#include "TestDuplicateVoiceSuppressor.moc"
