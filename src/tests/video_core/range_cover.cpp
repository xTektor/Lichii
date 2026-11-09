// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/range_cover.h"

using VideoCommon::RangesCover;

TEST_CASE("Ranges cover a span only without gaps", "[video_core][texture_cache]") {
    std::array<std::pair<u64, u64>, 3> covering{{{40, 100}, {0, 50}, {90, 120}}};
    REQUIRE(RangesCover(10, 110, covering));
    std::array<std::pair<u64, u64>, 2> gap{{{0, 50}, {60, 120}}};
    REQUIRE_FALSE(RangesCover(10, 110, gap));
    std::array<std::pair<u64, u64>, 1> short_end{{{0, 100}}};
    REQUIRE_FALSE(RangesCover(10, 110, short_end));
    std::array<std::pair<u64, u64>, 1> late_start{{{20, 200}}};
    REQUIRE_FALSE(RangesCover(10, 110, late_start));
    REQUIRE(RangesCover(5, 5, std::span<std::pair<u64, u64>>{}));
}
