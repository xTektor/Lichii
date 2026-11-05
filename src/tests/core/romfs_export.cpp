// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "core/file_sys/content_archive.h"

#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_export.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
#include "core/loader/nup.h"

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("romfs-export-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void Put(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << value;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

std::string ReadFile(const FileSys::VirtualFile& file) {
    std::string out(file->GetSize(), '\0');
    if (!out.empty()) {
        file->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0);
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

void Flatten(const FileSys::VirtualDir& dir, const std::string& prefix,
             std::map<std::string, std::string>& out) {
    REQUIRE(dir != nullptr);
    for (const auto& file : dir->GetFiles()) {
        out[prefix + file->GetName()] = ReadFile(file);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        Flatten(sub, prefix + sub->GetName() + "/", out);
    }
}

std::map<std::string, std::string> Flatten(const FileSys::VirtualDir& dir) {
    std::map<std::string, std::string> out;
    Flatten(dir, "", out);
    return out;
}

std::map<std::string, std::string> HostTree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code error;
    if (!fs::exists(dir, error)) {
        return out;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            out[fs::relative(entry.path(), dir).generic_string()] = ReadAll(entry.path());
        }
    }
    return out;
}

class ShortFile final : public FileSys::VectorVfsFile {
public:
    explicit ShortFile(std::string name)
        : VectorVfsFile(std::vector<u8>{'n', 'o'}, std::move(name)) {}

    std::size_t Read(u8*, std::size_t, std::size_t) const override {
        return 0;
    }
};

class LimitingFile final : public FileSys::VfsFile {
public:
    LimitingFile(FileSys::VirtualFile inner_, std::size_t readable_bytes_)
        : inner(std::move(inner_)), readable_bytes(readable_bytes_) {}

    std::string GetName() const override {
        return inner->GetName();
    }
    std::size_t GetSize() const override {
        return inner->GetSize();
    }
    bool Resize(std::size_t) override {
        return false;
    }
    FileSys::VirtualDir GetContainingDirectory() const override {
        return inner->GetContainingDirectory();
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= readable_bytes) {
            return 0;
        }
        return inner->Read(data, std::min(length, readable_bytes - offset), offset);
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view name) override {
        return inner->Rename(name);
    }

private:
    FileSys::VirtualFile inner;
    std::size_t readable_bytes;
};

struct InstalledRow {
    FileSys::TitleType title_type{};
    FileSys::ContentProviderEntry entry{};
    bool readable{true};
};

class FakeInstalled final : public FileSys::ContentProvider {
public:
    void Refresh() override {}
    bool HasEntry(u64, FileSys::ContentRecordType) const override {
        return false;
    }
    std::optional<u32> GetEntryVersion(u64) const override {
        return std::nullopt;
    }
    FileSys::VirtualFile GetEntryUnparsed(u64, FileSys::ContentRecordType) const override {
        return nullptr;
    }
    FileSys::VirtualFile GetEntryRaw(u64, FileSys::ContentRecordType) const override {
        return nullptr;
    }
    std::unique_ptr<FileSys::NCA> GetEntry(u64, FileSys::ContentRecordType) const override {
        return nullptr;
    }
    std::vector<FileSys::ContentProviderEntry> ListEntriesFilter(
        std::optional<FileSys::TitleType> title_type,
        std::optional<FileSys::ContentRecordType> record_type,
        std::optional<u64> title_id) const override {
        std::vector<FileSys::ContentProviderEntry> out;
        for (const auto& row : rows) {
            if (title_type && *title_type != row.title_type) {
                continue;
            }
            if (record_type && *record_type != row.entry.type) {
                continue;
            }
            if (title_id && *title_id != row.entry.title_id) {
                continue;
            }
            out.push_back(row.entry);
        }
        return out;
    }

    std::vector<InstalledRow> rows;
};

constexpr u64 TitleId = 0x0400486024DEC000ULL;

FileSys::VirtualFile Bytes(std::string_view name, std::string_view data) {
    return std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>(data.begin(), data.end()), std::string{name});
}

} // namespace

TEST_CASE("RomFS dump prefers installed content and otherwise a loose NUP", "[romfs]") {
    REQUIRE(FileSys::RouteRomFSDump(true, true) == FileSys::RomFSDumpRoute::Installed);
    REQUIRE(FileSys::RouteRomFSDump(true, false) == FileSys::RomFSDumpRoute::Installed);
    REQUIRE(FileSys::RouteRomFSDump(false, true) == FileSys::RomFSDumpRoute::LooseNup);
    REQUIRE(FileSys::RouteRomFSDump(false, false) == FileSys::RomFSDumpRoute::Unavailable);
}

