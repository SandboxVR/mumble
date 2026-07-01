// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DuplicateVoiceSuppressor.h"

#include <algorithm>
#include <cmath>
#include <sstream>

constexpr std::int64_t DuplicateVoiceSuppressor::OVERLAP_WINDOW_MS;
constexpr unsigned int DuplicateVoiceSuppressor::REQUIRED_WEAK_FRAMES;
constexpr unsigned int DuplicateVoiceSuppressor::REQUIRED_RELEASE_FRAMES;
constexpr double DuplicateVoiceSuppressor::MIN_STRONGER_RATIO;
constexpr double DuplicateVoiceSuppressor::MIN_STRONGER_SCORE_DELTA;

void DuplicateVoiceSuppressor::clear() {
	m_activityByChannel.clear();
	m_sessionStates.clear();
}

void DuplicateVoiceSuppressor::setAcousticOracle(IAcousticOracle *oracle) {
	m_acousticOracle = oracle;
}

void DuplicateVoiceSuppressor::setAcousticConfirmationEnabled(bool enabled) {
	m_acousticConfirmationEnabled = enabled;
}

DuplicateVoiceSuppressor::Result DuplicateVoiceSuppressor::shouldForwardVoicePacket(const PacketMetadata &metadata) {
	Result result;

	if (metadata.isWhisperOrDirect) {
		return result;
	}

	if (metadata.isTerminator || metadata.payloadSize == 0) {
		removeActivity(metadata);
		return result;
	}

	pruneChannel(metadata.channelID, metadata.timestampMilliseconds);

	auto &channelActivity = m_activityByChannel[metadata.channelID];
	const Activity *previousActivity = nullptr;
	auto previousIt = channelActivity.find(metadata.sessionID);
	if (previousIt != channelActivity.end()) {
		previousActivity = &previousIt->second;
	}

	unsigned int continuityFrames = 1;
	const double currentScore     = scorePacket(metadata, previousActivity, continuityFrames);

	std::vector< const Activity * > overlappingActivities;
	overlappingActivities.reserve(channelActivity.size());
	for (const auto &entry : channelActivity) {
		const Activity &activity = entry.second;
		if (activity.sessionID == metadata.sessionID) {
			continue;
		}

		const std::int64_t age = metadata.timestampMilliseconds - activity.timestampMilliseconds;
		if (age >= 0 && age <= OVERLAP_WINDOW_MS && activity.channelID == metadata.channelID) {
			overlappingActivities.push_back(&activity);
		}
	}

	if (!overlappingActivities.empty()) {
		result.overlapDetected  = true;
		result.overlapChannelID = metadata.channelID;
		result.overlappingSessions.push_back(metadata.sessionID);
		for (const Activity *activity : overlappingActivities) {
			result.overlappingSessions.push_back(activity->sessionID);
		}
		std::sort(result.overlappingSessions.begin(), result.overlappingSessions.end());
		result.overlappingSessions.erase(
			std::unique(result.overlappingSessions.begin(), result.overlappingSessions.end()),
			result.overlappingSessions.end());
	}

	SessionState &sessionState = m_sessionStates[metadata.sessionID];

	if (!overlappingActivities.empty() && !metadata.isPrioritySpeaker) {
		if (m_acousticConfirmationEnabled && m_acousticOracle != nullptr && metadata.payloadData != nullptr) {
			// Gated: only sessions currently part of an overlap candidate pair get decoded,
			// never all traffic. Both sessions in an overlapping pair independently reach
			// this branch on their own turn through this function, so this single call
			// (for the "current" packet's session) is sufficient to keep both sides fed.
			m_acousticOracle->submitPacket(metadata.sessionID, metadata.codec, metadata.payloadData,
											metadata.payloadSize, metadata.timestampMilliseconds);
		}

		const Activity *bestActivity =
			*std::max_element(overlappingActivities.begin(), overlappingActivities.end(),
							  [](const Activity *lhs, const Activity *rhs) { return lhs->score < rhs->score; });

		const bool sameCodec = bestActivity->codec == metadata.codec;
		const bool stronger  = bestActivity->isPrioritySpeaker || (sameCodec && isClearlyStronger(bestActivity->score, currentScore));

		bool acousticConfirmed     = false;
		bool acousticCheckRan      = false;
		bool metadataGateTriggered = false;

		if (stronger) {
			if (sessionState.candidatePrimarySession == bestActivity->sessionID) {
				sessionState.consecutiveWeakFrames++;
			} else {
				sessionState.candidatePrimarySession = bestActivity->sessionID;
				sessionState.consecutiveWeakFrames    = 1;
				sessionState.suppressed               = false;
			}
			sessionState.releaseFrames = 0;

			if (sessionState.consecutiveWeakFrames >= REQUIRED_WEAK_FRAMES) {
				// Gate 1 (ssvr_duplicate_voice_suppression): the metadata heuristic has
				// flagged metadata.sessionID as a candidate duplicate of bestActivity this
				// frame. Acoustic confirmation (gate 2) is only *consulted* at this exact
				// point, not every frame. When acoustic mode is off (or no oracle is wired),
				// acousticConfirmed defaults to true, preserving Phase 1 behavior exactly.
				// When gate 2 disagrees (vetoes), we intentionally do not suppress and do not
				// reset consecutiveWeakFrames, so a transient decode hiccup on one frame
				// doesn't discard an otherwise-valid streak -- but we still report the veto
				// below so it's visible in real time.
				metadataGateTriggered = true;
				acousticConfirmed     = true;
				if (m_acousticConfirmationEnabled && m_acousticOracle != nullptr) {
					acousticCheckRan  = true;
					acousticConfirmed = m_acousticOracle->isLikelySameSource(
						bestActivity->sessionID, metadata.sessionID, metadata.timestampMilliseconds);
				}

				if (acousticConfirmed) {
					sessionState.suppressed = true;
					result.decision         = Decision::Suppress;
				}
			}
		} else if (sessionState.suppressed && sessionState.candidatePrimarySession != 0) {
			sessionState.releaseFrames++;
			if (sessionState.releaseFrames < REQUIRED_RELEASE_FRAMES) {
				result.decision = Decision::Suppress;
			} else {
				sessionState = SessionState();
			}
		} else {
			sessionState = SessionState();
		}

		// Report whenever gate 1 actively muted/continued-muting a stream (Suppress) OR gate
		// 1 wanted to mute this frame but gate 2 vetoed it (metadataGateTriggered without
		// Suppress) -- both are real-time-interesting events for operators watching the log.
		if (result.decision == Decision::Suppress || metadataGateTriggered) {
			result.hasDetails                 = true;
			result.details.channelID          = metadata.channelID;
			result.details.keptSession        = bestActivity->sessionID;
			result.details.suppressedSession  = metadata.sessionID;
			result.details.keptScore          = bestActivity->score;
			result.details.suppressedScore    = currentScore;
			result.details.candidateSessions  = { metadata.sessionID };
			for (const Activity *activity : overlappingActivities) {
				result.details.candidateSessions.push_back(activity->sessionID);
			}
			std::sort(result.details.candidateSessions.begin(), result.details.candidateSessions.end());
			result.details.candidateSessions.erase(
				std::unique(result.details.candidateSessions.begin(), result.details.candidateSessions.end()),
				result.details.candidateSessions.end());

			result.details.metadataGateTriggered = metadataGateTriggered;
			result.details.acousticGateRan       = acousticCheckRan;
			result.details.acousticGateConfirmed = acousticConfirmed;
			result.details.correlationScore = acousticCheckRan ? m_acousticOracle->lastCorrelationScore() : -1.0;

			// Gate 1/gate 2 outcome and correlation score are exposed as structured fields
			// above (metadataGateTriggered/acousticGateRan/acousticGateConfirmed/
			// correlationScore) for callers to log clearly; keep this string to the
			// heuristic-specific diagnostic detail that isn't otherwise exposed.
			std::ostringstream reason;
			reason << "overlap_window_ms=" << OVERLAP_WINDOW_MS << " codec=" << codecName(metadata.codec)
				   << " consecutive_weaker_frames=" << sessionState.consecutiveWeakFrames
				   << " payload_size=" << metadata.payloadSize;
			if (bestActivity->isPrioritySpeaker) {
				reason << " kept_is_priority_speaker=true";
			}
			result.details.reason = reason.str();
		}
	} else {
		if (sessionState.suppressed) {
			sessionState.releaseFrames++;
			if (sessionState.releaseFrames >= REQUIRED_RELEASE_FRAMES) {
				sessionState = SessionState();
			}
		} else {
			sessionState = SessionState();
		}
	}

	updateActivity(metadata, currentScore, continuityFrames);
	return result;
}

