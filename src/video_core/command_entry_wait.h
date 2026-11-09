// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Tegra {

/// Whether the front waits for queued compute and copy work before fetching an entry that is
/// not yet on a GPU-written page. An entry that begins between methods is a command buffer the
/// CPU writes. The words GPU work writes for a later fetch are parameters of a method begun in
/// the entry before, so that entry still has method words left. The copy engine can place
/// GPU-written words anywhere, so a queued copy still waits.
[[nodiscard]] constexpr bool CommandEntryWaitsForWriters(u32 method_count_left,
                                                         bool copy_writers_pending) {
    return method_count_left != 0 || copy_writers_pending;
}

} // namespace Tegra
