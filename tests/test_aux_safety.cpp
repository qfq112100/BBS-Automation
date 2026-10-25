// Isolated auxiliary regression tests. No Service, registry writes, known-folder creation,
// real game execution, network, fixed scratch paths, or preexisting-directory cleanup.
#include "../src/common/utf.h"
#include "../src/common/winutil.h"
#include "../src/core/config.h"
#include "../src/core/game_detect.h"
#include "../src/core/logger.h"
#include "../src/core/monitor.h"
#include "../third_party/json/json.hpp"
#include "private/aux_test_hooks.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <windows.h>
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
void WriteBytes(const std::wstring& path, const std::string& bytes) {
    winutil::ScopedHandle file(CreateFileW(winutil::LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid())
        throw std::runtime_error("write fixture open failed");
    DWORD count = 0;
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
        count != bytes.size())
        throw std::runtime_error("write fixture failed");
}
std::string ReadBytes(const std::wstring& path) {
    winutil::ScopedHandle file(CreateFileW(winutil::LongPath(path).c_str(), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid())
        throw std::runtime_error("read fixture open failed");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > 4 * 1024 * 1024)
        throw std::runtime_error("read fixture size failed");
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    if (!bytes.empty() &&
        (!ReadFile(file.get(), &bytes[0], static_cast<DWORD>(bytes.size()), &count, nullptr) ||
         count != bytes.size()))
        throw std::runtime_error("read fixture failed");
    return bytes;
}
class Sandbox {
    std::wstring prefix;

