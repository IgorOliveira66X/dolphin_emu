// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Movie
{
struct ControllerState;
}

namespace Core
{
class System;

namespace RE4RNGSearch
{
void ResetSession();

// Called immediately after a GameCube movie controller state is read from the DTM.
void MutateMovieControllerState(u64 movie_frame, u64 movie_input_offset,
                                Movie::ControllerState& state);

// Called once for each controller-1 poll. It captures the configured in-memory checkpoint and
// closes attempts that did not finish before the profile's hard end frame.
void OnPadPoll(u64 movie_frame);

// Called at known RE4 drop PCs. Returns true while the search owns the event.
bool OnDropProbe(System* system, u32 pc);

// Hot-loop tracer logging is disabled during the search.
bool IsSuppressingTrace();
}  // namespace RE4RNGSearch
}  // namespace Core
