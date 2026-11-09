// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/command_entry_wait.h"

TEST_CASE("A command entry between methods does not wait for compute writers",
          "[gpu][command_list]") {
    CHECK_FALSE(Tegra::CommandEntryWaitsForWriters(0, false));
    CHECK(Tegra::CommandEntryWaitsForWriters(2, false));
    CHECK(Tegra::CommandEntryWaitsForWriters(1, false));
    CHECK(Tegra::CommandEntryWaitsForWriters(0, true));
    CHECK(Tegra::CommandEntryWaitsForWriters(3, true));
}
