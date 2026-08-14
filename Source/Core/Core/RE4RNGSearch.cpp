// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RE4RNGSearch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/IniFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Movie.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/RE4RNGTrace.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Core::RE4RNGSearch
{
namespace
{
constexpr u32 GATE_PC = 0x800D2794;
constexpr u32 ROUTE_PC = 0x800D1E9C;
constexpr u32 GRENADE_FINAL_ID_PC = 0x800D1EF0;
constexpr std::array<u32, 3> MONEY_PROBE_PCS = {0x800D2FC0, 0x800D2FD0, 0x800D2FE0};
constexpr u32 LATE_DROP_RESULT_PC = 0x800D2FE4;
constexpr u32 DROP_FINAL_RETURN_PC = 0x800D2FF8;
constexpr u32 PTAS_ITEM_ID = 0x78;
constexpr size_t MAX_DROPS = 10;
constexpr size_t MAX_CORPUS_ENTRIES = 8192;
constexpr size_t MAX_FOCUSED_ENTRIES = 4096;
constexpr u64 DEFAULT_FUZZ_SEED = 0x5245345F56375F31ULL;
constexpr std::string_view CONFIG_FILENAME = "RE4DropSearch.ini";

enum class MutationKind : u8
{
  BOff,
  LOn,
  ROn,
  StickXDelta,
  StickYDelta,
  StickYCenter,
  CStickXDelta,
  CStickYDelta,
};

enum class ControlGroup : u8
{
  B,
  L,
  R,
  StickX,
  StickY,
  CStickX,
  CStickY,
};

enum class TargetKind : u8
{
  Any,
  Hand,
  Grenade,
  AnyGrenade,
  Ptas,
  AnyPtas,
  Item,
};

enum class TargetMode : u8
{
  ExactAnyOrder,
  ContainsAnyOrder,
  PerSlot,
};

struct TargetSpec
{
  TargetKind kind = TargetKind::Any;
  u32 value = 0;
  u32 extra = 0;
  std::string text = "ANY";
};

struct SearchConfig
{
  bool enabled = true;
  std::string profile = "Village dual drop - Hand + 9900";
  std::string output_prefix = "re4_jpn_village_dual_v7";
  u64 expected_movie_frames = 8089;
  u64 expected_movie_inputs = 16173;
  u64 capture_frame = 7823;
  u64 window_first = 7826;
  u64 window_max = 8032;
  u64 movement_first = 7825;
  u64 movement_last = 7953;
  u64 hard_end_frame = 8070;
  u64 budget = 200000;
  u32 expected_drops = 2;
  u32 max_movement_cost_frames = 4;
  u32 max_plan_mutations = 12;
  u32 max_hold_slots = 6;
  u32 calibration_anchors = 9;
  bool full_calibration = false;
  u32 worker_id = 0;
  u32 worker_count = 1;
  u64 fuzz_seed = DEFAULT_FUZZ_SEED;
  u32 hand_grenade_id = 1;
  std::map<std::string, u32> grenade_aliases{{"HAND", 1}};
  u32 adaptive_points_address = 0x80287FA0;
  u32 adaptive_rank_address = 0x80287FA4;
  TargetMode target_mode = TargetMode::ExactAnyOrder;
  std::vector<TargetSpec> targets;
  std::vector<MutationKind> allowed_kinds;
  std::set<MutationKind> forced_kinds;
  std::vector<u64> hotspots;
};

struct Mutation
{
  u64 slot = 0;
  u8 duration = 1;
  MutationKind kind = MutationKind::BOff;
  int amount = 0;
};

struct Plan
{
  int phase = 0;
  std::vector<Mutation> mutations;
  std::string label = "BASELINE";
  std::string parent_label = "ROOT";
  u32 movement_cost_frames = 0;
};

struct ProbeState
{
  bool seen = false;
  u16 seed = 0;
  u32 r29 = 0;
  u32 r30 = 0;
  u32 r31 = 0;
};

struct DropEvent
{
  bool gate_seen = false;
  u64 gate_frame = 0;
  u16 gate_seed = 0;
  bool route_seen = false;
  u16 route_seed = 0;
  bool grenade_seen = false;
  u32 grenade_id = 0xFFFFFFFF;
  std::array<ProbeState, 3> money_probes{};
  bool late_result_seen = false;
  u32 late_r31 = 0xFFFFFFFF;
  u32 late_r29 = 0xFFFFFFFF;
  bool final_return_seen = false;
  bool complete = false;
};

struct Outcome
{
  std::vector<DropEvent> events;
  int active_event = -1;
  size_t gate_count = 0;
  size_t completed_count = 0;
  bool overflow = false;
  bool target = false;
  bool novel_signature = false;
  bool has_grenade_route = false;
  bool has_money_route = false;
  u32 matched_targets = 0;
  u64 fitness = 0;
  u32 difficulty_points = 0;
  u8 difficulty_rank = 0;
  std::string status = "ok";
};

struct CorpusEntry
{
  Plan plan;
  Outcome outcome;
  u64 score = 0;
};

struct SearchState
{
  SearchConfig config;
  bool armed = false;
  bool capture_requested = false;
  bool snapshot_ready = false;
  bool restore_requested = false;
  bool active = false;
  bool finished = false;
  bool found = false;
  bool validation_done = false;
  bool calibration_built = false;
  bool calibration_done = false;
  u64 attempts = 0;
  u64 rng_state = DEFAULT_FUZZ_SEED;
  u64 effective_window_last = 0;
  std::vector<u8> snapshot;
  std::deque<Plan> queue;
  Plan current_plan;
  Outcome current_outcome;
  std::vector<Outcome> validation_outcomes;
  std::vector<u64> baseline_signature;
  std::vector<CorpusEntry> corpus;
  std::vector<CorpusEntry> route_corpus;
  std::vector<CorpusEntry> money_corpus;
  std::vector<std::vector<CorpusEntry>> target_corpora;
  std::set<std::vector<u64>> seen_signatures;
  std::set<u16> seen_gate_seeds;
  std::set<u16> seen_route_seeds;
  std::set<u32> seen_grenade_ids;
  std::set<u32> seen_money_units;
  std::set<MutationKind> effective_kinds;
  std::set<u64> effective_slots;
  std::unordered_set<std::string> known_plan_labels;
  std::map<u64, std::array<u8, sizeof(Movie::ControllerState)>> mutated_inputs;
  float previous_speed = 1.0f;
  std::string config_path;
  std::string config_error;
  std::string validation_error;
  std::string found_dtm_path;
  std::chrono::steady_clock::time_point search_start{};
  std::chrono::steady_clock::time_point attempt_start{};
  std::ofstream results;
};

std::mutex s_mutex;
std::atomic_bool s_suppress_trace{false};
SearchState s_state;

std::string Trim(std::string text)
{
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

std::string Upper(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return text;
}

std::vector<std::string> SplitList(std::string_view text)
{
  std::vector<std::string> parts;
  size_t first = 0;
  while (first <= text.size())
  {
    const size_t comma = text.find(',', first);
    const size_t last = comma == std::string_view::npos ? text.size() : comma;
    std::string part = Trim(std::string{text.substr(first, last - first)});
    if (!part.empty())
      parts.push_back(std::move(part));
    if (comma == std::string_view::npos)
      break;
    first = comma + 1;
  }
  return parts;
}

std::optional<u64> ParseU64(std::string text)
{
  text = Trim(std::move(text));
  if (text.empty())
    return std::nullopt;
  try
  {
    size_t used = 0;
    const u64 value = std::stoull(text, &used, 0);
    if (used == text.size())
      return value;
  }
  catch (...)
  {
  }
  return std::nullopt;
}

std::string SafePrefix(std::string text)
{
  for (char& c : text)
  {
    const unsigned char value = static_cast<unsigned char>(c);
    if (!std::isalnum(value) && c != '_' && c != '-')
      c = '_';
  }
  while (text.find("__") != std::string::npos)
    text.replace(text.find("__"), 2, "_");
  return text.empty() ? "re4_drop_search" : text;
}

std::optional<MutationKind> ParseMutationKind(std::string text)
{
  text = Upper(Trim(std::move(text)));
  if (text == "B_OFF" || text == "B")
    return MutationKind::BOff;
  if (text == "L")
    return MutationKind::LOn;
  if (text == "R")
    return MutationKind::ROn;
  if (text == "SX" || text == "STICK_X")
    return MutationKind::StickXDelta;
  if (text == "SY" || text == "STICK_Y")
    return MutationKind::StickYDelta;
  if (text == "SY_CENTER" || text == "STICK_Y_CENTER")
    return MutationKind::StickYCenter;
  if (text == "CX" || text == "CSTICK_X")
    return MutationKind::CStickXDelta;
  if (text == "CY" || text == "CSTICK_Y")
    return MutationKind::CStickYDelta;
  return std::nullopt;
}

std::string KindName(MutationKind kind, int amount = 0)
{
  switch (kind)
  {
  case MutationKind::BOff:
    return "B_OFF";
  case MutationKind::LOn:
    return "L";
  case MutationKind::ROn:
    return "R";
  case MutationKind::StickXDelta:
    return fmt::format("SX{}{}", amount < 0 ? "m" : "p", std::abs(amount));
  case MutationKind::StickYDelta:
    return fmt::format("SY{}{}", amount < 0 ? "m" : "p", std::abs(amount));
  case MutationKind::StickYCenter:
    return "SY_CENTER";
  case MutationKind::CStickXDelta:
    return fmt::format("CX{}{}", amount < 0 ? "m" : "p", std::abs(amount));
  case MutationKind::CStickYDelta:
    return fmt::format("CY{}{}", amount < 0 ? "m" : "p", std::abs(amount));
  }
  return "?";
}

ControlGroup GetControlGroup(MutationKind kind)
{
  switch (kind)
  {
  case MutationKind::BOff:
    return ControlGroup::B;
  case MutationKind::LOn:
    return ControlGroup::L;
  case MutationKind::ROn:
    return ControlGroup::R;
  case MutationKind::StickXDelta:
    return ControlGroup::StickX;
  case MutationKind::StickYDelta:
  case MutationKind::StickYCenter:
    return ControlGroup::StickY;
  case MutationKind::CStickXDelta:
    return ControlGroup::CStickX;
  case MutationKind::CStickYDelta:
    return ControlGroup::CStickY;
  }
  return ControlGroup::B;
}

std::string TargetModeName(TargetMode mode)
{
  switch (mode)
  {
  case TargetMode::ExactAnyOrder:
    return "EXACT_ANY_ORDER";
  case TargetMode::ContainsAnyOrder:
    return "CONTAINS_ANY_ORDER";
  case TargetMode::PerSlot:
    return "PER_SLOT";
  }
  return "?";
}

bool GetIniString(const Common::IniFile& ini, std::string_view section, std::string_view key,
                  std::string* value)
{
  const Common::IniFile::Section* const found = ini.GetSection(section);
  return found && found->Get(key, value);
}

template <typename T>
void GetIniValue(const Common::IniFile& ini, std::string_view section, std::string_view key,
                 T* value)
{
  if (const Common::IniFile::Section* const found = ini.GetSection(section))
    found->Get(key, value, *value);
}

bool GetIniU64(const Common::IniFile& ini, std::string_view section, std::string_view key,
               u64* value, std::string* error)
{
  std::string text;
  if (!GetIniString(ini, section, key, &text))
    return true;
  const std::optional<u64> parsed = ParseU64(text);
  if (!parsed)
  {
    *error = fmt::format("invalid [{}] {} = {}", section, key, text);
    return false;
  }
  *value = *parsed;
  return true;
}

bool GetIniU32(const Common::IniFile& ini, std::string_view section, std::string_view key,
               u32* value, std::string* error)
{
  u64 wide = *value;
  if (!GetIniU64(ini, section, key, &wide, error))
    return false;
  if (wide > std::numeric_limits<u32>::max())
  {
    *error = fmt::format("[{}] {} is too large", section, key);
    return false;
  }
  *value = static_cast<u32>(wide);
  return true;
}

std::optional<TargetSpec> ParseTarget(std::string token, const SearchConfig& config,
                                      std::string* error)
{
  token = Upper(Trim(std::move(token)));
  if (token.empty())
    return std::nullopt;
  if (token == "ANY" || token == "IGNORE")
    return TargetSpec{TargetKind::Any, 0, 0, token};
  if (token == "ANY_GRENADE")
    return TargetSpec{TargetKind::AnyGrenade, 0, 0, token};
  if (token == "ANY_PTAS" || token == "ANY_MONEY")
    return TargetSpec{TargetKind::AnyPtas, 0, 0, "ANY_PTAS"};

  if (const auto alias = config.grenade_aliases.find(token);
      alias != config.grenade_aliases.end())
  {
    return TargetSpec{token == "HAND" ? TargetKind::Hand : TargetKind::Grenade, alias->second, 0,
                      token};
  }

  const size_t first_colon = token.find(':');
  const std::string kind = token.substr(0, first_colon);
  const std::string value = first_colon == std::string::npos ? "" : token.substr(first_colon + 1);
  if (kind == "GRENADE")
  {
    const std::optional<u64> id = ParseU64(value);
    if (id && *id <= std::numeric_limits<u32>::max())
      return TargetSpec{TargetKind::Grenade, static_cast<u32>(*id), 0, token};
  }
  else if (kind == "PTAS" || kind == "MONEY")
  {
    const std::optional<u64> amount = ParseU64(value);
    if (amount && *amount <= static_cast<u64>(std::numeric_limits<u32>::max()) * 10 &&
        (*amount % 10) == 0)
    {
      return TargetSpec{TargetKind::Ptas, static_cast<u32>(*amount / 10), 0,
                        fmt::format("PTAS:{}", *amount)};
    }
  }
  else if (kind == "ITEM")
  {
    const size_t second_colon = value.find(':');
    if (second_colon != std::string::npos)
    {
      const std::optional<u64> item_id = ParseU64(value.substr(0, second_colon));
      const std::optional<u64> item_value = ParseU64(value.substr(second_colon + 1));
      if (item_id && item_value && *item_id <= std::numeric_limits<u32>::max() &&
          *item_value <= std::numeric_limits<u32>::max())
      {
        return TargetSpec{TargetKind::Item, static_cast<u32>(*item_id),
                          static_cast<u32>(*item_value), token};
      }
    }
  }

  *error = fmt::format("unknown target '{}'", token);
  return std::nullopt;
}

bool LoadConfig(SearchConfig* config, std::string* path, std::string* error)
{
  *path = File::GetExeDirectory() + DIR_SEP + std::string(CONFIG_FILENAME);
  Common::IniFile ini;
  if (!ini.Load(*path))
  {
    *error = fmt::format("{} not found beside Dolphin.exe", CONFIG_FILENAME);
    return false;
  }

  GetIniValue(ini, "Search", "Enabled", &config->enabled);
  GetIniString(ini, "Search", "Profile", &config->profile);
  GetIniString(ini, "Search", "OutputPrefix", &config->output_prefix);
  config->output_prefix = SafePrefix(config->output_prefix);
  if (!GetIniU64(ini, "Search", "ExpectedMovieFrames", &config->expected_movie_frames, error) ||
      !GetIniU64(ini, "Search", "ExpectedMovieInputs", &config->expected_movie_inputs, error) ||
      !GetIniU64(ini, "Search", "Budget", &config->budget, error) ||
      !GetIniU32(ini, "Search", "ExpectedDrops", &config->expected_drops, error) ||
      !GetIniU32(ini, "Search", "WorkerId", &config->worker_id, error) ||
      !GetIniU32(ini, "Search", "WorkerCount", &config->worker_count, error) ||
      !GetIniU64(ini, "Search", "FuzzSeed", &config->fuzz_seed, error) ||
      !GetIniU64(ini, "Window", "CaptureFrame", &config->capture_frame, error) ||
      !GetIniU64(ini, "Window", "FirstSlot", &config->window_first, error) ||
      !GetIniU64(ini, "Window", "LastSlot", &config->window_max, error) ||
      !GetIniU64(ini, "Window", "MovementFirstFrame", &config->movement_first, error) ||
      !GetIniU64(ini, "Window", "MovementLastFrame", &config->movement_last, error) ||
      !GetIniU64(ini, "Window", "HardEndFrame", &config->hard_end_frame, error) ||
      !GetIniU32(ini, "Window", "MaxMovementCostFrames",
                 &config->max_movement_cost_frames, error) ||
      !GetIniU32(ini, "Mutations", "MaxPlanMutations", &config->max_plan_mutations, error) ||
      !GetIniU32(ini, "Mutations", "MaxHoldSlots", &config->max_hold_slots, error) ||
      !GetIniU32(ini, "Mutations", "CalibrationAnchors", &config->calibration_anchors, error) ||
      !GetIniU32(ini, "Memory", "AdaptivePointsAddress",
                 &config->adaptive_points_address, error) ||
      !GetIniU32(ini, "Memory", "AdaptiveRankAddress", &config->adaptive_rank_address, error))
  {
    return false;
  }
  GetIniValue(ini, "Mutations", "FullCalibration", &config->full_calibration);

  config->grenade_aliases.clear();
  if (const Common::IniFile::Section* const catalog = ini.GetSection("Catalog"))
  {
    for (const auto& [name, value] : catalog->GetValues())
    {
      const std::string trimmed_name = Trim(name);
      if (trimmed_name.starts_with(';') || trimmed_name.starts_with('#'))
        continue;
      const std::optional<u64> parsed = ParseU64(value);
      if (!parsed || *parsed > std::numeric_limits<u32>::max())
      {
        *error = fmt::format("invalid [Catalog] {} = {}", name, value);
        return false;
      }
      config->grenade_aliases[Upper(name)] = static_cast<u32>(*parsed);
    }
  }
  if (!config->grenade_aliases.contains("HAND"))
    config->grenade_aliases["HAND"] = 1;
  config->hand_grenade_id = config->grenade_aliases.at("HAND");

  std::string mode = "EXACT_ANY_ORDER";
  GetIniString(ini, "Target", "Mode", &mode);
  mode = Upper(Trim(std::move(mode)));
  if (mode == "EXACT" || mode == "EXACT_ANY_ORDER")
    config->target_mode = TargetMode::ExactAnyOrder;
  else if (mode == "CONTAINS" || mode == "CONTAINS_ANY_ORDER")
    config->target_mode = TargetMode::ContainsAnyOrder;
  else if (mode == "PER_SLOT" || mode == "SLOTS")
    config->target_mode = TargetMode::PerSlot;
  else
  {
    *error = fmt::format("invalid [Target] Mode = {}", mode);
    return false;
  }

  std::vector<std::string> target_tokens;
  std::string targets;
  if (GetIniString(ini, "Target", "Targets", &targets))
    target_tokens = SplitList(targets);
  if (target_tokens.empty())
  {
    for (size_t i = 1; i <= MAX_DROPS; ++i)
    {
      std::string target;
      if (GetIniString(ini, "Target", fmt::format("Target{}", i), &target) &&
          !Trim(target).empty())
      {
        target_tokens.push_back(std::move(target));
      }
    }
  }
  config->targets.clear();
  for (std::string token : target_tokens)
  {
    std::optional<TargetSpec> target = ParseTarget(std::move(token), *config, error);
    if (!target)
      return false;
    config->targets.push_back(std::move(*target));
  }

  std::string allowed = "B_OFF,L,R,SX,SY,SY_CENTER,CX,CY";
  GetIniString(ini, "Mutations", "AllowedKinds", &allowed);
  config->allowed_kinds.clear();
  for (const std::string& token : SplitList(allowed))
  {
    const std::optional<MutationKind> kind = ParseMutationKind(token);
    if (!kind)
    {
      *error = fmt::format("unknown mutation kind '{}'", token);
      return false;
    }
    if (std::find(config->allowed_kinds.begin(), config->allowed_kinds.end(), *kind) ==
        config->allowed_kinds.end())
    {
      config->allowed_kinds.push_back(*kind);
    }
  }

  std::string forced;
  GetIniString(ini, "Mutations", "ForcedKinds", &forced);
  config->forced_kinds.clear();
  for (const std::string& token : SplitList(forced))
  {
    const std::optional<MutationKind> kind = ParseMutationKind(token);
    if (!kind || std::find(config->allowed_kinds.begin(), config->allowed_kinds.end(), *kind) ==
                     config->allowed_kinds.end())
    {
      *error = fmt::format("ForcedKinds contains unavailable kind '{}'", token);
      return false;
    }
    config->forced_kinds.insert(*kind);
  }

  std::string hotspots;
  GetIniString(ini, "Mutations", "Hotspots", &hotspots);
  config->hotspots.clear();
  for (const std::string& token : SplitList(hotspots))
  {
    const std::optional<u64> slot = ParseU64(token);
    if (!slot)
    {
      *error = fmt::format("invalid hotspot '{}'", token);
      return false;
    }
    config->hotspots.push_back(*slot);
  }

  if (!config->enabled)
    return true;
  if (config->targets.empty())
    *error = "no targets configured";
  else if (config->targets.size() > MAX_DROPS)
    *error = "more than 10 targets configured";
  else if (config->expected_drops > MAX_DROPS)
    *error = "ExpectedDrops must be 0..10";
  else if (config->target_mode == TargetMode::ExactAnyOrder &&
           config->expected_drops != 0 && config->expected_drops != config->targets.size())
    *error = "EXACT_ANY_ORDER requires ExpectedDrops to equal the target count";
  else if (config->expected_drops != 0 && config->expected_drops < config->targets.size() &&
           config->target_mode != TargetMode::PerSlot)
    *error = "ExpectedDrops is smaller than the target list";
  else if (config->window_first > config->window_max)
    *error = "FirstSlot is after LastSlot";
  else if (config->capture_frame >= config->hard_end_frame)
    *error = "CaptureFrame must be before HardEndFrame";
  else if (config->movement_first > config->movement_last)
    *error = "MovementFirstFrame is after MovementLastFrame";
  else if (config->budget == 0)
    *error = "Budget must be positive";
  else if (config->worker_count == 0 || config->worker_id >= config->worker_count)
    *error = "WorkerId must be smaller than WorkerCount";
  else if (config->allowed_kinds.empty())
    *error = "AllowedKinds is empty";
  else if (config->max_plan_mutations == 0 || config->max_plan_mutations > 32)
    *error = "MaxPlanMutations must be 1..32";
  else if (config->max_hold_slots == 0 || config->max_hold_slots > 32)
    *error = "MaxHoldSlots must be 1..32";
  else if (config->calibration_anchors == 0 || config->calibration_anchors > 128)
    *error = "CalibrationAnchors must be 1..128";

  if (!error->empty())
    return false;

  config->fuzz_seed ^= (static_cast<u64>(config->worker_id) + 1) * 0x9E3779B97F4A7C15ULL;
  return true;
}

bool IsMoney(const DropEvent& event)
{
  return event.late_result_seen && event.late_r31 == PTAS_ITEM_ID;
}

bool IsGrenade(const DropEvent& event)
{
  return event.grenade_seen && event.grenade_id != 0xFFFFFFFF;
}

bool Matches(const DropEvent& event, const TargetSpec& target)
{
  switch (target.kind)
  {
  case TargetKind::Any:
    return event.complete;
  case TargetKind::Hand:
  case TargetKind::Grenade:
    return IsGrenade(event) && event.grenade_id == target.value;
  case TargetKind::AnyGrenade:
    return IsGrenade(event);
  case TargetKind::Ptas:
    return IsMoney(event) && event.late_r29 == target.value;
  case TargetKind::AnyPtas:
    return IsMoney(event);
  case TargetKind::Item:
    return event.late_result_seen && event.late_r31 == target.value &&
           event.late_r29 == target.extra;
  }
  return false;
}

std::string EventClass(const DropEvent& event)
{
  if (IsGrenade(event))
  {
    for (const auto& [name, id] : s_state.config.grenade_aliases)
    {
      if (event.grenade_id == id)
        return name;
    }
    return fmt::format("GRENADE_ID_{}", event.grenade_id);
  }
  if (IsMoney(event))
    return fmt::format("{}_PTAS", static_cast<u64>(event.late_r29) * 10);
  if (event.late_result_seen)
    return fmt::format("ITEM_{:X}_VALUE_{:X}", event.late_r31, event.late_r29);
  return event.complete ? "NO_DROP" : "UNKNOWN";
}

std::vector<size_t> CompleteEventIndices(const Outcome& outcome)
{
  std::vector<size_t> indices;
  for (size_t i = 0; i < outcome.events.size(); ++i)
  {
    if (outcome.events[i].complete)
      indices.push_back(i);
  }
  return indices;
}

u32 BestAnyOrderMatchCount(const Outcome& outcome)
{
  const size_t target_count = s_state.config.targets.size();
  if (target_count == 0)
    return 0;

  std::vector<bool> reachable(size_t{1} << target_count, false);
  reachable[0] = true;
  for (size_t event_index : CompleteEventIndices(outcome))
  {
    std::vector<bool> next = reachable;
    for (size_t mask = 0; mask < reachable.size(); ++mask)
    {
      if (!reachable[mask])
        continue;
      for (size_t target_index = 0; target_index < target_count; ++target_index)
      {
        if ((mask & (size_t{1} << target_index)) == 0 &&
            Matches(outcome.events[event_index], s_state.config.targets[target_index]))
        {
          next[mask | (size_t{1} << target_index)] = true;
        }
      }
    }
    reachable = std::move(next);
  }

  u32 best = 0;
  for (size_t mask = 0; mask < reachable.size(); ++mask)
  {
    if (reachable[mask])
      best = std::max(best, static_cast<u32>(std::popcount(mask)));
  }
  return best;
}

u64 TargetCloseness(const DropEvent& event, const TargetSpec& target)
{
  if (Matches(event, target))
    return 500000;

  const bool wants_grenade = target.kind == TargetKind::Hand ||
                             target.kind == TargetKind::Grenade ||
                             target.kind == TargetKind::AnyGrenade;
  const bool wants_money = target.kind == TargetKind::Ptas ||
                           target.kind == TargetKind::AnyPtas ||
                           (target.kind == TargetKind::Item && target.value == PTAS_ITEM_ID);
  u64 score = 0;
  if (wants_grenade)
  {
    if (event.route_seen)
      score += 80000;
    if (IsGrenade(event))
      score += 120000;
  }
  if (wants_money)
  {
    const size_t probes = std::count_if(event.money_probes.begin(), event.money_probes.end(),
                                       [](const ProbeState& probe) { return probe.seen; });
    score += probes * 15000;
    if (IsMoney(event))
    {
      score += 100000;
      if (target.kind == TargetKind::Ptas)
      {
        const u32 distance = event.late_r29 > target.value ? event.late_r29 - target.value :
                                                             target.value - event.late_r29;
        score += 50000 / (1 + distance);
        score += std::min<u64>(event.late_r29, 5000);
      }
    }
  }
  if (target.kind == TargetKind::Item && event.late_result_seen &&
      event.late_r31 == target.value)
  {
    score += 150000;
  }
  return score;
}

void UpdateOutcome(Outcome& outcome)
{
  outcome.has_grenade_route = false;
  outcome.has_money_route = false;
  for (const DropEvent& event : outcome.events)
  {
    outcome.has_grenade_route |= event.route_seen || IsGrenade(event);
    outcome.has_money_route |= IsMoney(event) ||
                               std::any_of(event.money_probes.begin(), event.money_probes.end(),
                                           [](const ProbeState& probe) { return probe.seen; });
  }

  bool goal = false;
  if (s_state.config.target_mode == TargetMode::PerSlot)
  {
    outcome.matched_targets = 0;
    goal = s_state.config.targets.size() <= outcome.events.size();
    for (size_t i = 0; i < s_state.config.targets.size(); ++i)
    {
      if (Matches(outcome.events[i], s_state.config.targets[i]))
        ++outcome.matched_targets;
      else
        goal = false;
    }
  }
  else
  {
    outcome.matched_targets = BestAnyOrderMatchCount(outcome);
    goal = outcome.matched_targets == s_state.config.targets.size();
    if (s_state.config.target_mode == TargetMode::ExactAnyOrder)
      goal &= outcome.completed_count == s_state.config.targets.size();
  }
  outcome.target = goal;

  u64 fitness = static_cast<u64>(outcome.matched_targets) * 1000000000ULL;
  for (const TargetSpec& target : s_state.config.targets)
  {
    u64 best = 0;
    for (const DropEvent& event : outcome.events)
      best = std::max(best, TargetCloseness(event, target));
    fitness += best;
  }
  fitness += outcome.completed_count * 1000 + outcome.gate_count * 100;
  outcome.fitness = fitness;
}

std::vector<u64> Signature(const Outcome& outcome)
{
  std::vector<u64> key;
  key.reserve(outcome.events.size() * 12 + 2);
  for (const DropEvent& event : outcome.events)
  {
    key.push_back((static_cast<u64>(event.gate_seen) << 63) | event.gate_frame);
    key.push_back((static_cast<u64>(event.gate_seed) << 32) |
                  (static_cast<u64>(event.route_seen) << 31) | event.route_seed);
    key.push_back((static_cast<u64>(event.grenade_seen) << 63) | event.grenade_id);
    key.push_back((static_cast<u64>(event.late_result_seen) << 63) | event.late_r31);
    key.push_back(event.late_r29);
    for (const ProbeState& probe : event.money_probes)
    {
      key.push_back((static_cast<u64>(probe.seen) << 63) |
                    (static_cast<u64>(probe.seed) << 32) | probe.r29);
      key.push_back((static_cast<u64>(probe.r30) << 32) | probe.r31);
    }
    key.push_back((static_cast<u64>(event.final_return_seen) << 1) |
                  static_cast<u64>(event.complete));
  }
  key.push_back(outcome.gate_count);
  key.push_back(outcome.completed_count);
  return key;
}

u64 FirstFrame(const Mutation& mutation)
{
  return mutation.slot == 0 ? 0 : mutation.slot - 1;
}

u64 LastFrame(const Mutation& mutation)
{
  return mutation.slot + 2 * (static_cast<u64>(mutation.duration) - 1);
}

bool Compatible(const Mutation& first, const Mutation& second)
{
  if (GetControlGroup(first.kind) != GetControlGroup(second.kind))
    return true;
  return LastFrame(first) < FirstFrame(second) || LastFrame(second) < FirstFrame(first);
}

bool CostsMovement(const Mutation& mutation)
{
  switch (mutation.kind)
  {
  case MutationKind::BOff:
  case MutationKind::LOn:
  case MutationKind::ROn:
  case MutationKind::StickYCenter:
    return true;
  case MutationKind::StickYDelta:
    return mutation.amount < 0;
  default:
    return false;
  }
}

u32 PlanMovementCost(const std::vector<Mutation>& mutations)
{
  std::set<u64> affected_frames;
  for (const Mutation& mutation : mutations)
  {
    if (!CostsMovement(mutation))
      continue;
    const u64 first = std::max(FirstFrame(mutation), s_state.config.movement_first);
    const u64 last = std::min(LastFrame(mutation), s_state.config.movement_last);
    if (first > last)
      continue;
    for (u64 frame = first; frame <= last; ++frame)
      affected_frames.insert(frame);
  }
  return static_cast<u32>(affected_frames.size());
}

std::string MutationName(const Mutation& mutation)
{
  return fmt::format("F{}:{}{}", mutation.slot, KindName(mutation.kind, mutation.amount),
                     mutation.duration > 1 ? fmt::format("x{}", mutation.duration) : "");
}

std::string MakePlanLabel(const std::vector<Mutation>& mutations)
{
  if (mutations.empty())
    return "BASELINE";
  std::string label;
  for (const Mutation& mutation : mutations)
  {
    if (!label.empty())
      label += '+';
    label += MutationName(mutation);
  }
  return label;
}

void NormalizePlan(Plan& plan)
{
  std::vector<Mutation> prepared;
  prepared.reserve(plan.mutations.size());
  for (Mutation mutation : plan.mutations)
  {
    mutation.slot = std::clamp(mutation.slot, s_state.config.window_first,
                               s_state.effective_window_last);
    mutation.slot -= (mutation.slot - s_state.config.window_first) & 1;
    const u64 available_wide = (s_state.effective_window_last - mutation.slot) / 2 + 1;
    const u8 available = static_cast<u8>(std::min<u64>(available_wide, 255));
    mutation.duration = std::clamp<u8>(mutation.duration, 1,
                                       static_cast<u8>(std::min<u32>(s_state.config.max_hold_slots,
                                                                      available)));
    if (mutation.kind == MutationKind::StickXDelta ||
        mutation.kind == MutationKind::StickYDelta ||
        mutation.kind == MutationKind::CStickXDelta ||
        mutation.kind == MutationKind::CStickYDelta)
    {
      if (mutation.amount == 0)
        mutation.amount = 32;
      mutation.amount = std::clamp(mutation.amount, -127, 127);
    }
    else
    {
      mutation.amount = 0;
    }
    prepared.push_back(mutation);
  }

  std::sort(prepared.begin(), prepared.end(), [](const Mutation& first, const Mutation& second) {
    return std::tie(first.slot, first.kind, first.duration, first.amount) <
           std::tie(second.slot, second.kind, second.duration, second.amount);
  });

  std::vector<Mutation> normalized;
  normalized.reserve(std::min<size_t>(prepared.size(), s_state.config.max_plan_mutations));
  for (Mutation mutation : prepared)
  {
    if (!std::all_of(normalized.begin(), normalized.end(), [&](const Mutation& existing) {
          return Compatible(existing, mutation);
        }))
    {
      continue;
    }

    while (mutation.duration > 1)
    {
      std::vector<Mutation> candidate = normalized;
      candidate.push_back(mutation);
      if (PlanMovementCost(candidate) <= s_state.config.max_movement_cost_frames)
        break;
      --mutation.duration;
    }
    std::vector<Mutation> candidate = normalized;
    candidate.push_back(mutation);
    if (PlanMovementCost(candidate) <= s_state.config.max_movement_cost_frames)
      normalized.push_back(mutation);
    if (normalized.size() >= s_state.config.max_plan_mutations)
      break;
  }

  plan.mutations = std::move(normalized);
  plan.movement_cost_frames = PlanMovementCost(plan.mutations);
  plan.label = MakePlanLabel(plan.mutations);
}

Mutation MakeMutation(u64 slot, MutationKind kind, u8 duration = 1, int amount = 0)
{
  return Mutation{slot, duration, kind, amount};
}

Plan MakePlan(int phase, std::vector<Mutation> mutations = {})
{
  Plan plan;
  plan.phase = phase;
  plan.mutations = std::move(mutations);
  NormalizePlan(plan);
  return plan;
}

bool IsFrameInMutation(u64 frame, const Mutation& mutation)
{
  return frame >= FirstFrame(mutation) && frame <= LastFrame(mutation);
}

u8 AddClamped(u8 value, int delta)
{
  return static_cast<u8>(std::clamp(static_cast<int>(value) + delta, 0, 255));
}

void ApplyMutation(Movie::ControllerState& state, const Mutation& mutation)
{
  switch (mutation.kind)
  {
  case MutationKind::BOff:
    state.B = false;
    break;
  case MutationKind::LOn:
    state.L = true;
    state.TriggerL = 255;
    break;
  case MutationKind::ROn:
    state.R = true;
    state.TriggerR = 255;
    break;
  case MutationKind::StickXDelta:
    state.AnalogStickX = AddClamped(state.AnalogStickX, mutation.amount);
    break;
  case MutationKind::StickYDelta:
    state.AnalogStickY = AddClamped(state.AnalogStickY, mutation.amount);
    break;
  case MutationKind::StickYCenter:
    state.AnalogStickY = 128;
    break;
  case MutationKind::CStickXDelta:
    state.CStickX = AddClamped(state.CStickX, mutation.amount);
    break;
  case MutationKind::CStickYDelta:
    state.CStickY = AddClamped(state.CStickY, mutation.amount);
    break;
  }
}

size_t EventCapacity()
{
  if (s_state.config.expected_drops != 0)
    return s_state.config.expected_drops;
  return MAX_DROPS;
}

Outcome MakeOutcome()
{
  Outcome outcome;
  outcome.events.resize(EventCapacity());
  return outcome;
}

std::string WorkerSuffix()
{
  return s_state.config.worker_count > 1 ? fmt::format("_w{}", s_state.config.worker_id) : "";
}

std::string OutputPath(std::string_view suffix)
{
  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  return File::GetUserPath(D_LOGS_IDX) + s_state.config.output_prefix + WorkerSuffix() +
         std::string(suffix);
}

std::string TargetDescription()
{
  std::string description;
  for (const TargetSpec& target : s_state.config.targets)
  {
    if (!description.empty())
      description += " + ";
    description += target.text;
  }
  return description;
}

void OpenResultsLocked()
{
  s_state.results = std::ofstream(OutputPath("_results.csv"), std::ios::out | std::ios::trunc);
  if (!s_state.results)
    return;
  s_state.results.imbue(std::locale::classic());
  s_state.results << "attempt,phase,plan,parent,movement_cost_frames,mutations,completed_drops,";
  for (size_t i = 0; i < EventCapacity(); ++i)
  {
    const size_t drop = i + 1;
    s_state.results << fmt::format(
        "drop{}_frame,drop{}_gate,drop{}_route,drop{}_grenade_id,drop{}_money_p1_seed,"
        "drop{}_money_p2_seed,drop{}_money_p3_seed,drop{}_late_r31,drop{}_late_r29,"
        "drop{}_class,",
        drop, drop, drop, drop, drop, drop, drop, drop, drop, drop);
  }
  s_state.results << "matched_targets,target,fitness,grenade_route,money_route,novel_signature,"
                     "corpus_size,adaptive_points,adaptive_rank,status,elapsed_ms\n";
  s_state.results.flush();
}

void WriteEventCSV(std::ostream& out, const DropEvent& event)
{
  out << fmt::format("{},${:04X},${:04X},${:08X}", event.gate_frame, event.gate_seed,
                     event.route_seed, event.grenade_id);
  for (const ProbeState& probe : event.money_probes)
    out << fmt::format(",${:04X}", probe.seed);
  out << fmt::format(",${:08X},${:08X},{}", event.late_r31, event.late_r29,
                     EventClass(event));
}

void WriteResultLocked(const Plan& plan, const Outcome& outcome, double elapsed_ms)
{
  if (!s_state.results)
    return;
  s_state.results << fmt::format("{},{},{},{},{},{},{},", s_state.attempts, plan.phase,
                                 plan.label, plan.parent_label, plan.movement_cost_frames,
                                 plan.mutations.size(), outcome.completed_count);
  for (const DropEvent& event : outcome.events)
  {
    WriteEventCSV(s_state.results, event);
    s_state.results << ',';
  }
  s_state.results << fmt::format("{},{},{},{},{},{},{},{},{},{},{:.3f}\n",
                                 outcome.matched_targets, outcome.target ? 1 : 0,
                                 outcome.fitness, outcome.has_grenade_route ? 1 : 0,
                                 outcome.has_money_route ? 1 : 0,
                                 outcome.novel_signature ? 1 : 0, s_state.corpus.size(),
                                 outcome.difficulty_points, outcome.difficulty_rank,
                                 outcome.status, elapsed_ms);
  if ((s_state.attempts % 100) == 0 || outcome.target)
    s_state.results.flush();
}

bool WriteWinningDTMLocked(System* system)
{
  if (!system)
    return false;
  const std::string source = system->GetMovie().GetCurrentMoviePath();
  if (source.empty() || !File::Exists(source))
    return false;
  const std::string destination = OutputPath("_FOUND.dtm");
  if (!File::Copy(source, destination, true))
    return false;

  File::IOFile dtm(destination, "rb+");
  if (!dtm)
    return false;
  for (const auto& [input_offset, bytes] : s_state.mutated_inputs)
  {
    if (!dtm.Seek(static_cast<s64>(sizeof(Movie::DTMHeader) + input_offset),
                  File::SeekOrigin::Begin) ||
        !dtm.WriteArray(bytes))
    {
      return false;
    }
  }
  if (!dtm.Flush())
    return false;
  s_state.found_dtm_path = destination;
  return true;
}

void WriteFoundLocked(bool dtm_written)
{
  std::ofstream out(OutputPath("_FOUND.txt"), std::ios::out | std::ios::trunc);
  if (!out)
    return;
  out.imbue(std::locale::classic());
  out << "RE4 JPN GENERIC DROP TARGET FOUND\n";
  out << "Searcher: v7.1 generic profile engine\n";
  out << fmt::format("Profile: {}\n", s_state.config.profile);
  out << fmt::format("Target mode: {}\n", TargetModeName(s_state.config.target_mode));
  out << fmt::format("Target: {}\n", TargetDescription());
  out << fmt::format("Attempt: {}\n", s_state.attempts);
  out << fmt::format("Plan: {}\n", s_state.current_plan.label);
  out << fmt::format("Parent: {}\n", s_state.current_plan.parent_label);
  out << fmt::format("Estimated movement cost: {} / {} emulated frames\n",
                     s_state.current_plan.movement_cost_frames,
                     s_state.config.max_movement_cost_frames);
  out << fmt::format("Mutation window: {}..{}\n", s_state.config.window_first,
                     s_state.effective_window_last);
  out << fmt::format("Adaptive difficulty: points {} | rank {}\n",
                     s_state.current_outcome.difficulty_points,
                     s_state.current_outcome.difficulty_rank);
  for (size_t i = 0; i < s_state.current_outcome.events.size(); ++i)
  {
    const DropEvent& event = s_state.current_outcome.events[i];
    out << fmt::format(
        "Drop {}: {} | frame {} | gate ${:04X} | route ${:04X} | grenade ${:08X} | "
        "late r31 ${:08X} r29 ${:08X}\n",
        i + 1, EventClass(event), event.gate_frame, event.gate_seed, event.route_seed,
        event.grenade_id, event.late_r31, event.late_r29);
  }
  out << fmt::format("Winning DTM: {}\n",
                     dtm_written ? s_state.found_dtm_path : "ERROR: could not write DTM");
  out << "\nF<N> identifies the two-frame TAS slot ending at N; xK holds the change for K "
         "consecutive slots. The generated DTM patches the exact controller polls, including "
         "subframes that cannot be edited conveniently in Dolphin.\n";
}

void WriteSummaryLocked(std::string_view status)
{
  std::ofstream out(OutputPath("_summary.txt"), std::ios::out | std::ios::trunc);
  if (!out)
    return;
  out.imbue(std::locale::classic());
  out << "RE4 JPN GENERIC DROP SEARCH v7.1 SUMMARY\n";
  out << fmt::format("Status: {}\n", status);
  out << fmt::format("Profile: {}\n", s_state.config.profile);
  out << fmt::format("Config: {}\n", s_state.config_path);
  if (!s_state.config_error.empty())
    out << fmt::format("Config error: {}\n", s_state.config_error);
  if (!s_state.validation_error.empty())
    out << fmt::format("Validation error: {}\n", s_state.validation_error);
  out << fmt::format("Attempts: {} / {}\n", s_state.attempts, s_state.config.budget);
  out << fmt::format("Drops per attempt: {}\n", s_state.config.expected_drops);
  out << fmt::format("Target mode: {}\n", TargetModeName(s_state.config.target_mode));
  out << fmt::format("Target: {}\n", TargetDescription());
  out << fmt::format("Capture frame: {}\n", s_state.config.capture_frame);
  out << fmt::format("Effective mutation window: {}..{}\n", s_state.config.window_first,
                     s_state.effective_window_last);
  out << fmt::format("Maximum movement cost: {} frames\n",
                     s_state.config.max_movement_cost_frames);
  out << fmt::format("Unique signatures: {}\n", s_state.seen_signatures.size());
  out << fmt::format("Unique gate seeds: {}\n", s_state.seen_gate_seeds.size());
  out << fmt::format("Unique route seeds: {}\n", s_state.seen_route_seeds.size());
  out << fmt::format("Unique grenade IDs: {}\n", s_state.seen_grenade_ids.size());
  out << fmt::format("Unique ptas values: {}\n", s_state.seen_money_units.size());
  out << fmt::format("Generic corpus: {}\n", s_state.corpus.size());
  out << fmt::format("Grenade-route parents: {}\n", s_state.route_corpus.size());
  out << fmt::format("Money-route parents: {}\n", s_state.money_corpus.size());
  for (size_t i = 0; i < s_state.target_corpora.size(); ++i)
    out << fmt::format("Target {} parents: {}\n", i + 1, s_state.target_corpora[i].size());
  out << "Effective mutation kinds:";
  for (MutationKind kind : s_state.effective_kinds)
    out << ' ' << KindName(kind);
  out << '\n';
  out << fmt::format("Effective calibration slots: {}\n", s_state.effective_slots.size());
  if (!s_state.found_dtm_path.empty())
    out << fmt::format("Winning DTM: {}\n", s_state.found_dtm_path);
}

void InsertFocused(std::vector<CorpusEntry>& corpus, CorpusEntry entry)
{
  if (corpus.size() < MAX_FOCUSED_ENTRIES)
  {
    corpus.push_back(std::move(entry));
    return;
  }
  const auto worst = std::min_element(corpus.begin(), corpus.end(),
                                      [](const CorpusEntry& first, const CorpusEntry& second) {
                                        return first.score < second.score;
                                      });
  if (worst != corpus.end() && entry.score > worst->score)
    *worst = std::move(entry);
}

void RecordCoverageLocked(const Plan& plan, Outcome& outcome)
{
  for (const DropEvent& event : outcome.events)
  {
    if (event.gate_seen)
      s_state.seen_gate_seeds.insert(event.gate_seed);
    if (event.route_seen)
      s_state.seen_route_seeds.insert(event.route_seed);
    if (IsGrenade(event))
      s_state.seen_grenade_ids.insert(event.grenade_id);
    if (IsMoney(event))
      s_state.seen_money_units.insert(event.late_r29);
  }

  outcome.novel_signature = s_state.seen_signatures.insert(Signature(outcome)).second;
  if ((s_state.corpus.empty() || outcome.novel_signature || outcome.matched_targets != 0) &&
      s_state.corpus.size() < MAX_CORPUS_ENTRIES)
  {
    s_state.corpus.push_back({plan, outcome, outcome.fitness});
  }
  if (outcome.has_grenade_route && (outcome.novel_signature || s_state.route_corpus.empty()))
    InsertFocused(s_state.route_corpus, {plan, outcome, outcome.fitness + 500000});
  if (outcome.has_money_route && (outcome.novel_signature || s_state.money_corpus.empty()))
    InsertFocused(s_state.money_corpus, {plan, outcome, outcome.fitness + 500000});

  for (size_t target_index = 0; target_index < s_state.config.targets.size(); ++target_index)
  {
    u64 score = 0;
    for (const DropEvent& event : outcome.events)
      score = std::max(score, TargetCloseness(event, s_state.config.targets[target_index]));
    if (score != 0 && (outcome.novel_signature || s_state.target_corpora[target_index].empty()))
      InsertFocused(s_state.target_corpora[target_index], {plan, outcome, score});
  }
}

u64 NextRandomLocked()
{
  u64 value = s_state.rng_state;
  value ^= value >> 12;
  value ^= value << 25;
  value ^= value >> 27;
  s_state.rng_state = value;
  return value * 0x2545F4914F6CDD1DULL;
}

size_t RandomIndexLocked(size_t count)
{
  return count == 0 ? 0 : static_cast<size_t>(NextRandomLocked() % count);
}

bool ChanceLocked(u32 percent)
{
  return RandomIndexLocked(100) < percent;
}

u8 RandomDurationLocked()
{
  const size_t roll = RandomIndexLocked(100);
  if (roll < 52)
    return 1;
  if (roll < 79)
    return 2;
  if (roll < 91)
    return 3;
  if (roll < 97)
    return 4;
  return static_cast<u8>(s_state.config.max_hold_slots);
}

std::vector<MutationKind> ActiveKindsLocked()
{
  std::vector<MutationKind> kinds;
  for (MutationKind kind : s_state.config.allowed_kinds)
  {
    if (s_state.effective_kinds.contains(kind) || s_state.config.forced_kinds.contains(kind))
      kinds.push_back(kind);
  }
  return kinds.empty() ? s_state.config.allowed_kinds : kinds;
}

u64 ClampAndAlignSlot(u64 slot)
{
  slot = std::clamp(slot, s_state.config.window_first, s_state.effective_window_last);
  return slot - ((slot - s_state.config.window_first) & 1);
}

u64 NearbySlotLocked(u64 anchor)
{
  const s64 delta = (static_cast<s64>(RandomIndexLocked(9)) - 4) * 2;
  if (delta < 0 && static_cast<u64>(-delta) > anchor)
    return s_state.config.window_first;
  return ClampAndAlignSlot(static_cast<u64>(static_cast<s64>(anchor) + delta));
}

u64 RandomSlotLocked(const Plan* parent)
{
  if (parent && !parent->mutations.empty() && ChanceLocked(48))
    return NearbySlotLocked(parent->mutations[RandomIndexLocked(parent->mutations.size())].slot);
  if (!s_state.effective_slots.empty() && ChanceLocked(45))
  {
    auto it = s_state.effective_slots.begin();
    std::advance(it, RandomIndexLocked(s_state.effective_slots.size()));
    return NearbySlotLocked(*it);
  }
  if (!s_state.config.hotspots.empty() && ChanceLocked(55))
    return NearbySlotLocked(s_state.config.hotspots[RandomIndexLocked(s_state.config.hotspots.size())]);
  const size_t slots = static_cast<size_t>((s_state.effective_window_last -
                                            s_state.config.window_first) /
                                               2 +
                                           1);
  return s_state.config.window_first + 2 * RandomIndexLocked(slots);
}

Mutation RandomMutationLocked(const Plan* parent)
{
  const std::vector<MutationKind> kinds = ActiveKindsLocked();
  const MutationKind kind = kinds[RandomIndexLocked(kinds.size())];
  static constexpr std::array<int, 8> main_amounts = {-96, -64, -32, -16, 16, 32, 64, 96};
  static constexpr std::array<int, 6> y_amounts = {-96, -64, -32, -16, 16, 32};
  int amount = 0;
  if (kind == MutationKind::StickYDelta)
    amount = y_amounts[RandomIndexLocked(y_amounts.size())];
  else if (kind == MutationKind::StickXDelta || kind == MutationKind::CStickXDelta ||
           kind == MutationKind::CStickYDelta)
    amount = main_amounts[RandomIndexLocked(main_amounts.size())];
  return MakeMutation(RandomSlotLocked(parent), kind, RandomDurationLocked(), amount);
}

bool AddRandomMutationLocked(Plan& plan, const Plan* parent)
{
  if (plan.mutations.size() >= s_state.config.max_plan_mutations)
    return false;
  for (int attempt = 0; attempt < 64; ++attempt)
  {
    const Mutation mutation = RandomMutationLocked(parent);
    if (std::all_of(plan.mutations.begin(), plan.mutations.end(),
                    [&](const Mutation& existing) { return Compatible(existing, mutation); }))
    {
      const size_t before = plan.mutations.size();
      plan.mutations.push_back(mutation);
      NormalizePlan(plan);
      return plan.mutations.size() > before;
    }
  }
  return false;
}

const CorpusEntry& TournamentLocked(const std::vector<CorpusEntry>& corpus)
{
  size_t best = RandomIndexLocked(corpus.size());
  for (int round = 0; round < 3; ++round)
  {
    const size_t candidate = RandomIndexLocked(corpus.size());
    if (corpus[candidate].score > corpus[best].score)
      best = candidate;
  }
  return corpus[best];
}

bool WantsGrenadeTarget()
{
  return std::any_of(s_state.config.targets.begin(), s_state.config.targets.end(),
                     [](const TargetSpec& target) {
                       return target.kind == TargetKind::Hand ||
                              target.kind == TargetKind::Grenade ||
                              target.kind == TargetKind::AnyGrenade;
                     });
}

bool WantsMoneyTarget()
{
  return std::any_of(s_state.config.targets.begin(), s_state.config.targets.end(),
                     [](const TargetSpec& target) {
                       return target.kind == TargetKind::Ptas ||
                              target.kind == TargetKind::AnyPtas ||
                              (target.kind == TargetKind::Item && target.value == PTAS_ITEM_ID);
                     });
}

Plan SelectParentLocked()
{
  const size_t roll = RandomIndexLocked(100);
  if (roll < 35 && !s_state.target_corpora.empty())
  {
    const size_t start = RandomIndexLocked(s_state.target_corpora.size());
    for (size_t offset = 0; offset < s_state.target_corpora.size(); ++offset)
    {
      const auto& corpus = s_state.target_corpora[(start + offset) % s_state.target_corpora.size()];
      if (!corpus.empty())
        return TournamentLocked(corpus).plan;
    }
  }
  if (roll < 58 && WantsGrenadeTarget() && !s_state.route_corpus.empty())
    return TournamentLocked(s_state.route_corpus).plan;
  if (roll < 81 && WantsMoneyTarget() && !s_state.money_corpus.empty())
    return TournamentLocked(s_state.money_corpus).plan;
  if (roll < 97 && !s_state.corpus.empty())
    return TournamentLocked(s_state.corpus).plan;
  return MakePlan(2);
}

void EditPlanLocked(Plan& plan)
{
  if (plan.mutations.empty())
  {
    AddRandomMutationLocked(plan, &plan);
    return;
  }
  Mutation& mutation = plan.mutations[RandomIndexLocked(plan.mutations.size())];
  switch (RandomIndexLocked(4))
  {
  case 0:
    mutation.slot = RandomSlotLocked(&plan);
    break;
  case 1:
    mutation.duration = RandomDurationLocked();
    break;
  case 2:
  {
    const Mutation replacement = RandomMutationLocked(&plan);
    mutation.kind = replacement.kind;
    mutation.amount = replacement.amount;
    break;
  }
  case 3:
    if (mutation.amount != 0)
      mutation.amount = -mutation.amount;
    else
      mutation.duration = static_cast<u8>(1 + RandomIndexLocked(s_state.config.max_hold_slots));
    break;
  }
  NormalizePlan(plan);
}

void SplicePlanLocked(Plan& plan)
{
  if (s_state.corpus.empty())
  {
    AddRandomMutationLocked(plan, &plan);
    return;
  }
  const Plan& other = TournamentLocked(s_state.corpus).plan;
  if (other.mutations.empty())
  {
    AddRandomMutationLocked(plan, &plan);
    return;
  }
  const size_t additions = 1 + RandomIndexLocked(std::min<size_t>(3, other.mutations.size()));
  for (size_t i = 0; i < additions &&
                     plan.mutations.size() < s_state.config.max_plan_mutations;
       ++i)
  {
    plan.mutations.push_back(other.mutations[RandomIndexLocked(other.mutations.size())]);
  }
  NormalizePlan(plan);
}

bool QueueUniqueLocked(Plan plan)
{
  NormalizePlan(plan);
  if (plan.mutations.empty() || !s_state.known_plan_labels.insert(plan.label).second)
    return false;
  s_state.queue.push_back(std::move(plan));
  return true;
}

bool BuildNextAdaptivePlanLocked()
{
  for (int generation_attempt = 0; generation_attempt < 2048; ++generation_attempt)
  {
    Plan parent = SelectParentLocked();
    Plan candidate = parent;
    candidate.phase = 2;
    candidate.parent_label = parent.label;
    const size_t operation = RandomIndexLocked(100);
    if (operation < 12)
    {
      candidate.mutations.clear();
      candidate.parent_label = "FRESH";
      const size_t count = 1 + RandomIndexLocked(7);
      for (size_t i = 0; i < count; ++i)
        AddRandomMutationLocked(candidate, &candidate);
    }
    else if (operation < 55)
    {
      AddRandomMutationLocked(candidate, &parent);
      if (ChanceLocked(28))
        AddRandomMutationLocked(candidate, &parent);
    }
    else if (operation < 76)
    {
      EditPlanLocked(candidate);
    }
    else if (operation < 87)
    {
      if (!candidate.mutations.empty())
        candidate.mutations.erase(candidate.mutations.begin() +
                                  RandomIndexLocked(candidate.mutations.size()));
    }
    else
    {
      SplicePlanLocked(candidate);
    }

    if (candidate.mutations.empty())
      AddRandomMutationLocked(candidate, &parent);
    NormalizePlan(candidate);
    if (!candidate.mutations.empty() && !s_state.known_plan_labels.contains(candidate.label))
      return QueueUniqueLocked(std::move(candidate));
  }
  return false;
}

void BuildValidationQueueLocked()
{
  Plan first = MakePlan(0);
  first.label = "BASELINE_A";
  Plan second = MakePlan(0);
  second.label = "BASELINE_B";
  s_state.queue.push_back(std::move(first));
  s_state.queue.push_back(std::move(second));
}

int CalibrationAmount(MutationKind kind)
{
  switch (kind)
  {
  case MutationKind::StickXDelta:
  case MutationKind::StickYDelta:
  case MutationKind::CStickXDelta:
  case MutationKind::CStickYDelta:
    return -64;
  default:
    return 0;
  }
}

std::vector<u64> CalibrationSlotsLocked()
{
  std::vector<u64> slots;
  const u64 total = (s_state.effective_window_last - s_state.config.window_first) / 2 + 1;
  if (s_state.config.full_calibration || s_state.config.calibration_anchors >= total)
  {
    for (u64 slot = s_state.config.window_first; slot <= s_state.effective_window_last; slot += 2)
      slots.push_back(slot);
    return slots;
  }
  const u64 anchors = std::min<u64>(s_state.config.calibration_anchors, total);
  for (u64 index = 0; index < anchors; ++index)
  {
    const u64 slot_index = anchors == 1 ? 0 : (index * (total - 1)) / (anchors - 1);
    slots.push_back(s_state.config.window_first + 2 * slot_index);
  }
  for (u64 hotspot : s_state.config.hotspots)
    slots.push_back(ClampAndAlignSlot(hotspot));
  std::sort(slots.begin(), slots.end());
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  return slots;
}

void BuildCalibrationQueueLocked()
{
  for (u64 slot : CalibrationSlotsLocked())
  {
    for (MutationKind kind : s_state.config.allowed_kinds)
    {
      QueueUniqueLocked(MakePlan(1, {MakeMutation(slot, kind, 1, CalibrationAmount(kind))}));
    }
  }
  s_state.calibration_built = true;
}

void RecordCalibrationLocked(const Plan& plan, const Outcome& outcome)
{
  if (plan.phase != 1 || plan.mutations.size() != 1)
    return;
  if (Signature(outcome) != s_state.baseline_signature)
  {
    s_state.effective_kinds.insert(plan.mutations[0].kind);
    s_state.effective_slots.insert(plan.mutations[0].slot);
  }
}

void FinishCalibrationLocked()
{
  if (s_state.calibration_done)
    return;
  for (MutationKind kind : s_state.config.forced_kinds)
    s_state.effective_kinds.insert(kind);
  if (s_state.effective_kinds.empty())
  {
    for (MutationKind kind : s_state.config.allowed_kinds)
      s_state.effective_kinds.insert(kind);
  }
  s_state.calibration_done = true;
  OSD::AddTypedMessage(
      OSD::MessageType::RE4DropSearch,
      fmt::format("RE4 v7.1 calibration DONE | {} kinds | {} active slots",
                  s_state.effective_kinds.size(), s_state.effective_slots.size()),
      OSD::Duration::VERY_LONG, OSD::Color::GREEN);
}

bool ConfigureWindowFromBaselineLocked()
{
  if (s_state.validation_outcomes.size() != 2)
  {
    s_state.validation_error = "expected two neutral validation attempts";
    return false;
  }
  const Outcome& first = s_state.validation_outcomes[0];
  const Outcome& second = s_state.validation_outcomes[1];
  const size_t required = s_state.config.expected_drops != 0 ?
                              s_state.config.expected_drops :
                              std::max<size_t>(1, s_state.config.targets.size());
  if (first.completed_count < required || second.completed_count < required ||
      first.gate_count < required || second.gate_count < required)
  {
    s_state.validation_error = fmt::format(
        "neutral replay did not expose {} complete drops (first {}/{}, second {}/{})", required,
        first.gate_count, first.completed_count, second.gate_count, second.completed_count);
    return false;
  }
  if (Signature(first) != Signature(second))
  {
    s_state.validation_error = "two neutral replays produced different drop signatures";
    return false;
  }
  u64 last_gate_frame = 0;
  for (const DropEvent& event : first.events)
  {
    if (event.gate_seen)
      last_gate_frame = std::max(last_gate_frame, event.gate_frame);
  }
  if (last_gate_frame + 1 < s_state.config.window_first ||
      last_gate_frame > s_state.config.window_max)
  {
    s_state.validation_error = fmt::format(
        "last neutral drop gate was frame {}, outside supported window {}..{}", last_gate_frame,
        s_state.config.window_first - 1, s_state.config.window_max);
    return false;
  }
  u64 rounded_slot = last_gate_frame;
  if (((rounded_slot - s_state.config.window_first) & 1) != 0)
    ++rounded_slot;
  s_state.effective_window_last =
      std::clamp(rounded_slot, s_state.config.window_first, s_state.config.window_max);
  s_state.baseline_signature = Signature(first);
  return true;
}

bool PrepareNextPlanLocked()
{
  if (s_state.attempts >= s_state.config.budget)
    return false;

  if (s_state.queue.empty() && !s_state.validation_done)
  {
    if (!ConfigureWindowFromBaselineLocked())
      return false;
    s_state.validation_done = true;
    OSD::AddTypedMessage(
        OSD::MessageType::RE4DropSearch,
        fmt::format("RE4 v7.1 validation PASSED | {} drops | window {}-{}",
                    s_state.validation_outcomes[0].completed_count,
                    s_state.config.window_first, s_state.effective_window_last),
        OSD::Duration::VERY_LONG, OSD::Color::GREEN);
    BuildCalibrationQueueLocked();
  }

  if (s_state.queue.empty() && s_state.validation_done && s_state.calibration_built)
  {
    FinishCalibrationLocked();
    if (!BuildNextAdaptivePlanLocked())
      return false;
  }
  if (s_state.queue.empty())
    return false;

  s_state.current_plan = std::move(s_state.queue.front());
  s_state.queue.pop_front();
  s_state.current_outcome = MakeOutcome();
  s_state.mutated_inputs.clear();
  s_state.attempt_start = std::chrono::steady_clock::now();
  return true;
}

void FinishSearchLocked(System* system, std::string_view message, u32 color)
{
  s_state.finished = true;
  s_state.active = false;
  s_suppress_trace.store(false, std::memory_order_release);
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, s_state.previous_speed);
  if (s_state.results)
    s_state.results.flush();
  WriteSummaryLocked(message);
  OSD::AddTypedMessage(OSD::MessageType::RE4DropSearch, std::string(message),
                       OSD::Duration::VERY_LONG, color);
  if (system)
    system->GetCPU().Break();
}

bool MovieMatchesConfig(const Movie::MovieManager& movie)
{
  const bool frames_match = s_state.config.expected_movie_frames == 0 ||
                            movie.GetTotalFrames() == s_state.config.expected_movie_frames;
  const bool inputs_match = s_state.config.expected_movie_inputs == 0 ||
                            movie.GetTotalInputCount() == s_state.config.expected_movie_inputs;
  return frames_match && inputs_match;
}

void CaptureOnCPUThread(System* system)
{
  {
    std::lock_guard lock{s_mutex};
    const auto& movie = system->GetMovie();
    if (!movie.IsPlayingInput() || !MovieMatchesConfig(movie))
    {
      s_state.previous_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
      s_state.validation_error = fmt::format(
          "wrong DTM (got {} frames / {} inputs; expected {} / {})", movie.GetTotalFrames(),
          movie.GetTotalInputCount(), s_state.config.expected_movie_frames,
          s_state.config.expected_movie_inputs);
      FinishSearchLocked(system, "RE4 v7.1 FAILED: DTM does not match the active profile",
                         OSD::Color::RED);
      return;
    }
  }

  auto snapshot = ::State::SaveToMemory(*system);
  {
    std::lock_guard lock{s_mutex};
    if (snapshot.empty())
    {
      FinishSearchLocked(system, "RE4 v7.1 FAILED: in-memory snapshot error", OSD::Color::RED);
      return;
    }
    s_state.snapshot = std::move(snapshot);
    s_state.snapshot_ready = true;
    s_state.active = true;
    s_state.previous_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 0.0f);
    s_suppress_trace.store(true, std::memory_order_release);
    OpenResultsLocked();
    BuildValidationQueueLocked();
    s_state.search_start = std::chrono::steady_clock::now();
    if (!PrepareNextPlanLocked())
    {
      FinishSearchLocked(system, "RE4 v7.1 FAILED: could not prepare validation",
                         OSD::Color::RED);
      return;
    }
    OSD::AddTypedMessage(
        OSD::MessageType::RE4DropSearch,
        fmt::format("RE4 DROP SEARCH v7.1 START | {} | F{} | budget {}", s_state.config.profile,
                    s_state.config.capture_frame, s_state.config.budget),
        OSD::Duration::VERY_LONG, OSD::Color::GREEN);
  }
  system->GetCPU().Continue();
}

void RestoreOnCPUThread(System* system)
{
  std::span<u8> snapshot;
  {
    std::lock_guard lock{s_mutex};
    snapshot = std::span<u8>{s_state.snapshot};
  }
  const bool restored = ::State::LoadFromMemory(*system, snapshot);
  {
    std::lock_guard lock{s_mutex};
    s_state.restore_requested = false;
    if (!restored)
    {
      FinishSearchLocked(system, "RE4 v7.1 FAILED: in-memory restore error", OSD::Color::RED);
      return;
    }
    if (!PrepareNextPlanLocked())
    {
      if (!s_state.validation_done)
      {
        FinishSearchLocked(system,
                           fmt::format("RE4 v7.1 validation FAILED | {}",
                                       s_state.validation_error),
                           OSD::Color::RED);
      }
      else
      {
        FinishSearchLocked(
            system,
            fmt::format("RE4 v7.1 DONE | {} attempts | {} signatures", s_state.attempts,
                        s_state.seen_signatures.size()),
            OSD::Color::YELLOW);
      }
      return;
    }
  }
  system->GetCPU().Continue();
}

void CompleteAttempt(System* system, std::string status)
{
  bool found = false;
  bool request_restore = false;
  bool new_grenade_route = false;
  bool new_money_route = false;
  bool dtm_written = false;
  u64 attempts = 0;
  double rate = 0.0;
  std::string found_plan;

  {
    std::lock_guard lock{s_mutex};
    if (!s_state.active || s_state.finished || s_state.restore_requested)
      return;

    s_state.current_outcome.difficulty_points =
        system->GetMemory().Read_U32(s_state.config.adaptive_points_address);
    s_state.current_outcome.difficulty_rank =
        system->GetMemory().Read_U8(s_state.config.adaptive_rank_address);
    UpdateOutcome(s_state.current_outcome);
    s_state.current_outcome.status = std::move(status);
    ++s_state.attempts;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - s_state.attempt_start)
                                  .count();
    const size_t old_route_size = s_state.route_corpus.size();
    const size_t old_money_size = s_state.money_corpus.size();
    RecordCalibrationLocked(s_state.current_plan, s_state.current_outcome);
    RecordCoverageLocked(s_state.current_plan, s_state.current_outcome);
    new_grenade_route = s_state.route_corpus.size() > old_route_size;
    new_money_route = s_state.money_corpus.size() > old_money_size;
    WriteResultLocked(s_state.current_plan, s_state.current_outcome, elapsed_ms);

    if (!s_state.validation_done)
      s_state.validation_outcomes.push_back(s_state.current_outcome);

    if (s_state.current_outcome.target)
    {
      s_state.found = true;
      found = true;
      dtm_written = WriteWinningDTMLocked(system);
      WriteFoundLocked(dtm_written);
      found_plan = s_state.current_plan.label;
    }
    else
    {
      s_state.restore_requested = true;
      request_restore = true;
    }

    attempts = s_state.attempts;
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s_state.search_start)
            .count();
    rate = seconds > 0.0 ? static_cast<double>(attempts) / seconds : 0.0;

    if (new_grenade_route && s_state.route_corpus.size() <= 20)
    {
      OSD::AddTypedMessage(OSD::MessageType::RE4DropSearch,
                           fmt::format("RE4 v7.1 GRENADE ROUTE | attempt {} | parents {}", attempts,
                                       s_state.route_corpus.size()),
                           OSD::Duration::VERY_LONG, OSD::Color::GREEN);
    }
    else if (new_money_route && s_state.money_corpus.size() <= 20)
    {
      OSD::AddTypedMessage(OSD::MessageType::RE4DropSearch,
                           fmt::format("RE4 v7.1 MONEY ROUTE | attempt {} | parents {}", attempts,
                                       s_state.money_corpus.size()),
                           OSD::Duration::VERY_LONG, OSD::Color::CYAN);
    }

    if ((attempts % 100) == 0)
    {
      OSD::AddTypedMessage(
          OSD::MessageType::RE4DropSearch,
          fmt::format("RE4 v7.1 | {} | {:.2f}/s | phase {} | cost {}/{} | corpus {} | G {} | P {}",
                      attempts, rate, s_state.current_plan.phase,
                      s_state.current_plan.movement_cost_frames,
                      s_state.config.max_movement_cost_frames, s_state.corpus.size(),
                      s_state.route_corpus.size(), s_state.money_corpus.size()),
          OSD::Duration::VERY_LONG, OSD::Color::CYAN);
    }
  }

  if (found)
  {
    std::lock_guard lock{s_mutex};
    FinishSearchLocked(system,
                       fmt::format("RE4 v7.1 TARGET FOUND | attempt {} | DTM {} | {}", attempts,
                                   dtm_written ? "OK" : "ERROR", found_plan),
                       OSD::Color::GREEN);
    return;
  }
  if (request_restore)
  {
    system->GetCPU().AddCPUThreadJob([system] { RestoreOnCPUThread(system); });
    system->GetCPU().Break();
  }
}

