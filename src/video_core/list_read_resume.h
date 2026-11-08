// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>

namespace Tegra {

/// A channel suspended until the GPU finishes the copy of a command list it is about to read.
/// The resumed read skips the writer wait: that wait already ran, and running it again would also
/// wait for other channels' work queued during the suspension. The flag lasts for one read. If
/// the step returns before that read, it is cleared so a later list does not skip its wait.
struct ListReadResume {
    bool skip_writer_wait{};

    void Arm() {
        skip_writer_wait = true;
    }

    /// True only for the read that was suspended. Consumes the flag.
    bool Consume() {
        return std::exchange(skip_writer_wait, false);
    }

    void Clear() {
        skip_writer_wait = false;
    }
};

} // namespace Tegra
