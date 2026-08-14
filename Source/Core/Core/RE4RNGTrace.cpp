// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RE4RNGTrace.h"

#include <array>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "Core/RE4RNGSearch.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Core::RE4RNGTrace
{
namespace
{
constexpr std::string_view TARGET_GAME_ID = "G4BJ08";
constexpr std::string_view RNG_TRACE_FILENAME = "re4_jpn_rng_trace.csv";
constexpr std::string_view PAD_TRACE_FILENAME = "re4_jpn_pad_trace.csv";
constexpr std::string_view DROP_TRACE_FILENAME = "re4_jpn_drop_trace.csv";

struct InputSnapshot
{
  u16 buttons = 0;
  u8 stick_x = GCPadStatus::MAIN_STICK_CENTER_X;
  u8 stick_y = GCPadStatus::MAIN_STICK_CENTER_Y;
  u8 cstick_x = GCPadStatus::C_STICK_CENTER_X;
  u8 cstick_y = GCPadStatus::C_STICK_CENTER_Y;
  u8 trigger_l = 0;
  u8 trigger_r = 0;
  u8 analog_a = 0;
  u8 analog_b = 0;
  bool connected = false;
};

struct TraceState
{
  bool active = false;
  u64 frame = 0;
  u64 input_count = 0;
  u64 poll_index = 0;
  u64 total_calls = 0;
  u64 external_calls = 0;
  u64 calls_this_poll = 0;
  u64 calls_previous_poll = 0;
  u64 drop_probe_index = 0;
  u32 last_caller = 0;
  u16 last_seed_before = 0;
  u16 last_seed_after = 0;
  u8 last_rng_byte = 0;
  InputSnapshot input{};
  std::ofstream rng_log;
  std::ofstream pad_log;
  std::ofstream drop_log;
};

std::mutex s_mutex;
TraceState s_state;

bool IsTargetGame()
{
  return SConfig::GetInstance().GetGameID() == TARGET_GAME_ID;
}

bool IsInternalRNGCaller(u32 caller)
{
  // Calls made from inside the RNG routine itself dominate the v1 log and are not useful for
  // input-to-callsite comparisons. We still count them in total_calls; we simply do not emit rows.
  return caller >= 0x8014BDA0 && caller <= 0x8014BE60;
}

std::pair<u16, u8> NextRNG(u16 old_seed)
{
  const u16 shifted = static_cast<u16>(old_seed >> 1);
  const u8 low = static_cast<u8>(shifted & 0xFF);
  const u8 high = static_cast<u8>((shifted + (old_seed >> 8)) & 0xFF);

  u16 next = static_cast<u16>((static_cast<u16>(high) << 8) | low);
  if (next == old_seed)
    next = static_cast<u16>(next + 0x0101);

  return {next, static_cast<u8>(next >> 8)};
}

std::string_view DropProbeLabel(u32 pc)
{
  switch (pc)
  {
  case 0x800D1978:
    return "drop_path_1978";
  case 0x800D1B34:
    return "drop_path_1B34";
  case 0x800D1E9C:
    return "grenade_type_roll";
  case 0x800D1EF0:
    return "grenade_final_id_checkpoint";
  case 0x800D224C:
    return "ammo_item_path";
  case 0x800D23F8:
    return "quantity_checkpoint";
  case 0x800D24B8:
    return "drop_path_24B8";
  case 0x800D2770:
    return "drop_path_2770";
  case 0x800D2794:
    return "drop_gate_rng_call";
  case 0x800D2798:
    return "drop_gate_after_rng";
  case 0x800D27B0:
    return "drop_path_27B0";
  case 0x800D2B64:
    return "drop_selected_path";
  case 0x800D2BF0:
    return "ammo_path_checkpoint";
  case 0x800D2FC0:
    return "late_drop_rng_checkpoint";
  case 0x800D2FD0:
    return "late_drop_rng_call";
  case 0x800D2FE0:
    return "late_drop_result_pre";
  case 0x800D2FE4:
    return "late_drop_result";
  case 0x800D2FF8:
    return "drop_final_return";
  default:
    return "unknown";
  }
}

void OpenTraceFiles()
{
  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  const std::string logs_path = File::GetUserPath(D_LOGS_IDX);

  s_state.rng_log =
      std::ofstream(logs_path + std::string(RNG_TRACE_FILENAME), std::ios::out | std::ios::trunc);
  s_state.pad_log =
      std::ofstream(logs_path + std::string(PAD_TRACE_FILENAME), std::ios::out | std::ios::trunc);
  s_state.drop_log =
      std::ofstream(logs_path + std::string(DROP_TRACE_FILENAME), std::ios::out | std::ios::trunc);

  if (s_state.rng_log)
  {
    s_state.rng_log
        << "call_index,external_index,frame,input_count,poll_index,call_in_poll,caller,lr,"
           "seed_before,seed_after,rng_byte,buttons,stick_x,stick_y,cstick_x,cstick_y,"
           "trigger_l,trigger_r,analog_a,analog_b,connected\n";
    s_state.rng_log.flush();
  }

  if (s_state.pad_log)
  {
    s_state.pad_log
        << "poll_index,frame,input_count,rng_calls_previous_poll,buttons,stick_x,stick_y,"
           "cstick_x,cstick_y,trigger_l,trigger_r,analog_a,analog_b,connected\n";
    s_state.pad_log.flush();
  }

  if (s_state.drop_log)
  {
    s_state.drop_log
        << "probe_index,frame,input_count,poll_index,pc,label,seed,lr,ctr,cr,xer,"
           "buttons,stick_x,stick_y,cstick_x,cstick_y,trigger_l,trigger_r,analog_a,analog_b,"
           "r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,r16,r17,r18,r19,"
           "r20,r21,r22,r23,r24,r25,r26,r27,r28,r29,r30,r31\n";
    s_state.drop_log.flush();
  }
}

void EnsureActive()
{
  if (s_state.active)
    return;

  s_state.active = true;
  OpenTraceFiles();
}

void UpdateHUD()
{
  if (!s_state.active)
    return;

  const std::string text = fmt::format(
      "RE4 RNG v2 | F:{} Poll:{} PrevCalls:{} Total:{} Ext:{}\n"
      "Caller:${:08X} Seed:${:04X}->${:04X} Byte:${:02X} Btn:${:04X} "
      "LS:{:02X},{:02X} CS:{:02X},{:02X}",
      s_state.frame, s_state.poll_index, s_state.calls_previous_poll, s_state.total_calls,
      s_state.external_calls, s_state.last_caller, s_state.last_seed_before,
      s_state.last_seed_after, s_state.last_rng_byte, s_state.input.buttons,
      s_state.input.stick_x, s_state.input.stick_y, s_state.input.cstick_x,
      s_state.input.cstick_y);

  OSD::AddTypedMessage(OSD::MessageType::RE4RNGTrace, text, OSD::Duration::VERY_LONG,
                       OSD::Color::CYAN);
}
}  // namespace

bool IsDropProbeAddress(u32 pc)
{
  switch (pc)
  {
  case 0x800D1978:
  case 0x800D1B34:
  case 0x800D1E9C:
  case 0x800D1EF0:
  case 0x800D224C:
  case 0x800D23F8:
  case 0x800D24B8:
  case 0x800D2770:
  case 0x800D2794:
  case 0x800D2798:
  case 0x800D27B0:
  case 0x800D2B64:
  case 0x800D2BF0:
  case 0x800D2FC0:
  case 0x800D2FD0:
  case 0x800D2FE0:
  case 0x800D2FE4:
  case 0x800D2FF8:
    return true;
  default:
    return false;
  }
}

void ResetSession()
{
  std::lock_guard lock{s_mutex};

  if (s_state.rng_log.is_open())
    s_state.rng_log.close();
  if (s_state.pad_log.is_open())
    s_state.pad_log.close();
  if (s_state.drop_log.is_open())
    s_state.drop_log.close();

  s_state = TraceState{};
  s_state.active = IsTargetGame();
  if (!s_state.active)
    return;

  OpenTraceFiles();
  Core::RE4RNGSearch::ResetSession();
  OSD::AddTypedMessage(
      OSD::MessageType::RE4RNGTrace,
      fmt::format("RE4 JPN RNG tracer v2 armed | RNG ${:08X} | seed ${:08X} | drop probes ON",
                  RNG_FUNCTION_ADDRESS, RNG_STATE_ADDRESS),
      OSD::Duration::VERY_LONG, OSD::Color::GREEN);
}

void OnPadStatus(const GCPadStatus& pad_status, int controller_id, u64 frame, u64 input_count)
{
  if (controller_id != 0 || !IsTargetGame())
    return;

  Core::RE4RNGSearch::OnPadPoll(frame);
  if (Core::RE4RNGSearch::IsSuppressingTrace())
    return;

  std::lock_guard lock{s_mutex};
  EnsureActive();

  s_state.calls_previous_poll = s_state.calls_this_poll;
  s_state.calls_this_poll = 0;
  s_state.frame = frame;
  s_state.input_count = input_count;
  ++s_state.poll_index;

  s_state.input.buttons = pad_status.button;
  s_state.input.stick_x = pad_status.stickX;
  s_state.input.stick_y = pad_status.stickY;
  s_state.input.cstick_x = pad_status.substickX;
  s_state.input.cstick_y = pad_status.substickY;
  s_state.input.trigger_l = pad_status.triggerLeft;
  s_state.input.trigger_r = pad_status.triggerRight;
  s_state.input.analog_a = pad_status.analogA;
  s_state.input.analog_b = pad_status.analogB;
  s_state.input.connected = pad_status.isConnected;

  if (s_state.pad_log)
  {
    s_state.pad_log << fmt::format(
        "{},{},{},{},${:04X},{},{},{},{},{},{},{},{},{}\n", s_state.poll_index,
        s_state.frame, s_state.input_count, s_state.calls_previous_poll, s_state.input.buttons,
        s_state.input.stick_x, s_state.input.stick_y, s_state.input.cstick_x,
        s_state.input.cstick_y, s_state.input.trigger_l, s_state.input.trigger_r,
        s_state.input.analog_a, s_state.input.analog_b, s_state.input.connected ? 1 : 0);

    if ((s_state.poll_index & 0x3F) == 0)
      s_state.pad_log.flush();
  }

  UpdateHUD();
}

void OnRNGFunctionEntry(System* system)
{
  if (system == nullptr || !IsTargetGame() || Core::RE4RNGSearch::IsSuppressingTrace())
    return;

  const auto& ppc_state = system->GetPowerPC().GetPPCState();
  const u32 lr = ppc_state.spr[SPR_LR];
  const u32 caller = lr >= 4 ? lr - 4 : 0;
  const u16 seed_before = system->GetMemory().Read_U16(RNG_STATE_ADDRESS);
  const auto [seed_after, rng_byte] = NextRNG(seed_before);

  std::lock_guard lock{s_mutex};
  EnsureActive();

  ++s_state.total_calls;
  ++s_state.calls_this_poll;
  s_state.last_caller = caller;
  s_state.last_seed_before = seed_before;
  s_state.last_seed_after = seed_after;
  s_state.last_rng_byte = rng_byte;

  if (IsInternalRNGCaller(caller))
    return;

  ++s_state.external_calls;
  if (s_state.rng_log)
  {
    s_state.rng_log << fmt::format(
        "{},{},{},{},{},{},${:08X},${:08X},${:04X},${:04X},${:02X},${:04X},"
        "{},{},{},{},{},{},{},{},{}\n",
        s_state.total_calls, s_state.external_calls, s_state.frame, s_state.input_count,
        s_state.poll_index, s_state.calls_this_poll, caller, lr, seed_before, seed_after, rng_byte,
        s_state.input.buttons, s_state.input.stick_x, s_state.input.stick_y,
        s_state.input.cstick_x, s_state.input.cstick_y, s_state.input.trigger_l,
        s_state.input.trigger_r, s_state.input.analog_a, s_state.input.analog_b,
        s_state.input.connected ? 1 : 0);

    if ((s_state.external_calls & 0x3F) == 0)
      s_state.rng_log.flush();
  }
}

void OnDropProbeEntry(System* system, u32 pc)
{
  if (system == nullptr || !IsTargetGame() || !IsDropProbeAddress(pc))
    return;

  if (Core::RE4RNGSearch::OnDropProbe(system, pc))
    return;

  const auto& ppc_state = system->GetPowerPC().GetPPCState();
  const u16 seed = system->GetMemory().Read_U16(RNG_STATE_ADDRESS);
  const u32 lr = ppc_state.spr[SPR_LR];
  const u32 ctr = ppc_state.spr[SPR_CTR];
  const u32 cr = ppc_state.cr.Get();
  const u32 xer = ppc_state.GetXER().Hex;

  std::lock_guard lock{s_mutex};
  EnsureActive();
  ++s_state.drop_probe_index;

  if (!s_state.drop_log)
    return;

  s_state.drop_log << fmt::format(
      "{},{},{},{},${:08X},{},${:04X},${:08X},${:08X},${:08X},${:08X},"
      "${:04X},{},{},{},{},{},{},{},{},",
      s_state.drop_probe_index, s_state.frame, s_state.input_count, s_state.poll_index, pc,
      DropProbeLabel(pc), seed, lr, ctr, cr, xer, s_state.input.buttons, s_state.input.stick_x,
      s_state.input.stick_y, s_state.input.cstick_x, s_state.input.cstick_y,
      s_state.input.trigger_l, s_state.input.trigger_r, s_state.input.analog_a,
      s_state.input.analog_b);

  for (size_t i = 0; i < std::size(ppc_state.gpr); ++i)
  {
    if (i != 0)
      s_state.drop_log << ',';
    s_state.drop_log << fmt::format("${:08X}", ppc_state.gpr[i]);
  }
  s_state.drop_log << '\n';
  s_state.drop_log.flush();
}

}  // namespace Core::RE4RNGTrace
