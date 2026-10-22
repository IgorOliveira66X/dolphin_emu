// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>

namespace Core::RE4RNGTrace
{
struct DropResult
{
  bool selected = false;
  std::uint32_t item = 0;
  std::uint32_t quantity = 0;
};

constexpr bool IsDropOutputAddress(std::uint32_t address)
{
  return address >= 0x80000000 && address <= 0x817ffffc && (address & 3) == 0;
}

// RandomItemCk writes two 32-bit outputs through its second and third arguments.
// A zero quantity requests the game's default pack; it does not mean no item.
template <typename ReadWord>
std::optional<DropResult> ReadDropResult(std::uint32_t result, std::uint32_t item_address,
                                         std::uint32_t quantity_address, ReadWord read_word)
{
  if (result == 0)
    return DropResult{};
  if (result != 1 || !IsDropOutputAddress(item_address) ||
      !IsDropOutputAddress(quantity_address) || item_address == quantity_address)
  {
    return std::nullopt;
  }
  const auto item = read_word(item_address);
  const auto quantity = read_word(quantity_address);
  if (item > 0xffff || quantity > 10000000)
    return std::nullopt;
  return DropResult{true, item, quantity};
}
}  // namespace Core::RE4RNGTrace
