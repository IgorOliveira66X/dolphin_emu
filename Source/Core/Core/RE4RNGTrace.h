// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

struct GCPadStatus;

namespace Core
{
class System;

namespace RE4RNGTrace
{
constexpr u32 RNG_FUNCTION_ADDRESS = 0x8014BDA0;
constexpr u32 RNG_STATE_ADDRESS = 0x802B69BC;

// Starts a fresh trace when Biohazard 4 JPN (G4BJ08) boots.
// For other games this is a no-op.
void ResetSession();

// Captures every GameCube controller poll delivered to controller 1.
void OnPadStatus(const GCPadStatus& pad_status, int controller_id, u64 frame, u64 input_count);

// Called at the entry of $8014BDA0 on the emulated CPU thread.
void OnRNGFunctionEntry(System* system);

// True for RE4 JPN drop-related PCs instrumented by the tracer.
bool IsDropProbeAddress(u32 pc);

// Called before executing a drop-related instruction. Dumps PPC register state.
void OnDropProbeEntry(System* system, u32 pc);

}  // namespace RE4RNGTrace
}  // namespace Core
