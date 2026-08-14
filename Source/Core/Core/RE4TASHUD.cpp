// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RE4TASHUD.h"

#include <bit>
#include <cmath>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Core::RE4TASHUD
{
namespace
{
constexpr std::string_view TARGET_GAME_ID = "G4BJ08";
constexpr u32 PLAYER_POINTER_ADDRESS = 0x80281760;
constexpr u32 DYNAMIC_DIFFICULTY_POINTS_ADDRESS = 0x80287FA0;
constexpr u32 DYNAMIC_DIFFICULTY_LEVEL_ADDRESS = 0x80287FA4;
constexpr u32 POSITION_OFFSET = 0x94;
constexpr u32 YAW_OFFSET = 0xA4;
constexpr u32 MEM1_START = 0x80000000;
constexpr u32 MEM1_END = 0x81800000;
constexpr float MAX_REASONABLE_COORDINATE = 100000000.0f;
constexpr double RADIANS_TO_DEGREES = 57.2957795130823208768;

struct Vec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct HUDState
{
  bool has_previous = false;
  Vec3 previous_position{};
  u64 previous_frame = 0;
  u64 previous_input_count = 0;
};

HUDState s_state;

bool IsTargetGame()
{
  return SConfig::GetInstance().GetGameID() == TARGET_GAME_ID;
}

bool IsValidPlayerPointer(u32 pointer)
{
  return pointer >= MEM1_START && pointer <= MEM1_END - (YAW_OFFSET + sizeof(float));
}

float ReadFloat(const Memory::MemoryManager& memory, u32 address)
{
  return std::bit_cast<float>(memory.Read_U32(address));
}

bool IsReasonable(float value)
{
  return std::isfinite(value) && std::abs(value) < MAX_REASONABLE_COORDINATE;
}

bool IsValidPosition(const Vec3& position)
{
  return IsReasonable(position.x) && IsReasonable(position.y) && IsReasonable(position.z);
}

void ShowWaitingHUD(u64 frame, u64 input_count, u32 player_pointer,
                    u8 dynamic_difficulty_level, s32 dynamic_difficulty_points)
{
  OSD::AddTypedMessage(
      OSD::MessageType::RE4TASHUD,
      fmt::format("RE4 TAS HUD | F:{} I:{}\nDA Rank:{} | Pontos:{}\n"
                  "Aguardando Leon | PTR:${:08X}",
                  frame, input_count, dynamic_difficulty_level, dynamic_difficulty_points,
                  player_pointer),
      OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
}
}  // namespace

void ResetSession()
{
  s_state = {};

  if (!IsTargetGame())
    return;

  OSD::AddTypedMessage(OSD::MessageType::RE4TASHUD,
                       fmt::format("RE4 TAS HUD armado | Leon PTR @ ${:08X}",
                                   PLAYER_POINTER_ADDRESS),
                       OSD::Duration::VERY_LONG, OSD::Color::GREEN);
}

void OnPadPoll(System& system, int controller_id, u64 frame, u64 input_count)
{
  if (controller_id != 0 || !IsTargetGame())
    return;

  const auto& memory = system.GetMemory();
  const s32 dynamic_difficulty_points =
      static_cast<s32>(memory.Read_U32(DYNAMIC_DIFFICULTY_POINTS_ADDRESS));
  const u8 dynamic_difficulty_level = memory.Read_U8(DYNAMIC_DIFFICULTY_LEVEL_ADDRESS);
  const u32 player_pointer = memory.Read_U32(PLAYER_POINTER_ADDRESS);
  if (!IsValidPlayerPointer(player_pointer))
  {
    s_state.has_previous = false;
    ShowWaitingHUD(frame, input_count, player_pointer, dynamic_difficulty_level,
                   dynamic_difficulty_points);
    return;
  }

  const Vec3 position{
      ReadFloat(memory, player_pointer + POSITION_OFFSET),
      ReadFloat(memory, player_pointer + POSITION_OFFSET + sizeof(float)),
      ReadFloat(memory, player_pointer + POSITION_OFFSET + 2 * sizeof(float)),
  };
  const float yaw = ReadFloat(memory, player_pointer + YAW_OFFSET);

  if (!IsValidPosition(position) || !std::isfinite(yaw))
  {
    s_state.has_previous = false;
    ShowWaitingHUD(frame, input_count, player_pointer, dynamic_difficulty_level,
                   dynamic_difficulty_points);
    return;
  }

  float delta_x = 0.0f;
  float delta_y = 0.0f;
  float delta_z = 0.0f;
  float horizontal_per_poll = 0.0f;
  float horizontal_per_vi = 0.0f;
  double movement_heading = 0.0;
  u64 frame_delta = 0;

  const bool advances_timeline =
      s_state.has_previous && frame > s_state.previous_frame &&
      input_count > s_state.previous_input_count;
  if (advances_timeline)
  {
    delta_x = position.x - s_state.previous_position.x;
    delta_y = position.y - s_state.previous_position.y;
    delta_z = position.z - s_state.previous_position.z;
    horizontal_per_poll = std::hypot(delta_x, delta_z);
    frame_delta = frame - s_state.previous_frame;
    horizontal_per_vi = horizontal_per_poll / static_cast<float>(frame_delta);
    if (horizontal_per_poll > 0.000001f)
      movement_heading = std::atan2(static_cast<double>(delta_x), static_cast<double>(delta_z)) *
                         RADIANS_TO_DEGREES;
  }

  const std::string delta_text = advances_timeline ?
                                     fmt::format(
                                         "dPoll X:{:+.3f} Y:{:+.3f} Z:{:+.3f} | {} VI",
                                         delta_x, delta_y, delta_z, frame_delta) :
                                     "dPoll: -- | primeiro poll/rewind";
  const std::string speed_text =
      advances_timeline ?
          fmt::format("Vel H: {:.4f}/VI  {:.4f}/poll | Dir:{:+.2f} deg",
                      horizontal_per_vi, horizontal_per_poll, movement_heading) :
          "Vel H: -- | Dir: --";

  OSD::AddTypedMessage(
      OSD::MessageType::RE4TASHUD,
      fmt::format("RE4 TAS HUD | F:{} I:{}\n"
                  "DA Rank:{} | Pontos:{}\n"
                  "POS X:{:.3f} Y:{:.3f} Z:{:.3f}\n"
                  "{}\n"
                  "{}\n"
                  "Leon Yaw:{:.4f}",
                  frame, input_count, dynamic_difficulty_level, dynamic_difficulty_points,
                  position.x, position.y, position.z, delta_text, speed_text, yaw),
      OSD::Duration::VERY_LONG, OSD::Color::CYAN);

  s_state.has_previous = true;
  s_state.previous_position = position;
  s_state.previous_frame = frame;
  s_state.previous_input_count = input_count;
}
}  // namespace Core::RE4TASHUD
