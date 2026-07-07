// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DuplicateVoiceSuppressor.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {
struct AcousticSubmission {
	IAcousticOracle *oracle = nullptr;
	std::uint32_t sessionID = 0;
	Mumble::Protocol::AudioCodec codec = Mumble::Protocol::AudioCodec::Opus;
	const unsigned char *payloadData = nullptr;
	std::size_t payloadSize = 0;
	std::int64_t timestampMilliseconds = 0;
};

struct AcousticCheck {
	IAcousticOracle *oracle = nullptr;
	std::uint32_t keptSession = 0;
	std::uint32_t suppressedSession = 0;
	std::int64_t timestampMilliseconds = 0;
};
} // namespace

constexpr std::int64_t DuplicateVoiceSuppressor::OVERLAP_WINDOW_MS;
constexpr std::int64_t DuplicateVoiceSuppressor::ACOUSTIC_CAPTURE_WINDOW_MS;
constexpr unsigned int DuplicateVoiceSuppressor::REQUIRED_WEAK_FRAMES;
constexpr unsigned int DuplicateVoiceSuppressor::REQUIRED_RELEASE_FRAMES;
constexpr double DuplicateVoiceSuppressor::MIN_STRONGER_RATIO;
constexpr double DuplicateVoiceSuppressor::MIN_STRONGER_SCORE_DELTA;

void DuplicateVoiceSuppressor::clear() {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_activityByChannel.clear();
	m_sessionStates.clear();
	m_acousticCaptureUntilBySession.clear();
}

void DuplicateVoiceSuppressor::forgetSession(std::uint32_t sessionID) {
	std::lock_guard< std::mutex > lock(m_mutex);

	for (auto channelIt = m_activityByChannel.begin(); channelIt != m_activityByChannel.end();) {
		channelIt->second.erase(sessionID);
		if (channelIt->second.empty()) {
			channelIt = m_activityByChannel.erase(channelIt);
		} else {
			++channelIt;
		}
	}

	m_sessionStates.erase(sessionID);
	m_acousticCaptureUntilBySession.erase(sessionID);
}

void DuplicateVoiceSuppressor::setAcousticOracle(IAcousticOracle *oracle) {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_acousticOracle = oracle;
}

void DuplicateVoiceSuppressor::setAcousticConfirmationEnabled(bool enabled) {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_acousticConfirmationEnabled = enabled;
}

DuplicateVoiceSuppressor::Result DuplicateVoiceSuppressor::shouldForwardVoicePacket(const PacketMetadata &metadata) {
	Result result;
	AcousticSubmission acousticSubmission;
	AcousticCheck acousticCheck;

	{
		std::lock_guard< std::mutex > lock(m_mutex);

		if (metadata.isWhisperOrDirect) {
			return result;
		}

		if (metadata.isTerminator || metadata.payloadSize == 0) {
			removeActivity(metadata);
			return result;
		}

		pruneChannel(metadata.channelID, metadata.timestampMilliseconds);
		pruneAcousticCaptureWindows(metadata.timestampMilliseconds);

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

		const bool acousticEnabled = m_acousticConfirmationEnabled && m_acousticOracle != nullptr;
		if (acousticEnabled && !overlappingActivities.empty()) {
			const std::int64_t captureUntil = metadata.timestampMilliseconds + ACOUSTIC_CAPTURE_WINDOW_MS;
			m_acousticCaptureUntilBySession[metadata.sessionID] = captureUntil;
			for (const Activity *activity : overlappingActivities) {
				m_acousticCaptureUntilBySession[activity->sessionID] =
					std::max(m_acousticCaptureUntilBySession[activity->sessionID], captureUntil);
			}
		}

		if (acousticEnabled && metadata.payloadData != nullptr) {
			auto captureIt = m_acousticCaptureUntilBySession.find(metadata.sessionID);
			if (captureIt != m_acousticCaptureUntilBySession.end()
				&& metadata.timestampMilliseconds <= captureIt->second) {
				acousticSubmission.oracle = m_acousticOracle;
				acousticSubmission.sessionID = metadata.sessionID;
				acousticSubmission.codec = metadata.codec;
				acousticSubmission.payloadData = metadata.payloadData;
				acousticSubmission.payloadSize = metadata.payloadSize;
				acousticSubmission.timestampMilliseconds = metadata.timestampMilliseconds;
			}
		}

		SessionState &sessionState = m_sessionStates[metadata.sessionID];

		if (!overlappingActivities.empty() && !metadata.isPrioritySpeaker) {
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
					metadataGateTriggered = true;
					acousticConfirmed     = true;
					if (acousticEnabled) {
						acousticCheckRan = true;
						acousticConfirmed = false;
						acousticCheck.oracle = m_acousticOracle;
						acousticCheck.keptSession = bestActivity->sessionID;
						acousticCheck.suppressedSession = metadata.sessionID;
						acousticCheck.timestampMilliseconds = metadata.timestampMilliseconds;
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

			// Report whenever gate 1 actively muted/continued-muting a stream (Suppress)
			// OR gate 1 wanted to mute this frame but gate 2 vetoed it (metadataGateTriggered
			// without Suppress).
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
				result.details.correlationScore      = -1.0;

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
	}

	if (acousticSubmission.oracle != nullptr) {
		acousticSubmission.oracle->submitPacket(acousticSubmission.sessionID, acousticSubmission.codec,
												acousticSubmission.payloadData, acousticSubmission.payloadSize,
												acousticSubmission.timestampMilliseconds);
	}

	if (acousticCheck.oracle != nullptr) {
		const bool acousticConfirmed = acousticCheck.oracle->isLikelySameSource(
			acousticCheck.keptSession, acousticCheck.suppressedSession, acousticCheck.timestampMilliseconds);
		const double correlationScore = acousticCheck.oracle->lastCorrelationScore();

		result.details.acousticGateConfirmed = acousticConfirmed;
		result.details.correlationScore      = correlationScore;

		if (acousticConfirmed) {
			std::lock_guard< std::mutex > lock(m_mutex);
			SessionState &sessionState = m_sessionStates[acousticCheck.suppressedSession];
			if (sessionState.candidatePrimarySession == acousticCheck.keptSession
				&& sessionState.consecutiveWeakFrames >= REQUIRED_WEAK_FRAMES) {
				sessionState.suppressed = true;
				result.decision = Decision::Suppress;
			}
		}
	}

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

void DuplicateVoiceSuppressor::pruneAcousticCaptureWindows(std::int64_t nowMilliseconds) {
	for (auto it = m_acousticCaptureUntilBySession.begin(); it != m_acousticCaptureUntilBySession.end();) {
		if (nowMilliseconds > it->second) {
			it = m_acousticCaptureUntilBySession.erase(it);
		} else {
			++it;
		}
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
	m_acousticCaptureUntilBySession.erase(metadata.sessionID);
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
