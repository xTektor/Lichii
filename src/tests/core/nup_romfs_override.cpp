// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nup.h"

namespace {

namespace fs = std::filesystem;

constexpr u64 TitleId = 0x0400486024DEC000ULL;
constexpr u64 OtherTitleId = 0x0400ABCD00010000ULL;

std::string TitleDir(u64 title_id) {
    return fmt::format("{:016X}", title_id);
}

void Put(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << value;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

std::string ReadVirtual(const FileSys::VirtualFile& file) {
    REQUIRE(file != nullptr);
    std::string out(file->GetSize(), '\0');
    if (!out.empty()) {
        REQUIRE(file->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0) == out.size());
    }
    return out;
}

std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

FileSys::VirtualFile FindPath(FileSys::VirtualDir dir, std::string_view path) {
    while (!path.empty() && dir != nullptr) {
        const auto slash = path.find('/');
        const auto name = path.substr(0, slash);
        if (slash == std::string_view::npos) {
            return dir->GetFile(name);
        }
        dir = dir->GetSubdirectory(name);
        path.remove_prefix(slash + 1);
    }
    return nullptr;
}

std::string RomFSFile(const FileSys::VirtualFile& packed, std::string_view path) {
    const auto extracted = FileSys::ExtractRomFS(packed);
    REQUIRE(extracted != nullptr);
    return ReadVirtual(FindPath(extracted, path));
}

class OverrideContext {
public:
    OverrideContext() {
        for (size_t i = 0; i < directories.size(); ++i) {
            previous[i] = Common::FS::GetEdenPath(directories[i].first);
            fs::create_directories(root / directories[i].second);
            Common::FS::SetEdenPath(directories[i].first, root / directories[i].second);
        }
        Put(root / "nand" / "user" / "save" / "sentinel.bin", "save-sentinel");
        const auto dump = root / "Game";
        Put(dump / "Base/ExeFS/main", "main");
        Put(dump / "Base/ExeFS/main.npdm", Npdm(TitleId));
        Put(dump / "Base/RomFS/ui/icon.txt", "BASE");
        Put(dump / "Base/RomFS/ui/keep.txt", "KEEP");
        Put(dump / "updates/1/exefs/main", "update-main");
        Put(dump / "updates/1/exefs/main.npdm", Npdm(TitleId));
        Put(dump / "updates/1/romfs/ui/icon.txt", "UPDATE");
        Put(dump / "updates/1/romfs/ui/added.txt", "ADDED");
        std::string error;
        REQUIRE(FileSys::Nup::PackDump(dump, archive, {}, {}, nullptr, &error) ==
                FileSys::Nup::PackResult::Success);
        INFO(error);
        original_nup = ReadAll(archive);
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~OverrideContext() {
        for (size_t i = 0; i < directories.size(); ++i) {
            Common::FS::SetEdenPath(directories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(TitleId);
        std::error_code error;
        fs::remove_all(root, error);
    }

    OverrideContext(const OverrideContext&) = delete;
    OverrideContext& operator=(const OverrideContext&) = delete;

    FileSys::VirtualFile OpenRuntime(const std::vector<std::string>& disabled = {}) {
        Settings::values.disabled_addons[TitleId] = disabled;
        auto loader = MakeLoader();
        REQUIRE(loader->ReadProgramId(program_id) == Loader::ResultStatus::Success);
        REQUIRE(program_id == TitleId);
        REQUIRE(loader->ApplyRomFSOverrides(system) == Loader::ResultStatus::Success);
        FileSys::RomFSFactory factory{*loader, system.GetContentProvider(),
                                      system.GetFileSystemController()};
        const auto opened = factory.OpenCurrentProcess(program_id);
        REQUIRE(opened != nullptr);
        return opened;
    }

    const fs::path root =
        fs::temp_directory_path() /
        ("nup-override-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path archive = root / "game.nup";
    std::string original_nup;
    Core::System system;

private:
    std::unique_ptr<Loader::AppLoader_NUP> MakeLoader() {
        auto file = system.GetFilesystem()->OpenFile(archive.string(), FileSys::OpenMode::Read);
        REQUIRE(file != nullptr);
        return std::make_unique<Loader::AppLoader_NUP>(std::move(file));
    }

    static constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> directories{{
        {Common::FS::EdenPath::NANDDir, "nand"},
        {Common::FS::EdenPath::SDMCDir, "sdmc"},
        {Common::FS::EdenPath::LoadDir, "load"},
        {Common::FS::EdenPath::DumpDir, "dump"},
    }};
    std::array<fs::path, directories.size()> previous{};
    u64 program_id{};
};

} // namespace

TEST_CASE("Loose NUP runtime RomFS sees one enabled override and restores it when disabled",
          "[nup]") {
    OverrideContext context;
    const auto mod = context.root / "load" / TitleDir(TitleId) / "IconTest" / "romfs";
    Put(mod / "ui/icon.txt", "MOD");
    Put(mod / "ui/only-mod.txt", "ONLY");

    const auto enabled = context.OpenRuntime();
    REQUIRE(RomFSFile(enabled, "ui/icon.txt") == "MOD");
    REQUIRE(RomFSFile(enabled, "ui/keep.txt") == "KEEP");
    REQUIRE(RomFSFile(enabled, "ui/added.txt") == "ADDED");
    REQUIRE(RomFSFile(enabled, "ui/only-mod.txt") == "ONLY");
    REQUIRE(ReadAll(context.archive) == context.original_nup);
    REQUIRE(ReadAll(context.root / "nand/user/save/sentinel.bin") == "save-sentinel");

    const auto disabled = context.OpenRuntime({"IconTest"});
    REQUIRE(RomFSFile(disabled, "ui/icon.txt") == "UPDATE");
    REQUIRE(RomFSFile(disabled, "ui/keep.txt") == "KEEP");
    REQUIRE(RomFSFile(disabled, "ui/added.txt") == "ADDED");
    REQUIRE(FindPath(FileSys::ExtractRomFS(disabled), "ui/only-mod.txt") == nullptr);
    REQUIRE(ReadAll(context.archive) == context.original_nup);
    REQUIRE(ReadAll(context.root / "nand/user/save/sentinel.bin") == "save-sentinel");
}

TEST_CASE("Loose NUP RomFS overrides use the booted title and the documented layer order",
          "[nup]") {
    OverrideContext context;
    Put(context.root / "load" / TitleDir(OtherTitleId) / "IconTest" / "romfs/ui/icon.txt",
        "OTHER");
    REQUIRE(RomFSFile(context.OpenRuntime(), "ui/icon.txt") == "UPDATE");

    Put(context.root / "sdmc/atmosphere/contents" / TitleDir(TitleId) / "romfs/ui/icon.txt",
        "ATMOSPHERE");
    Put(context.root / "load" / TitleDir(TitleId) / "zzz" / "romfs/ui/icon.txt", "NAMED");
    // The Atmosphere directory is named with the title id, which sorts before "zzz".
    REQUIRE(RomFSFile(context.OpenRuntime(), "ui/icon.txt") == "ATMOSPHERE");
    REQUIRE(RomFSFile(context.OpenRuntime({"SDMC"}), "ui/icon.txt") == "NAMED");
    REQUIRE(ReadAll(context.archive) == context.original_nup);
}

TEST_CASE("Loose NUP mod lookup refuses paths that leave the mod directory", "[nup]") {
    OverrideContext context;
    const auto secret = context.root / "secret.txt";
    Put(secret, "secret-bytes");
    const auto romfs = context.root / "load" / TitleDir(TitleId) / "IconTest" / "romfs";
    Put(romfs / "ui/icon.txt", "MOD");

    FileSys::RealVfsFilesystem vfs;
    const auto dir = vfs.OpenDirectory(romfs.string(), FileSys::OpenMode::Read);
    REQUIRE(dir != nullptr);
    REQUIRE(dir->GetFileRelative("../../../../secret.txt") == nullptr);
    REQUIRE(dir->GetFileRelative("../../../secret.txt") == nullptr);
    REQUIRE(ReadAll(secret) == "secret-bytes");

    std::error_code error;
    fs::create_symlink(secret, romfs / "leak.txt", error);
    if (error) {
        SKIP("this environment cannot create a symlink");
    }
    const auto opened = context.OpenRuntime();
    REQUIRE(FindPath(FileSys::ExtractRomFS(opened), "leak.txt") == nullptr);
    REQUIRE(RomFSFile(opened, "ui/icon.txt") == "MOD");
    REQUIRE(ReadAll(secret) == "secret-bytes");
    REQUIRE(ReadAll(context.archive) == context.original_nup);
    REQUIRE(ReadAll(context.root / "nand/user/save/sentinel.bin") == "save-sentinel");
}
