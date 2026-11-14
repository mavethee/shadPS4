// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Core {

struct TscPatchReport {
    u32 found{};
    u32 windows{};
    u32 moved{};
    u32 skipped{};
};

using TscPatchVeto = bool (*)(const u8* instruction, u64 length);

TscPatchReport PatchRdtscInSegment(u8* code, u64 size, u8* trampolines, u64 trampoline_capacity,
                                   u64* trampoline_used, const u64* multiplier,
                                   TscPatchVeto veto = nullptr,
                                   std::vector<u64>* skipped_offsets = nullptr);

} // namespace Core
