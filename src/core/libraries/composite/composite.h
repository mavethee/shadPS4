// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/kernel/threads.h"
#include "core/loader/symbols_resolver.h"

namespace Libraries::Composite {

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Composite