TEST_CASE("Installed RomFS dump targets ignore other titles and unreadable entries", "[romfs]") {
    FakeInstalled installed;
    installed.rows.push_back(InstalledRow{
        FileSys::TitleType::Application,
        {TitleId, FileSys::ContentRecordType::Program},
        true,
    });
    installed.rows.push_back(InstalledRow{
        FileSys::TitleType::Application,
        {TitleId, FileSys::ContentRecordType::HtmlDocument},
        true,
    });
    installed.rows.push_back(InstalledRow{
        FileSys::TitleType::Application,
        {TitleId, FileSys::ContentRecordType::LegalInformation},
        false,
    });
    installed.rows.push_back(InstalledRow{
        FileSys::TitleType::AOC,
        {TitleId + 0x1000, FileSys::ContentRecordType::Data},
        true,
    });
    installed.rows.push_back(InstalledRow{
        FileSys::TitleType::Application,
        {0x0400ABCD00010000ULL, FileSys::ContentRecordType::Program},
        true,
    });

    const auto targets = FileSys::ListInstalledRomFSDumpTargets(
        installed, TitleId, [&](const FileSys::ContentProviderEntry& entry) {
            for (const auto& row : installed.rows) {
                if (row.entry.title_id == entry.title_id && row.entry.type == entry.type) {
                    return row.readable;
                }
            }
            return false;
        });

    REQUIRE(targets.size() == 3);
    REQUIRE(targets[0].title_id == TitleId);
    REQUIRE(targets[0].record_type == FileSys::ContentRecordType::Program);
    REQUIRE(targets[1].record_type == FileSys::ContentRecordType::HtmlDocument);
    REQUIRE(targets[2].title_id == TitleId + 0x1000);
    REQUIRE(targets[2].title_type == FileSys::TitleType::AOC);
    REQUIRE(FileSys::ListInstalledRomFSDumpTargets(installed, 0x0100, [](const auto&) {
                return true;
            }).empty());
}

TEST_CASE("Loose NUP export matches the loader RomFS and leaves the archive unchanged", "[romfs]") {
    TempDir temp;
    const auto root = temp.path / "Game";
    Put(root / "Base/ExeFS/main", "base main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/keep.txt", "base-keep");
    Put(root / "Base/RomFS/replaced.txt", "base-replaced");
    Put(root / "Base/RomFS/sub/nested.txt", "nested");
    Put(root / "Base/RomFS/empty.txt", "");
    Put(root / "updates/65536/exefs/main", "old main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/old.txt", "from-older-update");
    Put(root / "updates/131072/exefs/main", "new main");
    Put(root / "updates/131072/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/131072/romfs/replaced.txt", "update-replaced");
    Put(root / "updates/131072/romfs/new.txt", "from-newest-update");
    Put(root / "dlc/0400486024DED001/0/romfs/addon.txt", "addon");

    const auto archive = temp.path / "game.nup";
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, {}, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    const auto before = ReadAll(archive);

    FileSys::RealVfsFilesystem real;
    const auto file = real.OpenFile(archive.string(), FileSys::OpenMode::Read);
    auto loose = FileSys::OpenLooseNupProgramRomFS(file);
    REQUIRE(loose.recognized);
    REQUIRE(loose.ok);
    REQUIRE(loose.title_id == TitleId);
    REQUIRE(loose.update_layered);
    REQUIRE(loose.update_version == 131072);

    Loader::AppLoader_NUP loader{file};
    FileSys::VirtualFile packed;
    REQUIRE(loader.ReadRomFS(packed) == Loader::ResultStatus::Success);
    REQUIRE(Flatten(loose.root) == Flatten(FileSys::ExtractRomFS(packed)));

    const std::map<std::string, std::string> expected{{"keep.txt", "base-keep"},
                                                      {"replaced.txt", "update-replaced"},
                                                      {"sub/nested.txt", "nested"},
                                                      {"empty.txt", ""},
                                                      {"new.txt", "from-newest-update"}};
    REQUIRE(Flatten(loose.root) == expected);

    FileSys::RomFSExportOptions options;
    options.destination = temp.path / "dump" / "romfs";
    options.protect_source = archive;
    const auto exported = FileSys::ExportRomFSTree(loose.root, options);
    REQUIRE(exported.code == FileSys::RomFSExportCode::Success);
    REQUIRE(exported.files_written == expected.size());
    REQUIRE(HostTree(options.destination) == expected);
    REQUIRE(ReadAll(archive) == before);
    REQUIRE(fs::file_size(archive) == before.size());
}