void DuplicateVoiceSuppressor::pruneChannel(unsigned int channelID, std::int64_t nowMilliseconds) {
	auto channelIt = m_activityByChannel.find(channelID);
	if (channelIt == m_activityByChannel.end()) {
		return;
	}

	auto &channelActivity = channelIt->second;
	for (auto it = channelActivity.begin(); it != channelActivity.end();) {
		if (nowMilliseconds - it->second.timestampMilliseconds > OVERLAP_WINDOW_MS) {
			it = channelActivity.erase(it);
		} else {
			++it;
		}
	}

	if (channelActivity.empty()) {
		m_activityByChannel.erase(channelIt);
	}
}

void DuplicateVoiceSuppressor::updateActivity(const PacketMetadata &metadata, double score,
											  unsigned int continuityFrames) {
	Activity activity;
	activity.sessionID             = metadata.sessionID;
	activity.channelID              = metadata.channelID;
	activity.timestampMilliseconds  = metadata.timestampMilliseconds;
	activity.codec                  = metadata.codec;
	activity.payloadSize            = metadata.payloadSize;
	activity.frameNumber            = metadata.frameNumber;
	activity.score                  = score;
	activity.continuityFrames       = continuityFrames;
	activity.isPrioritySpeaker      = metadata.isPrioritySpeaker;

	m_activityByChannel[metadata.channelID][metadata.sessionID] = activity;
}

