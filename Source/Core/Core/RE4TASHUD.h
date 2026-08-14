// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;

namespace RE4TASHUD
{
// Clears the previous movement sample when a new game session starts.
void ResetSession();

// Samples Leon's coordinates at each controller-1 poll and updates the OSD.
void OnPadPoll(System& system, int controller_id, u64 frame, u64 input_count);
}  // namespace RE4TASHUD
}  // namespace Core
