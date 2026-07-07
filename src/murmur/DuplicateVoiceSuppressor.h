// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MURMUR_DUPLICATEVOICESUPPRESSOR_H_
#define MUMBLE_MURMUR_DUPLICATEVOICESUPPRESSOR_H_

#include "IAcousticOracle.h"
#include "MumbleProtocol.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class DuplicateVoiceSuppressor {
public:
	enum class Decision { Forward, Suppress, NoDecision };

	struct PacketMetadata {
		std::uint32_t sessionID = 0;
		unsigned int channelID  = 0;
		std::int64_t timestampMilliseconds = 0;
		Mumble::Protocol::AudioCodec codec = Mumble::Protocol::AudioCodec::Opus;
		std::size_t payloadSize            = 0;
		std::uint64_t frameNumber          = 0;
		bool isPrioritySpeaker             = false;
		bool isWhisperOrDirect             = false;
		bool isTerminator                  = false;
		// Optional; only required when acoustic confirmation is enabled. May be nullptr
		// (e.g. existing metadata-only tests) -- guarded by acoustic-path code, never
		// dereferenced otherwise.
		const unsigned char *payloadData = nullptr;
	};

	struct DecisionDetails {
		unsigned int channelID = 0;
		std::vector< std::uint32_t > candidateSessions;
		std::uint32_t keptSession       = 0;
		std::uint32_t suppressedSession = 0;
		double keptScore                = 0.0;
		double suppressedScore          = 0.0;
		// Gate 1 (ssvr_duplicate_voice_suppression): true whenever the metadata heuristic
		// flagged suppressedSession as a candidate duplicate of keptSession this frame.
		bool metadataGateTriggered = false;
		// Gate 2 (ssvr_duplicate_voice_suppression_acoustic): whether it was consulted this
		// frame, and if so, whether it confirmed (vs. vetoed) gate 1's candidate.
		bool acousticGateRan       = false;
		bool acousticGateConfirmed = false;
		// Meaningful only when acousticGateRan is true; -1.0 otherwise.
		double correlationScore = -1.0;
		std::string reason;
	};

	struct Result {
		Decision decision = Decision::NoDecision;
		DecisionDetails details;
		bool hasDetails = false;
		// True whenever this packet's session overlaps in time (within OVERLAP_WINDOW_MS,
		// same channel) with at least one other session's recent packet -- regardless of
		// whether gate 1 (metadata heuristic) or gate 2 (acoustic) ever reach a mute/veto
		// decision. Fires far earlier and more cheaply than hasDetails; useful to confirm
		// concurrent audio is actually reaching the server from two sessions at all, before
		// diagnosing why a mute/veto decision isn't (or is) being made.
		bool overlapDetected = false;
		unsigned int overlapChannelID = 0;
		std::vector< std::uint32_t > overlappingSessions;
	};

	Result shouldForwardVoicePacket(const PacketMetadata &metadata);
	void clear();
	void forgetSession(std::uint32_t sessionID);

	// Non-owning; the caller (Server) owns the real oracle and must outlive its use here.
	// Pass nullptr to disable (default). See setAcousticConfirmationEnabled() -- acoustic
	// confirmation is only consulted when both an oracle is set AND enabled is true, so
	// Phase 1 behavior is unaffected unless both are explicitly turned on.
	void setAcousticOracle(IAcousticOracle *oracle);
	void setAcousticConfirmationEnabled(bool enabled);

private:
	struct Activity {
		std::uint32_t sessionID = 0;
		unsigned int channelID  = 0;
		std::int64_t timestampMilliseconds = 0;
		Mumble::Protocol::AudioCodec codec = Mumble::Protocol::AudioCodec::Opus;
		std::size_t payloadSize            = 0;
		std::uint64_t frameNumber          = 0;
		double score                       = 0.0;
		unsigned int continuityFrames      = 0;
		bool isPrioritySpeaker             = false;
	};

	struct SessionState {
		std::uint32_t candidatePrimarySession = 0;
		unsigned int consecutiveWeakFrames    = 0;
		unsigned int releaseFrames            = 0;
		bool suppressed                       = false;
	};

	static constexpr std::int64_t OVERLAP_WINDOW_MS = 120;
	static constexpr std::int64_t ACOUSTIC_CAPTURE_WINDOW_MS = 250;
	static constexpr unsigned int REQUIRED_WEAK_FRAMES = 3;
	static constexpr unsigned int REQUIRED_RELEASE_FRAMES = 2;
	// Gate 1 only needs to flag a *candidate* duplicate pair -- when acoustic confirmation
	// (gate 2) is enabled, it makes the real same-source-or-not call. Real duplicate mic
	// pickup (two mics close together capturing the same voice) tends to produce very
	// SIMILAR payload sizes between the two streams, since both captures are near-identical
	// quality -- the original 1.25x/8-byte thresholds below were tuned as if "stronger"
	// meant "clearly the real speaker" on their own, which made gate 1 too strict to ever
	// flag the closest, cleanest duplicate-mic cases (field-tested: two headsets ~15cm
	// apart producing zero candidates at the old thresholds). Loosened so near-tied streams
	// still produce a candidate; gate 2 is what actually protects against false positives
	// when it's enabled.
	static constexpr double MIN_STRONGER_RATIO = 1.05;
	static constexpr double MIN_STRONGER_SCORE_DELTA = 2.0;

	std::unordered_map< unsigned int, std::unordered_map< std::uint32_t, Activity > > m_activityByChannel;
	std::unordered_map< std::uint32_t, SessionState > m_sessionStates;
	std::unordered_map< std::uint32_t, std::int64_t > m_acousticCaptureUntilBySession;

	std::mutex m_mutex;
	IAcousticOracle *m_acousticOracle          = nullptr;
	bool m_acousticConfirmationEnabled = false;

	void pruneChannel(unsigned int channelID, std::int64_t nowMilliseconds);
	void pruneAcousticCaptureWindows(std::int64_t nowMilliseconds);
	void updateActivity(const PacketMetadata &metadata, double score, unsigned int continuityFrames);
	void removeActivity(const PacketMetadata &metadata);
	double scorePacket(const PacketMetadata &metadata, const Activity *previousActivity,
					   unsigned int &continuityFrames) const;
	static bool isClearlyStronger(double strongerScore, double weakerScore);
	static const char *codecName(Mumble::Protocol::AudioCodec codec);
};

#endif // MUMBLE_MURMUR_DUPLICATEVOICESUPPRESSOR_H_
