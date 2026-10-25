// Direct engine regressions: physical whitelist files, native overwrite, Unicode and long paths.
#include "../src/core/file_ops.h"
#include "../src/core/logger.h"
#include "../src/core/sync_engine.h"
#include "test_isolation.h"
#include "test_fixture.h"
#include <cstdio>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int passed = 0, failed = 0;
constexpr const wchar_t* kRootFiles[] = {
    L"bot_5.dll", L"config.ini", L"hc.dat", L"HID.dll", L"shards.json", L"user.ini"};

// Assertions remain visible even in optimized release test builds.
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passed : ++failed;
}

// Fixture bytes use extended native paths, including paths longer than MAX_PATH.
void Put(const std::wstring& path, const std::string& bytes) {
    if (!fileops::EnsureDir(path.substr(0, path.find_last_of(L'\\'))))
        throw std::runtime_error("Cannot create fixture parent");
    winutil::ScopedHandle file(CreateFileW(testutil::Native(path).c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    if (!file.valid() || !WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                                    &written, nullptr) || written != bytes.size())
        throw std::runtime_error("Cannot write fixture bytes");
}

// Raw inventory includes internal names so unexpected owner/state/temp files cannot hide from assertions.
void RawFiles(const std::wstring& root, const std::wstring& relative, std::set<std::wstring>& files) {
    const auto directory = root + (relative.empty() ? L"" : L"\\" + relative);
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(testutil::Native(directory + L"\\*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot enumerate raw fixture directory");
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..")
            continue;
        const auto child = relative.empty() ? name : relative + L"\\" + name;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            RawFiles(root, child, files);
        else
            files.insert(child);
    } while (FindNextFileW(search, &data));
    const DWORD code = GetLastError();
    FindClose(search);
    if (code != ERROR_NO_MORE_FILES)
        throw std::runtime_error("Incomplete raw fixture enumeration");
}

// No owned-cache preparation: direct jobs must work on an ordinary existing directory.
core::SyncEngine Engine(const std::wstring& root) {
    core::SyncEngine engine;
    engine.gameRoot = root + L"\\game";
    engine.cacheRoot = root + L"\\cache";
    engine.managementRoot = root + L"\\management-must-not-be-created";
    if (!fileops::EnsureDir(engine.gameRoot) || !fileops::EnsureDir(engine.cacheRoot))
        throw std::runtime_error("Cannot create ordinary roots");
    return engine;
}

// Every whitelisted source file is copied, including identical bytes and long Unicode paths.
void TestCopyAndRestore() {
    testutil::Directory directory(L"direct-copy");
    auto engine = Engine(directory.path());
    std::set<std::wstring> expected;
    for (const auto* relative : kRootFiles) {
        Put(engine.gameRoot + L"\\" + relative, "ROOT-BYTES");
        expected.insert(relative);
    }
    const std::wstring unicode = L"saves\\中文 存档 #&\\进度.dat";
    const std::wstring shard = L"shards\\nested\\piece.dat";
    std::wstring longRelative = L"saves\\";
    for (int i = 0; i < 8; ++i)
        longRelative += L"segment-012345678901234567890123456789\\";
    longRelative += L"长路径.dat";
    Put(engine.gameRoot + L"\\" + unicode, "UNICODE-SAVE");
    Put(engine.gameRoot + L"\\" + shard, "SHARD");
    Put(engine.gameRoot + L"\\" + longRelative, "LONG-PATH");
    expected.insert(unicode);
    expected.insert(shard);
    expected.insert(longRelative);
    Put(engine.cacheRoot + L"\\saves\\obsolete.dat", "DELETE-SAVE");
    Put(engine.cacheRoot + L"\\shards\\old.dat", "DELETE-SHARD");
    Put(engine.cacheRoot + L"\\photo.jpg", "KEEP-PHOTO");
    Put(engine.cacheRoot + L"\\projects\\memo.txt", "KEEP-PROJECT");
    Put(engine.gameRoot + L"\\unrelated.dll", "NEVER-COPY");
    bool validProgress = true, sawVerified = false;
    int previous = 0;
    engine.onProgress = [&](int done, int total, const std::wstring& label) {
        validProgress = validProgress && done >= previous && done >= 0 && done <= total;
        previous = done;
        sawVerified = sawVerified || label == L"mirror/verified";
    };
    const auto mirror = engine.RunOnce(true);
    Check(mirror.ran && mirror.success && mirror.errors == 0 && mirror.copied == 9 && mirror.deleted == 2,
          "ordinary unmarked roots mirror all nine managed files and delete managed extras");
    Check(mirror.sourceNonempty && validProgress && sawVerified &&
              engine.progressDone == engine.progressTotal,
          "native operation progress is monotonic and retains the legacy completion label");
    Check(testutil::ReadBytes(engine.cacheRoot + L"\\" + unicode) == "UNICODE-SAVE" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\" + longRelative) == "LONG-PATH" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\" + shard) == "SHARD",
          "Unicode, nested saves/shards and extended-length paths copy exact fixture bytes");
    Check(!fileops::Exists(engine.cacheRoot + L"\\unrelated.dll") &&
              testutil::ReadBytes(engine.cacheRoot + L"\\photo.jpg") == "KEEP-PHOTO" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\projects\\memo.txt") == "KEEP-PROJECT",
          "whitelist excludes unrelated Game files and preserves unrelated cache contents");
    previous = 0;
    sawVerified = false;
    const auto identical = engine.RunOnce(false, true, true);
    Check(identical.success && identical.copied == 9 && identical.skipped == 0 && identical.deleted == 0,
          "manual offline backup copies every identical file again without hash skipping or deletes");
    Check(DeleteFileW(testutil::Native(engine.gameRoot + L"\\" + longRelative).c_str()) != FALSE,
          "remove one long-path Game file before restore");
    Put(engine.gameRoot + L"\\config.ini", "WRONG");
    Put(engine.gameRoot + L"\\saves\\game-only.dat", "KEEP-GAME-ONLY");
    previous = 0;
    bool sawRestore = false;
    engine.onProgress = [&](int done, int total, const std::wstring& label) {
        validProgress = validProgress && done >= previous && done <= total;
        previous = done;
        sawRestore = sawRestore || label == L"restore/verified";
    };
    const auto restore = engine.RestoreDetailed();
    Check(restore.success && restore.copied == 9 && restore.deleted == 0 && sawRestore && validProgress,
          "restore copies all current physical cache files, not just changed or missing files");
    Check(testutil::ReadBytes(engine.gameRoot + L"\\config.ini") == "ROOT-BYTES" &&
              testutil::ReadBytes(engine.gameRoot + L"\\" + longRelative) == "LONG-PATH" &&
              testutil::ReadBytes(engine.gameRoot + L"\\saves\\game-only.dat") == "KEEP-GAME-ONLY",
          "restore overwrites directly, recreates missing long path and never deletes Game extras");
    Check(engine.LastOutcome().success && engine.LastOutcome().copied == restore.copied,
          "LastOutcome retains the actual restore result");
    std::wstring error;
    Check(engine.PrepareManagement(error) && error.empty() &&
              !fileops::Exists(engine.managementRoot),
          "PrepareManagement only validates roots and cache directory, never creates management state");
    expected.insert(L"photo.jpg");
    expected.insert(L"projects\\memo.txt");
    std::set<std::wstring> actual;
    RawFiles(engine.cacheRoot, L"", actual);
    Check(actual == expected && !fileops::Exists(directory.path() + L"\\snap"),
          "raw physical cache contains only expected files: no ownership, state, staging or snapshots");
    Check(engine.ReconcileCacheAbsences({L"saves\\missing.dat"}, error) && error.empty(),
          "deprecated absence reconciliation is a metadata-free compatibility no-op");
}

// Game-only locked entries prove restore does not scan the destination before source-only copies.
void TestRestoreDoesNotInventoryGame() {
    testutil::Directory directory(L"restore-no-game-list");
    auto engine = Engine(directory.path());
    Put(engine.cacheRoot + L"\\config.ini", "CACHE-CONFIG");
    Put(engine.gameRoot + L"\\saves\\locked-unrelated.dat", "LOCKED-GAME");
    winutil::ScopedHandle locked(CreateFileW(
        testutil::Native(engine.gameRoot + L"\\saves\\locked-unrelated.dat").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(locked.valid(), "exclusive Game-only managed file lock is installed");
    const auto restore = engine.RestoreDetailed();
    Check(restore.success && restore.copied == 1 && restore.errors == 0 &&
              testutil::ReadBytes(engine.gameRoot + L"\\config.ini") == "CACHE-CONFIG",
          "locked destination-only managed file cannot obstruct source-only restore");
}

// A complete empty job succeeds; a non-forced offline request does not run or mutate either root.
void TestEmptyAndOffline() {
    testutil::Directory directory(L"direct-empty-offline");
    auto engine = Engine(directory.path());
    const auto empty = engine.RestoreDetailed();
    Check(empty.ran && empty.success && !empty.sourceNonempty && empty.copied == 0 && empty.errors == 0,
          "complete empty physical cache restore is a successful executed no-op");
    Put(engine.gameRoot + L"\\config.ini", "GAME");
    Put(engine.cacheRoot + L"\\config.ini", "CACHE");
    const auto offline = engine.RunOnce(false);
    Check(!offline.ran && !offline.success && !offline.cancelled && offline.errors == 0 &&
              testutil::ReadBytes(engine.cacheRoot + L"\\config.ini") == "CACHE",
          "offline nonforce request is a truthful skipped no-op");
    const auto forced = engine.RunOnce(false, true, true);
    Check(forced.ran && forced.success && forced.copied == 1 &&
              testutil::ReadBytes(engine.cacheRoot + L"\\config.ini") == "GAME",
          "explicit force bypasses only offline scheduling and executes native backup");
}
} // namespace

int main() {
    try {
        TestCopyAndRestore();
        TestRestoreDoesNotInventoryGame();
        TestEmptyAndOffline();
        core::Log().Close();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Fixture failure: %s\n", error.what());
        ++failed;
    }
    std::printf("Direct sync: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
