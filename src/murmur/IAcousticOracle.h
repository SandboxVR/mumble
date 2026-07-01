// Copyright 2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MURMUR_IACOUSTICORACLE_H_
#define MUMBLE_MURMUR_IACOUSTICORACLE_H_

#include "MumbleProtocol.h"

#include <cstddef>
#include <cstdint>

// Abstract interface DuplicateVoiceSuppressor consults to acoustically confirm a
// Phase-1-flagged "candidate duplicate" pair. Exists so DuplicateVoiceSuppressor's
// existing metadata-only unit tests can inject a fake, without linking Opus.
class IAcousticOracle {
public:
	virtual ~IAcousticOracle() = default;

	// Returns true if the two sessions' most-recently-submitted audio is acoustically
	// consistent with being the same physical sound event (e.g. two mics picking up one
	// voice), i.e. correlation confirms "same source". Returns false if not confirmed
	// (different speakers, or not enough decoded audio yet to judge) -- callers should
	// fail open (not suppress) in that case.
	virtual bool isLikelySameSource(std::uint32_t sessionA, std::uint32_t sessionB,
									 std::int64_t nowMilliseconds) = 0;

	// Feed a just-received packet's payload in for decoding/buffering, keyed by session.
	// Must only be called for packets belonging to a session that is currently part of an
	// overlap candidate pair -- not for all traffic server-wide.
	virtual void submitPacket(std::uint32_t sessionID, Mumble::Protocol::AudioCodec codec,
							   const unsigned char *payload, std::size_t payloadSize,
							   std::int64_t timestampMilliseconds) = 0;

	// Drop all cached decode state for a session. Must be called on session disconnect to
	// avoid leaking decoder state.
	virtual void forgetSession(std::uint32_t sessionID) = 0;

	// Correlation score in [0.0, 1.0] from the most recent isLikelySameSource() call, or
	// -1.0 if not computed / not enough data. Used for logging/diagnostics and by tests.
	virtual double lastCorrelationScore() const = 0;
};

#endif // MUMBLE_MURMUR_IACOUSTICORACLE_H_