DropEvent* ActiveDropEvent(Outcome& outcome)
{
  if (outcome.active_event < 0 ||
      outcome.active_event >= static_cast<int>(outcome.events.size()))
  {
    return nullptr;
  }
  return &outcome.events[static_cast<size_t>(outcome.active_event)];
}

void CompleteDropEvent(Outcome& outcome, DropEvent& event)
{
  if (!event.complete)
  {
    event.complete = true;
    ++outcome.completed_count;
  }
}
}  // namespace

void ResetSession()
{
  std::lock_guard lock{s_mutex};
  if (s_state.results.is_open())
    s_state.results.close();
  s_state = SearchState{};
  s_suppress_trace.store(false, std::memory_order_release);

  const bool loaded = LoadConfig(&s_state.config, &s_state.config_path, &s_state.config_error);
  s_state.armed = loaded && s_state.config.enabled;
  s_state.effective_window_last = s_state.config.window_max;
  s_state.rng_state = s_state.config.fuzz_seed;
  s_state.target_corpora.resize(s_state.config.targets.size());
  if (!loaded)
  {
    OSD::AddTypedMessage(OSD::MessageType::RE4DropSearch,
                         fmt::format("RE4 v7.1 CONFIG ERROR | {}", s_state.config_error),
                         OSD::Duration::VERY_LONG, OSD::Color::RED);
    return;
  }
  if (!s_state.config.enabled)
  {
    OSD::AddTypedMessage(OSD::MessageType::RE4DropSearch, "RE4 drop search v7.1 disabled in INI",
                         OSD::Duration::NORMAL, OSD::Color::YELLOW);
    return;
  }
  OSD::AddTypedMessage(
      OSD::MessageType::RE4DropSearch,
      fmt::format("RE4 drop search v7.1 armed | {} | {} drops | target {}",
                  s_state.config.profile, s_state.config.expected_drops, TargetDescription()),
      OSD::Duration::VERY_LONG, OSD::Color::GREEN);
}

