// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::MusicPlayerService {

struct OrbisMusicPlayerPlayStatusExtension {
    u64 size;                // +0x00 (0x48)
    u32 error_code;          // +0x08 (0: ok, non-zero: error)
    u32 play_state;          // +0x0c (0: stopped, 1: playing, 2: paused)
    u32 total_duration_ms;   // +0x10 (total duration in seconds as expected by PS4 Media Player)
    u32 unk14;               // +0x14
    u32 current_time_ms;     // +0x18 (position in seconds as expected by PS4 Media Player)
    u32 unk1c;               // +0x1c
    u32 volume;              // +0x20 (0 - 100)
    u32 current_track_index; // +0x24 (0-based current track index)
    u32 unk28;               // +0x28
    u32 total_tracks;        // +0x2c (total tracks)
    u32 unk30;               // +0x30
    u32 repeat_mode;         // +0x34 (0: off, 1: all, 2: one)
    u32 shuffle_mode;        // +0x38 (0: off, 1: on)
    u32 unk3c;               // +0x3c
    u32 unk40;               // +0x40
    u32 unk44;               // +0x44
};
static_assert(sizeof(OrbisMusicPlayerPlayStatusExtension) == 0x48);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::MusicPlayerService
