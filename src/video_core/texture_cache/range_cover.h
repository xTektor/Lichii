// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon {

/// Whether the half-open ranges cover [begin, end) without a gap. Sorts ranges.
[[nodiscard]] inline bool RangesCover(u64 begin, u64 end, std::span<std::pair<u64, u64>> ranges) {
    if (begin >= end) {
        return true;
    }
    std::ranges::sort(ranges);
    u64 covered = begin;
    for (const auto& [first, last] : ranges) {
        if (first > covered) {
            return false;
        }
        covered = (std::max)(covered, last);
        if (covered >= end) {
            return true;
        }
    }
    return false;
}

} // namespace VideoCommon