TEST_CASE("Malformed and non-NUP files are refused before export", "[romfs]") {
    const auto garbage = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{'n', 'o'}, "no.nup");
    const auto not_nup = FileSys::OpenLooseNupProgramRomFS(garbage);
    REQUIRE_FALSE(not_nup.recognized);
    REQUIRE_FALSE(not_nup.ok);
    REQUIRE(not_nup.detail.find("not a NUP") != std::string::npos);

    std::vector<u8> header(0x80, 0);
    header[0] = 'N';
    header[1] = 'U';
    header[2] = 'P';
    header[3] = 0x01;
    const auto malformed = FileSys::OpenLooseNupProgramRomFS(
        std::make_shared<FileSys::VectorVfsFile>(header, "bad.nup"));
    REQUIRE(malformed.recognized);
    REQUIRE_FALSE(malformed.ok);
    REQUIRE(malformed.detail.find("could not be read") != std::string::npos);
    REQUIRE(malformed.root == nullptr);
}

TEST_CASE("RomFS export rejects unsafe names, short reads, cancellation, and save paths",
          "[romfs]") {
    TempDir temp;
    const auto source = temp.path / "source.nup";
    Put(source, "source-bytes");
    const auto source_before = ReadAll(source);

    SECTION("unsafe names write nothing") {
        const auto dest = temp.path / "out";
        const auto escaped = temp.path / "escape.txt";
        Put(escaped, "sentinel");
        auto file = Bytes("../escape.txt", "nope");
        auto root = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{file}, std::vector<FileSys::VirtualDir>{}, "romfs");
        FileSys::RomFSExportOptions options;
        options.destination = dest;
        options.protect_source = source;
        const auto exported = FileSys::ExportRomFSTree(root, options);
        REQUIRE(exported.code == FileSys::RomFSExportCode::UnsafePath);
        REQUIRE(exported.detail.find("../escape.txt") != std::string::npos);
        REQUIRE_FALSE(fs::exists(dest));
        REQUIRE(ReadAll(escaped) == "sentinel");
        REQUIRE(ReadAll(source) == source_before);
    }

    SECTION("short read removes only files this export created") {
        const auto dest = temp.path / "partial";
        fs::create_directories(dest);
        Put(dest / "keep.txt", "keep");
        auto root = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{Bytes("ok.txt", "yes"),
                                              std::make_shared<ShortFile>("bad.txt")},
            std::vector<FileSys::VirtualDir>{}, "romfs");
        FileSys::RomFSExportOptions options;
        options.destination = dest;
        options.protect_source = source;
        const auto exported = FileSys::ExportRomFSTree(root, options);
        REQUIRE(exported.code == FileSys::RomFSExportCode::ShortRead);
        REQUIRE(exported.files_written == 0);
        REQUIRE(ReadAll(dest / "keep.txt") == "keep");
        REQUIRE_FALSE(fs::exists(dest / "ok.txt"));
        REQUIRE_FALSE(fs::exists(dest / "bad.txt"));
        REQUIRE(ReadAll(source) == source_before);
    }

    SECTION("skeleton does not read file bytes") {
        auto root = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{std::make_shared<ShortFile>("bad.txt")},
            std::vector<FileSys::VirtualDir>{std::make_shared<FileSys::VectorVfsDirectory>(
                std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "sub")},
            "romfs");
        FileSys::RomFSExportOptions options;
        options.destination = temp.path / "skeleton";
        options.full = false;
        options.protect_source = source;
        const auto exported = FileSys::ExportRomFSTree(root, options);
        REQUIRE(exported.code == FileSys::RomFSExportCode::Success);
        REQUIRE(exported.files_written == 0);
        REQUIRE(exported.bytes_written == 0);
        REQUIRE(fs::is_directory(options.destination / "sub"));
        REQUIRE_FALSE(fs::exists(options.destination / "bad.txt"));
    }

    SECTION("cancel before writing leaves no directory") {
        auto root = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{Bytes("a.txt", "alpha")},
            std::vector<FileSys::VirtualDir>{}, "romfs");
        FileSys::RomFSExportOptions options;
        options.destination = temp.path / "cancelled";
        options.protect_source = source;
        const auto exported =
            FileSys::ExportRomFSTree(root, options, [](u64, u64) { return false; });
        REQUIRE(exported.code == FileSys::RomFSExportCode::Cancelled);
        REQUIRE_FALSE(fs::exists(options.destination));
        REQUIRE(ReadAll(source) == source_before);
    }

    SECTION("cancel after the first file removes the partial export") {
        auto root = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{Bytes("a.txt", "alpha"), Bytes("b.txt", "beta")},
            std::vector<FileSys::VirtualDir>{}, "romfs");
        FileSys::RomFSExportOptions options;
        options.destination = temp.path / "cancelled-mid";
        options.protect_source = source;
        int calls = 0;
        const auto exported = FileSys::ExportRomFSTree(root, options, [&](u64, u64) {
            return ++calls < 4;
        });
        REQUIRE(exported.code == FileSys::RomFSExportCode::Cancelled);
        REQUIRE_FALSE(fs::exists(options.destination));
        REQUIRE_FALSE(fs::exists(options.destination / "a.txt"));
        REQUIRE(ReadAll(source) == source_before);
    }

    SECTION("save directories and the source file are not export destinations") {
        const auto save = temp.path / "nand" / "user" / "save";
        fs::create_directories(save);
        Put(save / "save.dat", "save");
        auto root = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{Bytes("a.txt", "alpha")},
            std::vector<FileSys::VirtualDir>{}, "romfs");
        FileSys::RomFSExportOptions into_save;
        into_save.destination = save / "romfs";
        into_save.protect_source = source;
        into_save.protected_directories.push_back(save);
        const auto saved = FileSys::ExportRomFSTree(root, into_save);
        REQUIRE(saved.code == FileSys::RomFSExportCode::ProtectedPath);
        REQUIRE(ReadAll(save / "save.dat") == "save");
        REQUIRE_FALSE(fs::exists(into_save.destination));

        FileSys::RomFSExportOptions onto_source;
        onto_source.destination = source;
        onto_source.protect_source = source;
        const auto overwritten = FileSys::ExportRomFSTree(root, onto_source);
        REQUIRE(overwritten.code == FileSys::RomFSExportCode::ProtectedPath);
        REQUIRE(ReadAll(source) == source_before);
    }
}