void MutateMovieControllerState(u64 movie_frame, u64 movie_input_offset,
                                Movie::ControllerState& state)
{
  std::lock_guard lock{s_mutex};
  if (!s_state.active || s_state.finished)
    return;
  bool changed = false;
  for (const Mutation& mutation : s_state.current_plan.mutations)
  {
    if (IsFrameInMutation(movie_frame, mutation))
    {
      ApplyMutation(state, mutation);
      changed = true;
    }
  }
  if (changed)
  {
    std::array<u8, sizeof(Movie::ControllerState)> bytes{};
    std::memcpy(bytes.data(), &state, sizeof(state));
    s_state.mutated_inputs[movie_input_offset] = bytes;
  }
}

void OnPadPoll(u64 movie_frame)
{
  bool request_capture = false;
  bool close_attempt = false;
  std::string close_status;
  {
    std::lock_guard lock{s_mutex};
    if (s_state.armed && !s_state.capture_requested && !s_state.snapshot_ready &&
        !s_state.finished && movie_frame >= s_state.config.capture_frame)
    {
      s_state.capture_requested = true;
      request_capture = true;
    }
    else if (s_state.active && !s_state.restore_requested && !s_state.finished &&
             movie_frame >= s_state.config.hard_end_frame)
    {
      UpdateOutcome(s_state.current_outcome);
      close_attempt = true;
      close_status = fmt::format("timeout_{}_gates_{}_complete", s_state.current_outcome.gate_count,
                                 s_state.current_outcome.completed_count);
    }
  }

  System* const system = &System::GetInstance();
  if (request_capture)
  {
    system->GetCPU().AddCPUThreadJob([system] { CaptureOnCPUThread(system); });
    system->GetCPU().Break();
  }
  else if (close_attempt)
  {
    CompleteAttempt(system, std::move(close_status));
  }
}