  public:
    std::wstring root;
    Sandbox() {
        std::vector<wchar_t> temp(32768);
        DWORD n = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (!n || n >= temp.size())
            throw std::runtime_error("GetTempPath failed");
        prefix.assign(temp.data(), n);
        for (unsigned i = 0; i < 100; ++i) {
            root = prefix + L"LOLBackupV3-aux-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i);
            if (CreateDirectoryW(winutil::LongPath(root).c_str(), nullptr)) {
                WriteBytes(root + L"\\.aux-test-owned", "isolated auxiliary test root\n");
                return;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::runtime_error("unique temp root creation failed");
        }
        throw std::runtime_error("unique temp root exhausted");
    }
    ~Sandbox() {
        // Only remove the newly-created unique root, with explicit ownership and parent checks.
        if (root.compare(0, prefix.size(), prefix) != 0 || root.size() <= prefix.size() + 16 ||
            root.find(L"LOLBackupV3-aux-", prefix.size()) != prefix.size())
            return;
        DWORD attributes = GetFileAttributesW(winutil::LongPath(root + L"\\.aux-test-owned").c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return;
        std::error_code error;
        fs::remove_all(fs::path(winutil::LongPath(root)), error);
        if (error)
            std::cerr << "WARNING: owned temp root cleanup failed\n";
    }
    std::wstring Path(const wchar_t* name) const {
        return root + L"\\" + name;
    }
};
class Child {
    winutil::ScopedHandle process;

  public:
    Child(const std::wstring& exe, const std::wstring& args) {
        std::wstring command = L"\"" + exe + L"\" " + args;
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(0);
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &start, &info))
            throw std::runtime_error("child creation failed");
        process.reset(info.hProcess);
        CloseHandle(info.hThread);
    }
    void Wait() {
        if (WaitForSingleObject(process.get(), 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("child timeout");
        DWORD code = 1;
        if (!GetExitCodeProcess(process.get(), &code) || code)
            throw std::runtime_error("child failed");
    }
    ~Child() {
        if (process.valid() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
            // Only this test-owned process may be terminated during exceptional cleanup.
            TerminateProcess(process.get(), 87);
            WaitForSingleObject(process.get(), 5000);
        }
    }
};
bool Increment(core::Config& config, void*) {
    ++config.winX;
    return true;
}
bool SetTheme(core::Config& config, void*) {
    config.theme = 1;
    return true;
}
bool InvalidMutation(core::Config& config, void*) {
    config.syncMinutes = 0;
    return true;
}
bool ThrowMutation(core::Config&, void*) {
    throw std::runtime_error("injected callback failure");
}
bool ExternalMutation(core::Config& config, void* path) {
    WriteBytes(*static_cast<std::wstring*>(path), "{\"theme\":0,\"external\":true}");
    config.theme = 1;
    return true;
}
void TestUtf() {
    std::wstring wide = L"unchanged";
    std::string bytes = "unchanged";
    Check(TryUtf8ToWide(u8"目录/🎮", wide), "valid UTF8");
    Check(TryWideToUtf8(wide, bytes) && bytes == u8"目录/🎮", "Unicode roundtrip");
    wide = L"unchanged";
    Check(!TryUtf8ToWide(std::string("\xC0\xAF", 2), wide) && wide == L"unchanged",
          "overlong UTF8 rejected atomically");
    Check(!TryUtf8ToWide(std::string("\xED\xA0\x80", 3), wide), "UTF8 surrogate rejected");
    Check(!TryUtf8ToWide(std::string("\xF4\x90\x80\x80", 4), wide), "out of Unicode range rejected");
    bytes = "unchanged";
    Check(!TryWideToUtf8(std::wstring(1, static_cast<wchar_t>(0xD800)), bytes) && bytes == "unchanged",
          "unpaired UTF16 rejected atomically");
    Check(Utf8ToWide(std::string("\xFF", 1)).empty(), "legacy UTF8 API fails closed");
    Check(WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xDC00))).empty(), "legacy UTF16 API fails closed");
    Check(TryUtf8ToWide(std::string("a\0b", 3), wide) && wide.size() == 3 && wide[1] == 0,
          "Unicode converter preserves NUL; path boundary rejects it");
}
void TestParse() {
    core::Config config;
    std::wstring error;
    Check(core::Config::Parse("{}", config, &error), "missing legacy fields migrate");
    Check(!core::Config::Parse(std::string(1000, '[') + "0" + std::string(1000, ']'), config, &error),
          "deep nesting rejected before stack exhaustion");
    Check(config.syncMinutes == 5, "missing defaults");
    Check(core::Config::Parse("\xEF\xBB\xBF{\"theme\":1}", config), "BOM accepted");
    config.theme = 1;
    const char* invalid[] = {"[]",
                             "null",
                             "{",
                             "{\"theme\":null}",
                             "{\"auto_on\":1}",
                             "{\"sync_minutes\":1.0}",
                             "{\"theme\":2}",
                             "{\"sync_minutes\":0}",
                             "{\"win_w\":-1}",
                             "{\"sync_minutes\":4294967297}",
                             "{\"win_x\":2147483648}",
                             "{\"win_x\":-2147483649}",
                             "{\"win_x\":18446744073709551615}",
                             "{\"game_root\":\"C:\\\\safe\\u0000\\\\child\"}",
                             "{\"cache_root\":\"C:\\\\safe\\nchild\"}",
                             "{\"theme\":0,\"theme\":1}",
                             "{\"extension\":{\"x\":1,\"x\":2}}"};
    for (auto raw : invalid) {
        Check(!core::Config::Parse(raw, config, &error) && !error.empty(),
              "invalid config rejected with friendly error");
        Check(config.theme == 1, "failed parse leaves output unchanged");
    }
    Check(core::Config::Parse(
              "{\"win_x\":-2147483648,\"win_y\":2147483647,\"sync_minutes\":1440,\"snapshot_hours\":168}",
              config),
          "integer endpoints accepted");
    Check(!core::Config::Parse("{\"sync_minutes\":0}", config, &error) &&
              error.find(L"sync_minutes") != std::wstring::npos,
          "friendly config error identifies field");
    std::wstring threadError;
    std::thread isolated([&] {
        core::Config other;
        core::Config::Parse("{\"theme\":null}", other);
        threadError = core::Config::LastError();
    });
    isolated.join();
    Check(threadError.find(L"theme") != std::wstring::npos &&
              core::Config::LastError().find(L"sync_minutes") != std::wstring::npos,
          "config errors remain calling-thread-local");
}
void TestConfig(Sandbox& box) {
    // Parse supplies a baseline, but a nonexistent destination is an explicit full initialization.
    const auto parsedPath = box.Path(L"parsed-new.json");
    const json parsedInput = {{"game_root", "D:\\Parsed\\Game"},
                              {"theme", 1},
                              {"auto_on", false},
                              {"sync_minutes", 17},
                              {"last_sync", "retained"},
                              {"extension", {{"kept", {1, u8"用户"}}}}};
    core::Config parsedNew;
    bool parsedPreserved = GetFileAttributesW(parsedPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
                           core::Config::Parse(parsedInput.dump(), parsedNew) && parsedNew.Save(parsedPath);
    if (parsedPreserved) {
        const auto initialized = json::parse(ReadBytes(parsedPath));
        for (auto it = parsedInput.begin(); it != parsedInput.end(); ++it)
            parsedPreserved =
                parsedPreserved && initialized.contains(it.key()) && initialized.at(it.key()) == it.value();
    }
    Check(parsedPreserved, "Parse baseline initializes new file with all supplied known and unknown fields");
    const std::wstring path = box.Path(L"settings.json");
    core::Config missing;
    missing.theme = 1;
    Check(core::Config::LoadSafe(box.Path(L"new-config.json"), missing) && missing.theme == 0,
          "absent file yields default config successfully");
    WriteBytes(path,
               "{\"game_root\":\"D:\\\\Riot Games\\\\League of "
               "Legends\\\\Game\",\"theme\":0,\"autostart\":true,\"extension\":{\"kept\":[1,\"用户\"]}}");
    core::Config config;
    Check(core::Config::LoadSafe(path, config), "single snapshot load");
    Check(config.autostart && config.gameRoot == L"D:\\Riot Games\\League of Legends\\Game",
          "legacy user values preserved");
    Check(core::Config::UpdateAtomic(path, SetTheme, nullptr), "missing-field update accepted");
    json saved = json::parse(ReadBytes(path));
    Check(saved.at("theme") == 1 && saved.at("autostart") == true &&
              saved.at("extension").at("kept")[1] == u8"用户",
          "migration retains unknown/user fields");
    std::string original = ReadBytes(path);
    Check(!core::Config::UpdateAtomic(path, InvalidMutation, nullptr) && !core::Config::LastError().empty(),
          "invalid mutation rejected");
    Check(ReadBytes(path) == original, "invalid mutation leaves disk intact");
    Check(!core::Config::UpdateAtomic(path, ThrowMutation, nullptr), "callback exception caught");
    Check(ReadBytes(path) == original, "callback exception leaves disk intact");
    WriteBytes(path, "{\"theme\":null}");
    config.theme = 1;
    Check(!core::Config::LoadSafe(path, config) && config.theme == 1, "bad load leaves output unchanged");
    Check(!config.Save(path) && ReadBytes(path) == "{\"theme\":null}",
          "Save refuses corrupt existing configuration");
    WriteBytes(path, "{\"theme\":0}");
    std::wstring mutablePath = path;
    Check(!core::Config::UpdateAtomic(path, ExternalMutation, &mutablePath),
          "external write conflict detected without second parse");
    Check(json::parse(ReadBytes(path)).at("external") == true, "external edit not overwritten");
    config = core::Config{};
    config.gameRoot = std::wstring(L"D:\\prefix") + wchar_t(0) + L"\\child";
    Check(!config.Save(box.Path(L"nul.json")), "in-memory NUL path refused");
    Check(!core::Config::LoadSafe(std::wstring(path) + wchar_t(0) + L".other", config),
          "config IO NUL refused");
    WriteBytes(box.Path(L"legacy.json"), R"({"theme":1,"plugin":{"preserved":true}})");
    const auto imported = box.Path(L"imported.json");
    Check(core::Config::ImportLegacyIfMissing(box.Path(L"legacy.json"), imported), "explicit legacy import");
    Check(json::parse(ReadBytes(imported)).at("plugin").at("preserved") == true,
          "import preserves extensions");
    Check(GetFileAttributesW((box.Path(L"legacy.json") + L".lock").c_str()) == INVALID_FILE_ATTRIBUTES,
          "import does not require writable legacy directory or create source lock");
    const auto importedBytes = ReadBytes(imported);
    WriteBytes(box.Path(L"legacy.json"), "{\"theme\":0}");
    Check(core::Config::ImportLegacyIfMissing(box.Path(L"legacy.json"), imported) &&
              ReadBytes(imported) == importedBytes,
          "import never overwrites current config");
    Check(core::Config::ImportLegacyIfMissing(box.Path(L"absent\\legacy.json"),
                                              box.Path(L"absent-import.json")),
          "missing legacy directory is no-op");
    // A sharing conflict on destination must clean its newly-created temporary file after closing it.
    WriteBytes(path, "{\"theme\":0}");
    {
        winutil::ScopedHandle pinned(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(pinned.valid(), "pin destination");
        Check(!core::Config::UpdateAtomic(path, SetTheme, nullptr), "replace sharing failure surfaced");
    }
    size_t temporaryFiles = 0;
    for (const auto& entry : fs::directory_iterator(box.root))
        if (entry.path().filename().wstring().find(L"settings.json.tmp.") == 0)
            ++temporaryFiles;
    Check(temporaryFiles == 0, "failed atomic replace leaves no temporary files");
    const std::wstring threaded = box.Path(L"threaded.json");
    config = core::Config{};
    config.winX = 0;
    Check(config.Save(threaded), "threaded initialize");
    std::atomic<int> failed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int j = 0; j < 15; ++j)
                if (!core::Config::UpdateAtomic(threaded, Increment, nullptr))
                    ++failed;
        });
    for (auto& thread : threads)
        thread.join();
    Check(failed == 0 && core::Config::LoadSafe(threaded, config) && config.winX == 60,
          "threaded updates do not lose values");
    const std::wstring concurrent = box.Path(L"concurrent.json");
    config = core::Config{};
    config.winX = 0;
    Check(config.Save(concurrent), "cross-process initialize");
    std::wstring executable = winutil::GetExePath();
    Child a(executable, L"--config-increment \"" + concurrent + L"\"");
    Child b(executable, L"--config-increment \"" + concurrent + L"\"");
    Child c(executable, L"--config-increment \"" + concurrent + L"\"");
    a.Wait();
    b.Wait();
    c.Wait();
    Check(core::Config::LoadSafe(concurrent, config) && config.winX == 60,
          "cross-process lock covers full read/modify/replace");
}
void TestLogger(Sandbox& box) {
    std::wstring longDir = box.root;
    for (int i = 0; i < 12; ++i)
        longDir += L"\\long-auxiliary-path-component-" + std::to_wstring(i);
    const std::wstring path = longDir + L"\\日志.jsonl";
    Check(path.size() > MAX_PATH, "long path fixture");
    {
        core::Logger logger;
        Check(logger.Open(path, 1, 2), "logger creates long directory tree");
        Check(logger.TryLog(core::LogLevel::Info, L"模块", L"first"), "long initial log");
        Check(logger.TryLog(core::LogLevel::Warn, L"模块", L"second"), "long rotation log");
        Check(logger.TryLog(core::LogLevel::Error, L"模块", L"third"), "long second rotation and flush");
        Check(!logger.TryLog(core::LogLevel::Info, L"x", std::wstring(1, static_cast<wchar_t>(0xD800))),
              "invalid Unicode log returns failure");
        Check(!logger.LastError().empty(), "logger error observable");
        Check(logger.TryClose(), "logger flush and close checked");
    }
    core::Config longConfig;
    longConfig.gameRoot = L"D:\\Riot Games\\League of Legends\\Game";
    Check(longConfig.Save(longDir + L"\\配置.json"), "config long-path atomic write");
    core::Config loadedLong;
    Check(core::Config::LoadSafe(longDir + L"\\配置.json", loadedLong) &&
              loadedLong.gameRoot == longConfig.gameRoot,
          "config long-path read uses same parsed snapshot");
    std::wstring text = L"unchanged", error;
    Check(core::Logger::TryExportAll(path, text, &error, 2), "long log export");
    Check(text.find(L"first") != std::wstring::npos && text.find(L"second") != std::wstring::npos &&
              text.find(L"third") != std::wstring::npos,
          "all rotations exported oldest first");
    Check(text.find(L"first") < text.find(L"second") && text.find(L"second") < text.find(L"third"),
          "export chronology");
    text = L"unchanged";
    Check(!core::Logger::TryExportAll(box.Path(L"no-log.jsonl"), text, &error) && text == L"unchanged" &&
              !error.empty(),
          "missing logs distinguished from empty logs");
    const auto empty = box.Path(L"empty.jsonl");
    WriteBytes(empty, "");
    Check(core::Logger::TryExportAll(empty, text, &error) && text.empty(),
          "empty existing log is successful");
    const auto malformed = box.Path(L"bad-utf.jsonl");
    WriteBytes(malformed, std::string("\xFF\n", 2));
    text = L"unchanged";
    Check(!core::Logger::TryExportAll(malformed, text, &error) && text == L"unchanged",
          "invalid UTF8 export is failure not silent loss");
    core::Logger logger;
    Check(!logger.Open(box.Path(L"bad.jsonl"), 1, -1) && !logger.LastError().empty(),
          "invalid rotation count rejected");
    const auto blocked = box.Path(L"blocked-parent");
    WriteBytes(blocked, "file not directory");
    Check(!logger.Open(blocked + L"\\logs.jsonl") && !logger.LastError().empty(),
          "directory creation failure observed");
    const auto raii = box.Path(L"raii.jsonl");
    {
        core::Logger scoped;
        Check(scoped.Open(raii) && scoped.TryLog(core::LogLevel::Info, L"test", L"RAII"),
              "RAII logger opens");
    }
    Check(DeleteFileW(raii.c_str()) != FALSE, "logger destructor closes handle");
    const auto rotating = box.Path(L"rotation-failure.jsonl");
    core::Logger rotation;
    Check(rotation.Open(rotating, 1, 2) && rotation.TryLog(core::LogLevel::Info, L"test", L"oldest"),
          "rotation failure fixture starts");
    Check(rotation.TryLog(core::LogLevel::Info, L"test", L"middle"),
          "rotation failure fixture first rotation");
    {
        winutil::ScopedHandle pinned(CreateFileW((rotating + L".1").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(pinned.valid(), "pin old rotation without delete sharing");
        Check(!rotation.TryLog(core::LogLevel::Info, L"test", L"blocked") && !rotation.LastError().empty(),
              "failed rotation is observable");
    }
    Check(rotation.TryLog(core::LogLevel::Info, L"test", L"newest") && rotation.TryClose(),
          "logger recovers after failed rotation");
    Check(core::Logger::TryExportAll(rotating, text, &error, 2) &&
              text.find(L"oldest") != std::wstring::npos && text.find(L"middle") != std::wstring::npos &&
              text.find(L"newest") != std::wstring::npos,
          "rotation error did not overwrite unmoved evidence");
}
std::string Record(int n, size_t payload = 0) {
    return json{{"ts", "2026-10-23 00:00:00.000"},
                {"level", "error"},
                {"module", u8"模块"},
                {"msg", std::string(payload, 'x')},
                {"n", n}}
               .dump() +
           "\n";
}
void TestTail(Sandbox& box) {
    const auto path = box.Path(L"tail.jsonl");
    std::vector<std::string> records = {"unchanged"};
    std::wstring error;
    core::LogTailStats stats;
    Check(!core::Logger::TryReadRawTail(path, records, &error) && records[0] == "unchanged" && !error.empty(),
          "tail missing is atomic failure");
    WriteBytes(path, "");
    Check(core::Logger::TryReadRawTail(path, records, &error) && records.empty() && error.empty(),
          "tail empty file differs from missing");
    Check(!core::Logger::TryReadRawTail(path, records, &error, 0), "tail zero record budget rejected");
    Check(!core::Logger::TryReadRawTail(path, records, &error, 200, 0), "tail zero byte budget rejected");
    Check(!core::Logger::TryReadRawTail(path, records, &error, 10001), "tail record cap enforced");
    Check(!core::Logger::TryReadRawTail(path, records, &error, 200, 8 * 1024 * 1024 + 1),
          "tail byte cap enforced");
    Check(!core::Logger::TryReadRawTail(path, records, &error, 200, 256 * 1024, 101),
          "tail rotation cap enforced");
    WriteBytes(path, Record(1) + Record(2) + "{incomplete");
    Check(core::Logger::TryReadRawTail(path, records, &error, 2) && records.size() == 2 &&
              json::parse(records[0]).at("n") == 1 && json::parse(records[1]).at("n") == 2,
          "tail keeps complete chronological records and omits unterminated suffix");
    WriteBytes(path, Record(3) + "\r\n" + Record(4));
    Check(core::Logger::TryReadRawTail(path, records, &error, 1) && records.size() == 1 &&
              json::parse(records[0]).at("n") == 4,
          "tail newest count selection");
    WriteBytes(path, std::string("\xFF\n", 2));
    records = {"unchanged"};
    Check(!core::Logger::TryReadRawTail(path, records, &error) && records[0] == "unchanged",
          "tail invalid UTF8 fails without replacing output");
    WriteBytes(path, std::string(6 * 1024 * 1024 + 4097, 'x') + "\n");
    Check(!core::Logger::TryReadRawTail(path, records, &error) && records[0] == "unchanged",
          "tail external oversized complete line fails instead of JSON truncation");
    WriteBytes(path, std::string(6 * 1024 * 1024 + 4097, 'x'));
    Check(!core::Logger::TryReadRawTail(path, records, &error),
          "tail oversized incomplete suffix is bounded");
    for (const size_t size : {size_t(12 * 1024 * 1024), size_t(49 * 1024 * 1024)}) {
        std::string history;
        history.reserve(size + 2048);
        int total = 0;
        while (history.size() < size)
            history += Record(total++, 512);
        WriteBytes(path, history);
        const size_t historyBytes = history.size();
        history.clear();
        history.shrink_to_fit();
        Check(core::Logger::TryReadRawTail(path, records, &error, 5, 256 * 1024, 5, &stats) &&
                  records.size() == 5 && json::parse(records.front()).at("n") == total - 5 &&
                  json::parse(records.back()).at("n") == total - 1,
              "12/49MiB tail returns exactly latest five in forward order");
        Check(stats.bytesRead <= 64 * 1024 && !stats.cacheHit,
              "tail data IO bounded by one block, independent of full-history size");
        const size_t firstRead = stats.bytesRead;
        std::vector<double> milliseconds;
        for (int repeat = 0; repeat < 10; ++repeat) {
            auto start = std::chrono::steady_clock::now();
            Check(core::Logger::TryReadRawTail(path, records, &error, 5, 256 * 1024, 5, &stats) &&
                      stats.cacheHit && stats.bytesRead == 0,
                  "unchanged refresh uses metadata cache and zero data reads");
            milliseconds.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(milliseconds.begin(), milliseconds.end());
        std::cout << "TAIL history=" << historyBytes << " firstRead=" << firstRead
                  << " cached-P50-ms=" << milliseconds[4] << " cached-P95-ms=" << milliseconds[9] << "\n";
        Check(core::Logger::TryReadRawTail(path, records, &error, 200, 256 * 1024, 5, &stats) &&
                  records.size() == 200 && stats.bytesRead <= 3 * 64 * 1024,
              "default 200 record preview is bounded and not served from incompatible cache key");
    }
    WriteBytes(path, Record(100));
    Check(core::Logger::TryReadRawTail(path, records, &error, 5, 256 * 1024, 5, &stats) && !stats.cacheHit &&
              records.size() == 1 && json::parse(records[0]).at("n") == 100,
          "truncate invalidates tail cache");
    Check(MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE,
          "tail rotate fixture");
    WriteBytes(path, Record(101));
    Check(core::Logger::TryReadRawTail(path, records, &error, 5, 256 * 1024, 5, &stats) && !stats.cacheHit &&
              records.size() == 2 && json::parse(records[0]).at("n") == 100 &&
              json::parse(records[1]).at("n") == 101,
          "rotation identity and chronology invalidate cache");
    Check(DeleteFileW(path.c_str()) && DeleteFileW((path + L".1").c_str()), "reset tail-owned fixtures");
    {
        core::Logger logger;
        Check(logger.Open(path, 0, 0) &&
                  logger.TryLog(core::LogLevel::Error, L"中文模块", std::wstring(16000, L'错')),
              "long Chinese Error fixture from actual logger");
        Check(core::Logger::TryReadRawTail(path, records, &error, 5, 8192) && records.size() == 1,
              "soft byte budget permits complete newest record");
        const auto parsed = json::parse(records[0]);
        Check(parsed.at("level") == "error" && parsed.at("module") == u8"中文模块" &&
                  parsed.at("ts").get<std::string>().size() == 23 &&
                  parsed.at("msg").get<std::string>().size() == 48000,
              "full JSON precedes presentation truncation, level module timestamp retained");
        Check(logger.TryLog(core::LogLevel::Fatal, L"x", std::wstring(1024 * 1024 - 1, L'\0')),
              "maximum control-character input accepted by existing logger contract");
        Check(core::Logger::TryReadRawTail(path, records, &error, 200) && records.size() == 1 &&
                  json::parse(records[0]).at("level") == "fatal" &&
                  json::parse(records[0]).at("msg").get<std::string>().size() == 1024 * 1024 - 1,
              "six-fold escaped maximum logger record returned complete not cropped");
        Check(logger.TryClose(), "close escaped-log fixture");
    }
#ifdef LOL_AUX_TEST_HOOKS
    Check(DeleteFileW(path.c_str()) != FALSE, "reset snapshot fixture");
    core::Logger writer;
    Check(writer.Open(path, 1, 2) && writer.TryLog(core::LogLevel::Info, L"test", L"old0") &&
              writer.TryLog(core::LogLevel::Info, L"test", L"old1"),
          "snapshot starts with two rotations");
    bool concurrentWrite = false;
    core::aux_test::SetTailSnapshotHook([&] {
        std::thread concurrent([&] {
            concurrentWrite = writer.TryLog(core::LogLevel::Info, L"test", L"new2") &&
                              writer.TryLog(core::LogLevel::Info, L"test", L"new3");
        });
        concurrent.join(); // Deterministic proof tail does not retain allLogsMutex while reading.
    });
    const bool readSnapshot = core::Logger::TryReadRawTail(path, records, &error, 10, 256 * 1024, 2);
    core::aux_test::SetTailSnapshotHook({});
    if (!readSnapshot || !concurrentWrite || records.size() != 2) {
        std::cerr << "SNAPSHOT read=" << readSnapshot << " writer=" << concurrentWrite
                  << " records=" << records.size() << " read-error=" << WideToUtf8(error)
                  << " write-error=" << WideToUtf8(writer.LastError()) << "\n";
        for (const auto& record : records)
            std::cerr << record << "\n";
    }
    Check(readSnapshot && concurrentWrite && records.size() == 2 &&
              json::parse(records[0]).at("msg") == "old0" && json::parse(records[1]).at("msg") == "old1",
          "pinned rotation snapshot remains coherent while writer rotates outside reader lock");
    Check(core::Logger::TryReadRawTail(path, records, &error, 10, 256 * 1024, 2) && records.size() == 3 &&
              json::parse(records.back()).at("msg") == "new3",
          "next snapshot observes new rotation set");
    Check(writer.TryClose(), "snapshot writer close");
    const auto noRotations = box.Path(L"tail-zero-rotations.jsonl");
    Check(writer.Open(noRotations, 1, 0) && writer.TryLog(core::LogLevel::Info, L"test", L"zero-old"),
          "zero retained rotations snapshot fixture");
    core::aux_test::SetTailSnapshotHook(
        [&] { concurrentWrite = writer.TryLog(core::LogLevel::Info, L"test", L"zero-new"); });
    const bool readZero = core::Logger::TryReadRawTail(noRotations, records, &error, 10, 256 * 1024, 0);
    core::aux_test::SetTailSnapshotHook({});
    Check(readZero && concurrentWrite && records.size() == 1 &&
              json::parse(records[0]).at("msg") == "zero-old",
          "zero-rotation retirement avoids delete-pending path reuse while snapshot remains readable");
    Check(writer.TryClose(), "zero-rotation writer closes");
    const auto rollback = box.Path(L"tail-rollback.jsonl");
    Check(writer.Open(rollback, 1, 2) && writer.TryLog(core::LogLevel::Info, L"test", L"keep0") &&
              writer.TryLog(core::LogLevel::Info, L"test", L"keep1") &&
              writer.TryLog(core::LogLevel::Info, L"test", L"keep2"),
          "replacement rollback fixture");
    const auto oldOne = ReadBytes(rollback + L".1"), oldTwo = ReadBytes(rollback + L".2");
    {
        winutil::ScopedHandle target(CreateFileW((rollback + L".2").c_str(), GENERIC_READ,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr, OPEN_EXISTING, 0, nullptr));
        winutil::ScopedHandle source(CreateFileW((rollback + L".1").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                                 nullptr, OPEN_EXISTING, 0, nullptr));
        Check(target.valid() && source.valid(), "pin replace target but prohibit moving source");
        Check(!writer.TryLog(core::LogLevel::Info, L"test", L"blocked") &&
                  ReadBytes(rollback + L".1") == oldOne && ReadBytes(rollback + L".2") == oldTwo,
              "failed replacement preserves both unmoved source and older target evidence");
    }
    Check(writer.TryLog(core::LogLevel::Info, L"test", L"recovered") && writer.TryClose(),
          "replacement resumes after conflicting owner closes");
    size_t retired = 0;
    for (const auto& entry : fs::directory_iterator(box.root))
        if (entry.path().filename().wstring().find(L".retired.") != std::wstring::npos)
            ++retired;
    Check(retired == 0, "retired rotation names reclaimed after final snapshot closes");
#endif
}
void TestLoggerLinks(Sandbox& box) {
    const auto sentinel = box.Path(L"log-sentinel.txt"), hard = box.Path(L"log-hard.jsonl");
    WriteBytes(sentinel, "sentinel-do-not-change");
    Check(CreateHardLinkW(hard.c_str(), sentinel.c_str(), nullptr) != FALSE, "isolated hardlink fixture");
    core::Logger logger;
    std::vector<std::string> records = {"unchanged"};
    std::wstring error, wide;
    Check(!logger.Open(hard) && !logger.TryLog(core::LogLevel::Info, L"test", L"blocked"),
          "writer rejects leaf hardlink at open and implicit retry");
    Check(!core::Logger::TryReadRawTail(hard, records, &error) && records[0] == "unchanged" &&
              !core::Logger::TryReadRaw(hard, wide, &error) &&
              !core::Logger::TryExportAll(hard, wide, &error),
          "preview raw and export readers consistently refuse hardlinks");
    Check(ReadBytes(sentinel) == "sentinel-do-not-change", "hardlink sentinel unchanged");
    const auto late = box.Path(L"log-late-hard.jsonl"), alias = box.Path(L"log-late-alias.jsonl");
    Check(logger.Open(late) && logger.TryLog(core::LogLevel::Info, L"test", L"before"),
          "late hardlink fixture starts as regular leaf");
    const auto before = ReadBytes(late);
    Check(CreateHardLinkW(alias.c_str(), late.c_str(), nullptr) != FALSE, "hardlink added to open file");
    Check(!logger.TryLog(core::LogLevel::Info, L"test", L"after") && ReadBytes(late) == before,
          "writer revalidates hardlink count before every append");
    Check(logger.TryClose(), "close refused late hardlink writer");
    const auto symbolic = box.Path(L"log-symbolic.jsonl");
    if (CreateSymbolicLinkW(symbolic.c_str(), sentinel.c_str(),
                            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ||
        CreateSymbolicLinkW(symbolic.c_str(), sentinel.c_str(), 0)) {
        Check(!logger.Open(symbolic) && !logger.TryLog(core::LogLevel::Error, L"test", L"blocked"),
              "writer rejects leaf symbolic link");
        Check(!core::Logger::TryReadRawTail(symbolic, records, &error) &&
                  !core::Logger::TryReadRaw(symbolic, wide, &error) &&
                  !core::Logger::TryExportAll(symbolic, wide, &error),
              "all readers reject leaf symbolic link");
        Check(ReadBytes(sentinel) == "sentinel-do-not-change", "symbolic target sentinel unchanged");
        Check(DeleteFileW(symbolic.c_str()) != FALSE, "unlink owned symbolic fixture without following it");
    } else {
        std::cout << "SKIP: symbolic log leaf fixture unavailable, Windows " << GetLastError() << "\n";
    }
}
std::wstring MakeLayout(const std::wstring& install) {
    const auto game = install + L"\\Game";
    fs::create_directories(winutil::LongPath(game));
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof(dos);
    std::string pe(sizeof(dos) + sizeof(DWORD), '\0');
    std::memcpy(&pe[0], &dos, sizeof(dos));
    const DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(&pe[sizeof(dos)], &signature, sizeof(signature));
    WriteBytes(install + L"\\LeagueClient.exe", pe);
    WriteBytes(game + L"\\League of Legends.exe", pe);
    return game; // Minimal inert PE headers are never launched.
}
void TestPathPolicy(Sandbox& box) {
    const auto game = MakeLayout(box.Path(L"Optimized\\外服客户端"));
    Check(core::GameDetect::IsGameRoot(game), "Optimized does not match timi substring");
    Check(core::GameDetect::Find(game) == game, "explicit valid foreign path retained");
    Check(core::GameDetect::Find(game, true).empty() && !core::GameDetect::LastError().empty(),
          "deep true rejects even a valid explicit candidate according to declared unsupported contract");
    fs::create_directory(winutil::LongPath(box.Path(L"Optimized\\WeGame")));
    fs::create_directory(winutil::LongPath(box.Path(L"Optimized\\TCLS")));
    fs::create_directory(winutil::LongPath(box.Path(L"Optimized\\英雄联盟")));
    Check(core::GameDetect::IsGameRoot(game),
          "unrelated grandparent sibling China installs do not reject foreign");
    for (const auto marker : {L"TCLS", L"WeGame", L"TenProtect", L"TPHelper.exe"}) {
        const auto ownedMarker = box.Path(L"Optimized\\外服客户端\\") + marker;
        WriteBytes(ownedMarker, "owned China marker");
        Check(!core::GameDetect::IsGameRoot(game), "actual client-local China marker still rejected");
        Check(DeleteFileW(ownedMarker.c_str()) != FALSE, "remove owned client China marker");
    }
    WriteBytes(game + L"\\TenioDL.exe", "owned China marker");
    Check(!core::GameDetect::IsGameRoot(game), "game-local China component rejected too");
    for (const auto token : {L"WeGaMe", L"TiMi", L"Tencent Games", L"腾讯游戏", L"英雄联盟"}) {
        const auto china = MakeLayout(box.root + L"\\" + token + L"\\Client");
        Check(!core::GameDetect::IsGameRoot(china),
              "exact case-insensitive/Chinese China ancestor token rejected");
    }
    Check(!core::GameDetect::IsGameRoot(L"C:\\"), "shallow root rejected without executable assumptions");
}
#ifdef LOL_AUX_TEST_HOOKS
void TestBoundedDiscovery(Sandbox& box) {
    using core::aux_test::ScanFixture;
    struct Reset {
        ~Reset() {
            core::aux_test::SetScanFixture(nullptr);
        }
    } reset;
    const auto root = box.Path(L"scan-root");
    fs::create_directory(winutil::LongPath(root));
    ScanFixture fixture;
    fixture.drives = {{root + L"\\", DRIVE_FIXED}};
    fixture.clock = [] { return ULONGLONG(100); };
    core::aux_test::SetScanFixture(&fixture);
    Check(core::GameDetect::Find(L"", true).empty() && fixture.opened == 0 && fixture.candidates == 0,
          "deep true does not touch process registry or disks");
    for (int iteration = 0; iteration < 1000; ++iteration)
        Check(core::GameDetect::Find(L"").empty(), "isolated empty fixed-root enumeration completes");
    Check(fixture.opened == 1000 && fixture.closed == 1000,
          "every FindFirstFile search closed once with FindClose over 1000 finds");
    const auto game = MakeLayout(root + L"\\Custom\\League of Legends");
    const auto openedBefore = fixture.opened, closedBefore = fixture.closed;
    Check(core::GameDetect::Find(L"") == game && fixture.opened == openedBefore + 1 &&
              fixture.closed == closedBefore + 1,
          "early success closes active search handle");
    fixture.throwAfterOpen = true;
    Check(core::GameDetect::Find(L"").empty() && fixture.opened == fixture.closed &&
              !core::GameDetect::LastError().empty(),
          "exception path closes active search handle");
    fixture.throwAfterOpen = false;
    fixture.entryLimit = 1;
    const auto entriesBefore = fixture.entries;
    Check(core::GameDetect::Find(L"").empty() && fixture.entries == entriesBefore + 1 &&
              fixture.opened == fixture.closed,
          "all-entry budget closes search without unbounded skipping");
    fixture.entryLimit = 400;
    fixture.candidateLimit = 1;
    const auto candidatesBefore = fixture.candidates;
    Check(core::GameDetect::Find(L"").empty() && fixture.candidates == candidatesBefore + 1 &&
              fixture.opened == fixture.closed,
          "candidate budget is global not per drive");
    fixture.candidateLimit = 2048;
    fixture.drives = {{root + L"\\", DRIVE_REMOTE},
                      {root + L"\\", DRIVE_REMOVABLE},
                      {root + L"\\", DRIVE_CDROM},
                      {root + L"\\", DRIVE_RAMDISK}};
    const auto noOpen = fixture.opened, noCandidates = fixture.candidates;
    Check(core::GameDetect::Find(L"").empty() && fixture.opened == noOpen &&
              fixture.candidates == noCandidates,
          "automatic drive scan excludes remote removable optical and RAM drives");
    fixture.drives = {{root + L"\\", DRIVE_FIXED}};
    ULONGLONG tick = 0;
    fixture.clock = [&] {
        tick += 2000;
        return tick;
    };
    Check(core::GameDetect::Find(L"").empty() && fixture.opened == noOpen &&
              fixture.candidates == noCandidates && !core::GameDetect::LastError().empty(),
          "deterministic cooperative deadline stops before filesystem candidates");
    fixture.clock = [] { return ULONGLONG(100); };
    fixture.drives.clear();
    fixture.runningCandidates = {MakeLayout(root + L"\\WeGame\\Client"), root + L"\\missing", game};
    Check(core::GameDetect::Find(L"") == game && fixture.opened == noOpen,
          "running China/unreadable candidates do not hide a later eligible foreign client");
    Check(core::aux_test::ParseDisplayIcon(L"  \"C:\\路径, with comma\\client.exe\" , -2  ") ==
              L"C:\\路径, with comma\\client.exe",
          "quoted DisplayIcon preserves internal commas");
    Check(core::aux_test::ParseDisplayIcon(L"C:\\path,with comma\\client.exe,-1") ==
              L"C:\\path,with comma\\client.exe",
          "unquoted DisplayIcon uses final numeric index comma");
    Check(core::aux_test::ParseDisplayIcon(L"C:\\plain\\client.exe") == L"C:\\plain\\client.exe",
          "DisplayIcon without index remains compatible");
    Check(core::aux_test::ParseDisplayIcon(L"\"C:\\missing closing quote").empty() &&
              core::aux_test::ParseDisplayIcon(L"\"C:\\client.exe\" , --2").empty(),
          "malformed quoted DisplayIcon fails without registry execution");
}
#endif
void TestDetectionAndMonitor(Sandbox& box) {
    const auto installation = box.Path(L"installation"), game = installation + L"\\Game";
    Check(CreateDirectoryW(installation.c_str(), nullptr) && CreateDirectoryW(game.c_str(), nullptr),
          "fake installation layout");
    Check(CreateDirectoryW((installation + L"\\LeagueClient.exe").c_str(), nullptr) &&
              CreateDirectoryW((game + L"\\League of Legends.exe").c_str(), nullptr),
          "directory named exe fixture");
    Check(!core::GameDetect::IsGameRoot(game), "directories named exe rejected");
    Check(RemoveDirectoryW((installation + L"\\LeagueClient.exe").c_str()) &&
              RemoveDirectoryW((game + L"\\League of Legends.exe").c_str()),
          "remove exe directories");
    WriteBytes(installation + L"\\LeagueClient.exe", "");
    WriteBytes(game + L"\\League of Legends.exe", "");
    Check(!core::GameDetect::IsGameRoot(game), "empty placeholder executables rejected");
    Check(core::GameDetect::Find(game, true).empty() && !core::GameDetect::LastError().empty(),
          "deep scan explicitly unsupported without global scanning");
    Check(core::GameDetect::GetCompanyName(std::wstring(game) + wchar_t(0) + L"\\x").empty(),
          "company metadata NUL path rejected");
    Check(core::Monitor::Probe(L"").state == core::LobbyState::Unknown, "unconfigured monitor unknown");
    auto nul = core::Monitor::Probe(std::wstring(game) + wchar_t(0) + L"\\x");
    Check(nul.state == core::LobbyState::Unknown && nul.error != ERROR_SUCCESS,
          "monitor path NUL yields unknown with code");
    const std::wstring self = winutil::GetExePath();
    Check(CopyFileW(self.c_str(), (installation + L"\\LeagueClient.exe").c_str(), FALSE) &&
              CopyFileW(self.c_str(), (game + L"\\League of Legends.exe").c_str(), FALSE),
          "test-owned executable copies only");
    Check(core::GameDetect::IsGameRoot(game), "readable PE layout accepted as heuristic only");
    Check(core::Monitor::Probe(game).state == core::LobbyState::Offline,
          "unrelated installations do not count online");
    Check(core::Monitor::ProbeGame(game).state == core::LobbyState::Offline,
          "actual game is offline without configured process");
    // Run a copy of THIS harmless test only, never a real game/client binary.
    Child fixture(installation + L"\\LeagueClient.exe", L"--lobby-fixture");
    auto online = core::Monitor::Probe(game);
    Check(online.state == core::LobbyState::Online, "current-session exact configured image counts online");
    const auto unrelated = box.Path(L"unrelated");
    Check(CreateDirectoryW(unrelated.c_str(), nullptr) &&
              CreateDirectoryW((unrelated + L"\\Game").c_str(), nullptr),
          "unrelated installation fixture");
    Check(CopyFileW(self.c_str(), (unrelated + L"\\LeagueClient.exe").c_str(), FALSE),
          "unrelated client fixture");
    Check(core::Monitor::Probe(unrelated + L"\\Game").state == core::LobbyState::Offline,
          "same process name at another install ignored");
    {
        Child actualGame(game + L"\\League of Legends.exe", L"--lobby-fixture");
        Check(core::Monitor::ProbeGame(game).state == core::LobbyState::Online,
              "actual-game probe recognizes only exact game executable");
    }
    // Child destructors terminate these exact owned fixtures; no global process killing.
}
// Logger metadata/context regressions are separate from the existing auxiliary safety cases.
// All paths come from the owned temporary Sandbox; messages are synthetic, not file contents/parameters.
std::vector<json> LoggerRecords(const std::wstring& path, int maxFiles = 5, size_t maxRecords = 200,
                                size_t maxBytes = 256 * 1024) {
    std::vector<std::string> raw;
    std::wstring error;
    Check(core::Logger::TryReadRawTail(path, raw, &error, maxRecords, maxBytes, maxFiles),
          "metadata fixture tail read succeeds");
    std::vector<json> result;
    for (const auto& line : raw)
        result.push_back(json::parse(line));
    return result;
}
void TestLoggerMetadataAndContexts(Sandbox& box) {
    static_assert(std::is_nothrow_constructible<core::ScopedLogContext, const std::wstring&>::value,
                  "log context construction must be noexcept");
    static_assert(std::is_nothrow_destructible<core::ScopedLogContext>::value,
                  "log context destruction must be noexcept");
    static_assert(!std::is_copy_constructible<core::ScopedLogContext>::value &&
                      !std::is_copy_assignable<core::ScopedLogContext>::value &&
                      !std::is_move_constructible<core::ScopedLogContext>::value,
                  "thread-affine contexts cannot be copied or moved");
    const auto path = box.Path(L"metadata-contexts.jsonl");
    core::Logger logger;
    const auto beforeOpen = GetTickCount64();
    Check(logger.Open(path, 0, 0), "metadata logger opens");
    struct Expected {
        std::wstring message;
        std::string trace;
        std::string level;
    };
    std::vector<Expected> expected;
    auto emit = [&](core::LogLevel level, const std::wstring& message, const std::string& trace,
                    const char* label) {
        Check(logger.TryLog(level, L"日志模块", message), "context fixture log succeeds");
        expected.push_back({message, trace, label});
    };
    emit(core::LogLevel::Debug, L"bare \"quoted\"\nline", "", "debug");
    {
        core::ScopedLogContext outer(L"request:outer-中文");
        emit(core::LogLevel::Info, L"outer", u8"request:outer-中文", "info");
        {
            core::ScopedLogContext inner(L"command:inner");
            emit(core::LogLevel::Warn, L"inner", "command:inner", "warn");
        }
        emit(core::LogLevel::Info, L"restored", u8"request:outer-中文", "info");
        {
            core::ScopedLogContext empty(L"");
            emit(core::LogLevel::Debug, L"empty masks outer", "", "debug");
        }
        emit(core::LogLevel::Info, L"after empty", u8"request:outer-中文", "info");
        {
            core::ScopedLogContext invalid(std::wstring(1, static_cast<wchar_t>(0xD800)));
            emit(core::LogLevel::Info, L"invalid context ignored", u8"request:outer-中文", "info");
        }
        {
            core::ScopedLogContext oversized(std::wstring(1024 * 1024 + 1, L't'));
            emit(core::LogLevel::Info, L"oversized context ignored", u8"request:outer-中文", "info");
        }
        {
            core::ScopedLogContext oversizedUtf8(std::wstring(400000, L'中'));
            emit(core::LogLevel::Info, L"UTF8 oversized context ignored", u8"request:outer-中文", "info");
        }
        struct ContextUnwind {};
        try {
            core::ScopedLogContext unwind(L"exception-scope");
            emit(core::LogLevel::Error, L"before unwind", "exception-scope", "error");
            throw ContextUnwind{};
        } catch (const ContextUnwind&) {
        }
        emit(core::LogLevel::Info, L"after unwind", u8"request:outer-中文", "info");
    }
    emit(core::LogLevel::Fatal, L"bare after all scopes", "", "fatal");
    const std::wstring invalidMessage(1, static_cast<wchar_t>(0xD800));
    Check(!logger.TryLog(core::LogLevel::Info, L"test", invalidMessage), "invalid log fails observably");
    const DWORD failedSystemError = GetLastError();
    const auto failure = logger.LastError();
    logger.Log(core::LogLevel::Info, L"test", invalidMessage);
    const DWORD debugSystemError = GetLastError();
    Check(!failure.empty() && logger.LastError() == failure && debugSystemError == failedSystemError,
          "Log debug fallback preserves detailed Logger and Win32 errors");
    Check(logger.Open(path, 0, 0), "same live Open remains idempotent");
    emit(core::LogLevel::Info, L"after idempotent Open", "", "info");
    Check(logger.TryClose(), "metadata logger closes and flushes");
    const auto records = LoggerRecords(path, 0);
    Check(records.size() == expected.size(), "invalid logs append no records");
    const auto firstRun = records.at(0).at("run_id").get<std::string>();
    Check(firstRun.size() > 36, "run id is a nonempty unique identifier, not a seconds timestamp");
    unsigned long long previousUptime = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& entry = records[i];
        Check(entry.at("ts").is_string() && entry.at("ts").get<std::string>().size() == 23 &&
                  entry.at("level") == expected[i].level && entry.at("module") == u8"日志模块" &&
                  entry.at("msg") == WideToUtf8(expected[i].message),
              "all four legacy JSON fields remain complete and UTF8 escaped correctly");
        Check(entry.at("pid").is_number_unsigned() && entry.at("pid") == GetCurrentProcessId() &&
                  entry.at("tid").is_number_unsigned() && entry.at("tid") == GetCurrentThreadId() &&
                  entry.at("seq").is_number_unsigned() && entry.at("seq") == i + 1 &&
                  entry.at("run_id") == firstRun && entry.at("trace").is_string() &&
                  entry.at("trace") == expected[i].trace,
              "automatic metadata and nested context restoration are exact");
        const auto uptime = entry.at("uptime_ms").get<unsigned long long>();
        Check(entry.at("uptime_ms").is_number_unsigned() && uptime >= previousUptime &&
                  uptime <= GetTickCount64() - beforeOpen,
              "uptime milliseconds are monotonic within the Open run");
        previousUptime = uptime;
    }
    const auto beforeReopen = GetTickCount64();
    Check(logger.Open(path, 0, 0) && logger.TryLog(core::LogLevel::Info, L"test", L"second run") &&
              logger.TryClose(),
          "same logger closes and reopens immediately without a sleep");
    const auto reopened = LoggerRecords(path, 0);
    const auto secondRun = reopened.back().at("run_id").get<std::string>();
    Check(reopened.size() == records.size() + 1 && secondRun != firstRun &&
              reopened.back().at("seq") == 1 && reopened.back().at("trace") == "" &&
              reopened.back().at("uptime_ms").get<unsigned long long>() <= GetTickCount64() - beforeReopen,
          "new Open has unique run id and resets sequence/uptime without inheriting context");
    core::Logger other;
    const auto otherPath = box.Path(L"metadata-other-instance.jsonl");
    Check(other.Open(otherPath, 0, 0) && other.TryLog(core::LogLevel::Info, L"test", L"other instance") &&
              other.TryClose(),
          "another logger instance creates its own run");
    const auto otherRun = LoggerRecords(otherPath, 0).at(0).at("run_id").get<std::string>();
    Check(otherRun != firstRun && otherRun != secondRun, "run ids distinguish Logger instances");
    // Legacy Log-after-Close lazily reopens the existing path, now with a fresh lifecycle identifier.
    Check(logger.TryLog(core::LogLevel::Info, L"test", L"lazy reopen") && logger.TryClose(),
          "legacy lazy reopen still works after Close");
    const auto lazy = LoggerRecords(path, 0);
    Check(lazy.back().at("seq") == 1 && lazy.back().at("run_id") != firstRun &&
              lazy.back().at("run_id") != secondRun && lazy.back().at("run_id") != otherRun,
          "lazy reopen cannot reuse another Open run id or sequence");
}
void TestLoggerThreadContexts(Sandbox& box) {
    const auto path = box.Path(L"metadata-threads.jsonl");
    core::Logger logger;
    Check(logger.Open(path, 0, 0), "thread logger opens");
    winutil::ScopedHandle gate(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    winutil::ScopedHandle readyA(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    winutil::ScopedHandle readyB(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(gate.valid() && readyA.valid() && readyB.valid(), "thread gates created without named resources");
    HANDLE ready[] = {readyA.get(), readyB.get()};
    std::atomic<bool> failed{false};
    DWORD threadIds[2]{};
    std::thread workers[2];
    DWORD waitResult = WAIT_FAILED;
    bool mainLogged = false;
    {
        core::ScopedLogContext mainTrace(L"main-thread");
        try {
            for (size_t index = 0; index < 2; ++index) {
                workers[index] = std::thread([&, index] {
                    try {
                        threadIds[index] = GetCurrentThreadId();
                        const auto module = L"worker:" + std::to_wstring(index);
                        auto emit = [&](const std::wstring& message) {
                            if (!logger.TryLog(core::LogLevel::Info, module, message))
                                failed = true;
                        };
                        emit(L"bare-before");
                        {
                            core::ScopedLogContext trace(L"thread:" + std::to_wstring(index));
                            SetEvent(ready[index]);
                            if (WaitForSingleObject(gate.get(), 10000) != WAIT_OBJECT_0)
                                failed = true;
                            for (int i = 0; i < 16; ++i)
                                emit(L"outer:" + std::to_wstring(i));
                            {
                                core::ScopedLogContext nested(L"nested:" + std::to_wstring(index));
                                emit(L"nested");
                            }
                            emit(L"restored");
                        }
                        emit(L"bare-after");
                    } catch (...) {
                        failed = true;
                        SetEvent(ready[index]);
                    }
                });
            }
            waitResult = WaitForMultipleObjects(2, ready, TRUE, 10000);
            mainLogged = logger.TryLog(core::LogLevel::Info, L"main", L"during workers");
        } catch (...) {
            SetEvent(gate.get());
            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();
            throw;
        }
        SetEvent(gate.get());
        for (auto& worker : workers)
            worker.join();
    }
    Check(waitResult == WAIT_OBJECT_0 && mainLogged && !failed,
          "two simultaneous worker scopes log without failures or cross-thread inheritance");
    Check(threadIds[0] && threadIds[1] && threadIds[0] != threadIds[1] &&
              threadIds[0] != GetCurrentThreadId() && threadIds[1] != GetCurrentThreadId(),
          "thread ids identify each simultaneous caller");
    Check(logger.TryLog(core::LogLevel::Info, L"main", L"after workers") && logger.TryClose(),
          "main thread context restores after workers");
    const auto records = LoggerRecords(path, 0);
    Check(records.size() == 42, "thread fixture retains every serialized record");
    const auto run = records.at(0).at("run_id").get<std::string>();
    unsigned long long previousUptime = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& entry = records[i];
        const auto module = entry.at("module").get<std::string>();
        const auto message = entry.at("msg").get<std::string>();
        Check(entry.at("seq") == i + 1 && entry.at("run_id") == run &&
                  entry.at("pid") == GetCurrentProcessId(),
              "concurrent writes have unique monotonic seq in one shared Open run");
        const auto uptime = entry.at("uptime_ms").get<unsigned long long>();
        Check(uptime >= previousUptime, "concurrent uptimes follow serialized order");
        previousUptime = uptime;
        if (module == "main") {
            Check(entry.at("tid") == GetCurrentThreadId() &&
                      entry.at("trace") == (message == "during workers" ? "main-thread" : ""),
                  "worker contexts never replace main thread trace");
        } else {
            Check(module == "worker:0" || module == "worker:1", "only expected worker modules appear");
            const size_t index = module == "worker:0" ? 0 : 1;
            const std::string suffix = std::to_string(index);
            const std::string trace = message == "bare-before" || message == "bare-after"
                                          ? ""
                                          : (message == "nested" ? "nested:" : "thread:") + suffix;
            Check(entry.at("tid") == threadIds[index] && entry.at("trace") == trace,
                  "TLS trace is isolated, nested and restored independently on each thread");
        }
    }
}
void TestLoggerMetadataRotation(Sandbox& box) {
    const auto path = box.Path(L"metadata-rotation.jsonl");
    core::Logger logger;
    // Omit maxFiles: the existing default is still five rotations plus current.
    Check(logger.Open(path, 1), "metadata logger uses default rotation count");
    {
        core::ScopedLogContext context(L"rotate:synthetic");
        for (int i = 1; i <= 9; ++i)
            Check(logger.TryLog(core::LogLevel::Info, L"rotate", L"entry-" + std::to_wstring(i)),
                  "metadata survives repeated rotations");
    }
    Check(logger.TryClose(), "rotating metadata logger closes");
    Check(GetFileAttributesW((path + L".5").c_str()) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((path + L".6").c_str()) == INVALID_FILE_ATTRIBUTES,
          "default retention remains exactly five rotated files");
    const auto records = LoggerRecords(path);
    Check(records.size() == 6, "tail spans current and five rotations");
    const auto run = records.at(0).at("run_id").get<std::string>();
    for (size_t i = 0; i < records.size(); ++i)
        Check(records[i].at("seq") == i + 4 && records[i].at("run_id") == run &&
                  records[i].at("trace") == "rotate:synthetic" &&
                  records[i].at("msg") == "entry-" + std::to_string(i + 4),
              "rotation keeps run id, trace and monotonic sequence in chronological tail");
    const auto newest = LoggerRecords(path, 5, 3);
    Check(newest.size() == 3 && newest.front().at("seq") == 7 && newest.back().at("seq") == 9,
          "bounded newest-record selection remains compatible with metadata");
    std::wstring readable, raw, error;
    Check(core::Logger::TryExportAll(path, readable, &error) &&
              readable.find(L"entry-4") < readable.find(L"entry-9") &&
              readable.find(L"[info] rotate:") != std::wstring::npos,
          "legacy human-readable export ignores extra JSON fields and preserves order");
    Check(core::Logger::TryReadRaw(path, raw, &error) && raw.find(L"\"run_id\"") != std::wstring::npos &&
              raw.find(L"rotate:synthetic") != std::wstring::npos,
          "raw export retains metadata and scoped trace");
    const auto defaults = box.Path(L"metadata-default-size.jsonl");
    core::Logger defaultSize;
    Check(defaultSize.Open(defaults), "default 2MiB logger opens");
    const std::wstring payload(700 * 1024, L'x');
    for (int i = 0; i < 3; ++i)
        Check(defaultSize.TryLog(core::LogLevel::Info, L"default", payload), "bounded size fixture appends");
    Check(GetFileAttributesW((defaults + L".1").c_str()) == INVALID_FILE_ATTRIBUTES,
          "three bounded appends cross 2MiB without premature rotation");
    Check(defaultSize.TryLog(core::LogLevel::Error, L"default", L"after threshold") && defaultSize.TryClose(),
          "next append rotates at existing default 2MiB threshold and Error still flushes");
    const auto bytes = ReadBytes(defaults + L".1");
    Check(bytes.size() >= 2 * 1024 * 1024 && bytes.size() < 3 * 1024 * 1024 &&
              GetFileAttributesW((defaults + L".2").c_str()) == INVALID_FILE_ATTRIBUTES,
          "default rotation threshold remains 2MiB, not altered by metadata");
    const auto latest = LoggerRecords(defaults, 5, 1);
    Check(latest.size() == 1 && latest[0].at("seq") == 4 && latest[0].at("level") == "error" &&
              latest[0].at("trace") == "",
          "default logger metadata remains tail-compatible after size rotation");
}
void TestMonitorSessionPolicies() {
    // Pure shared production policy only: real cross-session/RDP process behavior is not exercised here.
    Check(core::monitor_detail::IncludesSession(false, 1, 1),
          "own-session lifecycle policy includes the current session");
    Check(!core::monitor_detail::IncludesSession(false, 1, 2),
          "own-session lifecycle policy excludes another session");
    Check(core::monitor_detail::IncludesSession(true, 1, 1),
          "shared-file write safety includes the current session");
    Check(core::monitor_detail::IncludesSession(true, 1, 2),
          "shared-file write safety includes another session");
    Check(core::Monitor::ProbeGameForWriteSafety(L"").state == core::LobbyState::Unknown,
          "empty game root cannot authorize shared-file writes");
}
} // namespace
int main() {
    struct WideArgs {
        LPWSTR* value;
        ~WideArgs() {
            if (value)
                LocalFree(value);
        }
    };
    int argc = 0;
    WideArgs arguments{CommandLineToArgvW(GetCommandLineW(), &argc)};
    wchar_t** argv = arguments.value;
    if (!argv)
        return 5;
    if (argc == 3 && std::wstring(argv[1]) == L"--config-increment") {
        // Even manual invocation of child mode may only mutate this test's marked temp root.
        std::wstring supplied = argv[2];
        size_t slash = supplied.find_last_of(L"\\/");
        std::vector<wchar_t> temp(32768);
        DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (!length || length >= temp.size() || slash == std::wstring::npos ||
            supplied.substr(slash + 1) != L"concurrent.json")
            return 4;
        std::wstring prefix(temp.data(), length), parent = supplied.substr(0, slash);
        if (parent.compare(0, prefix.size(), prefix) != 0 ||
            parent.find(L"LOLBackupV3-aux-", prefix.size()) != prefix.size() ||
            parent.find_first_of(L"\\/", prefix.size()) != std::wstring::npos)
            return 4;
        DWORD marker = GetFileAttributesW((parent + L"\\.aux-test-owned").c_str());
        if (marker == INVALID_FILE_ATTRIBUTES ||
            (marker & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return 4;
        for (int i = 0; i < 20; ++i)
            if (!core::Config::UpdateAtomic(supplied, Increment, nullptr))
                return 2;
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--lobby-fixture") {
        // A bounded wait makes exceptional parent termination safe; this mode does no IO.
        winutil::ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid())
            return 3;
        WaitForSingleObject(event.get(), 15000);
        return 0;
    }
    try {
        Sandbox box;
        TestUtf();
        TestParse();
        TestConfig(box);
        TestLogger(box);
        TestLoggerMetadataAndContexts(box);
        TestLoggerThreadContexts(box);
        TestLoggerMetadataRotation(box);
        TestTail(box);
        TestLoggerLinks(box);
        TestPathPolicy(box);
#ifdef LOL_AUX_TEST_HOOKS
        TestBoundedDiscovery(box);
#endif
        TestMonitorSessionPolicies();
        TestDetectionAndMonitor(box);
        std::cout << "PASS: " << checks << " auxiliary safety checks; only unique temporary root modified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << " (Windows " << GetLastError()
                  << ")\n";
        return 1;
    }
}