TEST_CASE("A truncated NUP extent fails the export and keeps the archive", "[romfs]") {
    TempDir temp;
    const auto root = temp.path / "Game";
    Put(root / "Base/ExeFS/main", "main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/big.bin", std::string(64 * 1024, 'b'));
    const auto archive = temp.path / "game.nup";
    FileSys::Nup::PackOptions pack_options;
    pack_options.raw_threshold = 0;
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, pack_options, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    const auto before = ReadAll(archive);

    FileSys::RealVfsFilesystem real;
    const auto file = real.OpenFile(archive.string(), FileSys::OpenMode::Read);
    REQUIRE(file->GetSize() > 0x40);
    u64 blob_offset = 0;
    REQUIRE(file->ReadObject(&blob_offset, 0x38) == sizeof(blob_offset));
    REQUIRE(blob_offset > 0x80);
    REQUIRE(blob_offset < file->GetSize());
    const auto readable = static_cast<std::size_t>(blob_offset + (file->GetSize() - blob_offset) / 2);
    auto limited = std::make_shared<LimitingFile>(file, readable);

    auto loose = FileSys::OpenLooseNupProgramRomFS(limited);
    if (!loose.ok) {
        REQUIRE(loose.recognized);
        REQUIRE(loose.detail.find("could not be read") != std::string::npos);
    } else {
        FileSys::RomFSExportOptions options;
        options.destination = temp.path / "truncated";
        options.protect_source = archive;
        const auto exported = FileSys::ExportRomFSTree(loose.root, options);
        REQUIRE(exported.code == FileSys::RomFSExportCode::ShortRead);
        REQUIRE(exported.files_written == 0);
        REQUIRE_FALSE(fs::exists(options.destination / "big.bin"));
    }
    REQUIRE(ReadAll(archive) == before);
}
