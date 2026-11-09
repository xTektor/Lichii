// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/list_read_resume.h"

TEST_CASE("A resumed command-list read skips the writer wait once", "[gpu][command_list]") {
    Tegra::ListReadResume resume;
    CHECK_FALSE(resume.Consume());
    resume.Arm();
    CHECK(resume.Consume());
    CHECK_FALSE(resume.Consume());
}

TEST_CASE("Leaving a step before the list read does not skip a later writer wait",
          "[gpu][command_list]") {
    Tegra::ListReadResume resume;
    resume.Arm();
    resume.Clear();
    CHECK_FALSE(resume.Consume());
}