void DuplicateVoiceSuppressor::removeActivity(const PacketMetadata &metadata) {
	auto channelIt = m_activityByChannel.find(metadata.channelID);
	if (channelIt != m_activityByChannel.end()) {
		channelIt->second.erase(metadata.sessionID);
		if (channelIt->second.empty()) {
			m_activityByChannel.erase(channelIt);
		}
	}
	m_sessionStates.erase(metadata.sessionID);
}

double DuplicateVoiceSuppressor::scorePacket(const PacketMetadata &metadata, const Activity *previousActivity,
											 unsigned int &continuityFrames) const {
	continuityFrames = 1;
	if (previousActivity) {
		const bool frameContinuity =
			metadata.frameNumber > previousActivity->frameNumber && metadata.frameNumber - previousActivity->frameNumber <= 3;
		const bool timeContinuity =
			metadata.timestampMilliseconds >= previousActivity->timestampMilliseconds
			&& metadata.timestampMilliseconds - previousActivity->timestampMilliseconds <= OVERLAP_WINDOW_MS;

		if (frameContinuity || timeContinuity) {
			continuityFrames = std::min(previousActivity->continuityFrames + 1, 8u);
		}
	}

	// This first pass intentionally does not Opus-decode on Murmur. When PCM is not already available,
	// payload size and stream continuity are only a conservative proxy for likely primary speech energy.
	double score = static_cast< double >(metadata.payloadSize) + static_cast< double >(continuityFrames * 4);
	if (metadata.isPrioritySpeaker) {
		score += 100000.0;
	}

	return score;
}

bool DuplicateVoiceSuppressor::isClearlyStronger(double strongerScore, double weakerScore) {
	return strongerScore >= weakerScore * MIN_STRONGER_RATIO
		   && strongerScore - weakerScore >= MIN_STRONGER_SCORE_DELTA;
}

const char *DuplicateVoiceSuppressor::codecName(Mumble::Protocol::AudioCodec codec) {
	switch (codec) {
		case Mumble::Protocol::AudioCodec::Opus:
			return "opus";
		case Mumble::Protocol::AudioCodec::CELT_Alpha:
			return "celt_alpha";
		case Mumble::Protocol::AudioCodec::CELT_Beta:
			return "celt_beta";
		case Mumble::Protocol::AudioCodec::Speex:
			return "speex";
	}

	return "unknown";
}