bool OnDropProbe(System* system, u32 pc)
{
  if (!system || !s_suppress_trace.load(std::memory_order_acquire))
    return false;

  bool complete_now = false;
  bool target = false;
  {
    std::lock_guard lock{s_mutex};
    if (!s_state.active || s_state.finished)
      return true;

    Outcome& outcome = s_state.current_outcome;
    const u16 seed = system->GetMemory().Read_U16(RE4RNGTrace::RNG_STATE_ADDRESS);
    const u64 frame = system->GetMovie().GetCurrentFrame();
    const auto& ppc_state = system->GetPowerPC().GetPPCState();

    if (pc == GATE_PC)
    {
      if (DropEvent* previous = ActiveDropEvent(outcome))
        CompleteDropEvent(outcome, *previous);
      const int next_event = outcome.active_event + 1;
      ++outcome.gate_count;
      if (next_event >= static_cast<int>(outcome.events.size()))
      {
        outcome.overflow = true;
      }
      else
      {
        outcome.active_event = next_event;
        DropEvent& event = outcome.events[static_cast<size_t>(next_event)];
        event.gate_seen = true;
        event.gate_frame = frame;
        event.gate_seed = seed;
      }
    }
    else if (DropEvent* event = ActiveDropEvent(outcome))
    {
      if (pc == ROUTE_PC)
      {
        event->route_seen = true;
        event->route_seed = seed;
      }
      else if (pc == GRENADE_FINAL_ID_PC)
      {
        event->grenade_seen = true;
        event->grenade_id = ppc_state.gpr[29];
      }
      else if (const auto probe = std::find(MONEY_PROBE_PCS.begin(), MONEY_PROBE_PCS.end(), pc);
               probe != MONEY_PROBE_PCS.end())
      {
        ProbeState& state = event->money_probes[static_cast<size_t>(probe - MONEY_PROBE_PCS.begin())];
        state.seen = true;
        state.seed = seed;
        state.r29 = ppc_state.gpr[29];
        state.r30 = ppc_state.gpr[30];
        state.r31 = ppc_state.gpr[31];
      }
      else if (pc == LATE_DROP_RESULT_PC)
      {
        event->late_result_seen = true;
        event->late_r31 = ppc_state.gpr[31];
        event->late_r29 = ppc_state.gpr[29];
      }
      else if (pc == DROP_FINAL_RETURN_PC)
      {
        event->final_return_seen = true;
        CompleteDropEvent(outcome, *event);
      }
    }

    UpdateOutcome(outcome);
    target = outcome.target;
    const bool expected_complete = s_state.config.expected_drops != 0 &&
                                   outcome.completed_count >= s_state.config.expected_drops;
    const bool auto_target_complete = s_state.config.expected_drops == 0 && target;
    complete_now = expected_complete || auto_target_complete;
  }

  if (complete_now)
    CompleteAttempt(system, target ? "target" : "drops_complete");
  return true;
}

bool IsSuppressingTrace()
{
  return s_suppress_trace.load(std::memory_order_acquire);
}

}  // namespace Core::RE4RNGSearch
