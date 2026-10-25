// Standalone test translation unit: private hooks never enter a production build.
// All payloads are inert text, all process/action observations are fake, no game paths are opened.
#define TOOL_MANAGER_TEST
#include "../src/core/tool_manager.cpp"
#include <filesystem>
#include <iostream>

namespace {
using namespace core;
int checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void Reject(F fn, const char* message) {
    bool rejected = false;
    try {
        fn();
    } catch (...) {
        rejected = true;
    }
    Check(rejected, message);
}
void Put16(std::string& out, unsigned value) {
    out += static_cast<char>(value);
    out += static_cast<char>(value >> 8);
}
void Put32(std::string& out, unsigned value) {
    Put16(out, value);
    Put16(out, value >> 16);
}
// Minimal standards-compliant, uncompressed one-folder CAB assembled in memory.
std::string FakeCab(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string table, data;
    for (const auto& file : files) {
        Put32(table, static_cast<unsigned>(file.second.size()));
        Put32(table, static_cast<unsigned>(data.size()));
        Put16(table, 0);
        Put16(table, 0);
        Put16(table, 0);
        Put16(table, 0x20);
        table += file.first;
        table += '\0';
        data += file.second;
    }
    Check(data.size() < 32768, "fixture cabinet too large");
    unsigned offset = 44 + static_cast<unsigned>(table.size());
    std::string out = "MSCF";
    Put32(out, 0);
    Put32(out, offset + 8 + static_cast<unsigned>(data.size()));
    Put32(out, 0);
    Put32(out, 44);
    Put32(out, 0);
    out += '\3';
    out += '\1';
    Put16(out, 1);
    Put16(out, static_cast<unsigned>(files.size()));
    Put16(out, 0);
    Put16(out, 17);
    Put16(out, 0);
    Put32(out, offset);
    Put16(out, 1);
    Put16(out, 0);
    out += table;
    Put32(out, 0);
    Put16(out, static_cast<unsigned>(data.size()));
    Put16(out, static_cast<unsigned>(data.size()));
    out += data;
    return out;
}
struct Fixture {
    std::wstring root;
    Json manifest;
    std::map<int, std::string> resources;
    std::function<void(int)> beforeResource;
    std::function<void(const std::wstring&)> beforeLaunch;
    std::mutex mutex;
    std::vector<Process> processes;
    std::atomic<unsigned> launches{0}, closes{0}, forces{0}, polls{0};
    std::atomic<DWORD> launchError{ERROR_CANCELLED}, closeError{ERROR_SUCCESS};
    Fixture() {
        wchar_t temp[32768];
        DWORD n = GetTempPathW(32768, temp);
        Check(n > 0 && n < 32768, "temporary root unavailable");
        for (unsigned i = 0; i < 100; ++i) {
            root = std::wstring(temp) + L"BBS-tool-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i);
            if (CreateDirectoryW(Native(root).c_str(), nullptr))
                break;
            Check(GetLastError() == ERROR_ALREADY_EXISTS && i < 99, "cannot create unique fixture");
        }
        WriteNew(root + L"\\.test-owned", "tool-manager inert fixture");
        manifest = {{"format", 1}, {"tools", Json::array()}};
        const char* ids[] = {"rose", "akari", "ltk"};
        const char* versions[] = {"1.4.9", "1.5.1", "1.29.1"};
        const char* entries[] = {"Rose.exe", "Akari.exe", ""};
        for (int i = 0; i < 3; ++i) {
            std::string bytes = i == 2 ? "inert patcher host" : "inert executable fixture";
            std::string path = i == 2 ? "ltk_patcher_host.exe" : entries[i];
            Json files = Json::array({{{"cab", "f000000"},
                                       {"path", path},
                                       {"size", bytes.size()},
                                       {"sha256", Digest(INVALID_HANDLE_VALUE, &bytes)}}});
            manifest["tools"].push_back({{"id", ids[i]},
                                         {"name", ids[i]},
                                         {"version", versions[i]},
                                         {"entry", entries[i]},
                                         {"cab_resource", 3101 + i},
                                         {"elevated", i == 0},
                                         {"files", files},
                                         {"bindings", Json::array()}});
            resources[3101 + i] = FakeCab({{"f000000", bytes}});
        }
        manifest["tools"][0]["bindings"].push_back(
            {{"tool", "ltk"},
             {"from", "ltk_patcher_host.exe"},
             {"to", "_internal/injection/tools/ltk_patcher_host.exe"}});
        Update();
        testHooks.resource = [this](int id) -> Resource {
            if (beforeResource)
                beforeResource(id);
            auto found = resources.find(id);
            if (found == resources.end())
                Fail("Missing fake resource", ERROR_RESOURCE_NAME_NOT_FOUND);
            return {reinterpret_cast<const BYTE*>(found->second.data()), found->second.size()};
        };
        testHooks.processes = [this] {
            ++polls;
            std::lock_guard<std::mutex> lock(mutex);
            return processes;
        };
        testHooks.launch = [this](const std::wstring& path, bool elevated, bool cleanAkari) {
            Check(cleanAkari == (path.find(L"\\akari\\") != std::wstring::npos), "wrong child environment scope");
            Check(path.find(root + L"\\components\\") == 0, "launch escaped component root");
            Check(elevated == (path.find(L"\\rose\\") != std::wstring::npos), "wrong elevation verb");
            ++launches;
            if (beforeLaunch)
                beforeLaunch(path);
            return launchError.load();
        };
        testHooks.close = [this](DWORD pid, bool force) {
            Check(pid == 1234, "close targeted unrelated fake process");
            if (force)
                ++forces;
            else
                ++closes;
            return closeError.load();
        };
    }
    ~Fixture() {
        testHooks = {};
        // root is generated and newly created here, and cleanup requires its ownership marker.
        if (root.find(L"BBS-tool-test-") == std::wstring::npos)
            return;
        if (GetFileAttributesW(Native(root + L"\\.test-owned").c_str()) == INVALID_FILE_ATTRIBUTES)
            return;
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(Native(root)), error);
    }
    void Update() {
        resources[3100] = manifest.dump();
    }
    void SetProcesses(std::vector<Process> values) {
        std::lock_guard<std::mutex> lock(mutex);
        processes = std::move(values);
    }
};
ToolState Wait(ToolManager& manager, ToolId id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto state = manager.Snapshot()[Index(id)];
        if (!state.busy)
            return state;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("worker timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
template <class F> void Eventually(F predicate, const char* message) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(true, message);
}
void TestAkariEnvironment() {
    const wchar_t source[] = L"=C:=C:\\工作\0ELECTRON_RUN_AS_NODE=1\0"
                             L"eLeCtRoN_rUn_As_NoDe=0\0EMPTY=\0NODE_OPTIONS=--trace-warnings\0"
                             L"Path=C:\\工具\0ELECTRON_RUN_AS_NODE_EXTRA=keep\0VALUE=a=b\0";
    const wchar_t expected[] = L"=C:=C:\\工作\0EMPTY=\0NODE_OPTIONS=--trace-warnings\0"
                               L"Path=C:\\工具\0ELECTRON_RUN_AS_NODE_EXTRA=keep\0VALUE=a=b\0";
    auto filtered = AkariEnvironment(source);
    Check(filtered == std::vector<wchar_t>(expected, expected + sizeof(expected) / sizeof(wchar_t)),
          "environment filtering changed preserved entries or double NUL");
    Check(source[0] == L'=' && std::wstring(source + std::wstring(source).size() + 1) == L"ELECTRON_RUN_AS_NODE=1",
          "environment source changed");
    Check(AkariEnvironment(L"\0") == std::vector<wchar_t>({0, 0}), "empty environment not double NUL");
    Check(AkariEnvironment(L"ELECTRON_RUN_AS_NODE=1\0") == std::vector<wchar_t>({0, 0}),
          "all-filtered environment not double NUL");
    const wchar_t untouched[] = L"=D:=D:\\data\0SystemRoot=C:\\Windows\0";
    Check(AkariEnvironment(untouched) == std::vector<wchar_t>(untouched, untouched + sizeof(untouched) / sizeof(wchar_t)),
          "environment without target key changed");
}
void TestPaths() {
    Check(Relative("_internal/中文.dll") == L"_internal\\中文.dll", "Unicode mapping");
    for (const char* path : {"../escape", "/absolute", "C:/escape", "a/../../b", "a//b", "a/./b", "a/", "a.",
                             "a ", "a:stream", "NUL", "com1.exe", "a\\..\\b"})
        Reject([&] { Relative(path); }, "unsafe path accepted");
    std::string bytes = "abc";
    Check(Digest(INVALID_HANDLE_VALUE, &bytes) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256 vector");
}
void TestPrepareAndDryRun() {
    Fixture f;
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "dry initialize");
    Check(!manager.Launch(ToolId::Rose) && !manager.RequestClose(ToolId::Rose) &&
              !manager.ForceStop(ToolId::Rose) && !manager.SetFollowLobby(ToolId::Rose, true),
          "dry-run action authorization");
    Check(manager.Prepare(ToolId::Rose), "prepare accepted");
    ToolState rose = Wait(manager, ToolId::Rose);
    if (!rose.error.empty())
        std::wcerr << rose.error << L'\n';
    Check(rose.ready && rose.percent == 100 && rose.error.empty(), "staged preparation failed");
    Check(manager.Snapshot()[2].ready, "recursive LTK dependency not prepared");
    Check(ReadSmall(rose.directory + L"\\_internal\\injection\\tools\\ltk_patcher_host.exe") ==
              "inert patcher host",
          "dependency binding bytes wrong");
    Check(GetFileAttributesW(Native(rose.directory + L"\\.bbs-component.json").c_str()) !=
              INVALID_FILE_ATTRIBUTES,
          "completion marker missing");
    manager.ObserveLobby(1);
    manager.ObserveLobby(0);
    manager.Shutdown();
    Check(!f.launches && !f.closes && !f.forces, "dry run executed an action");
    Check(!manager.Prepare(ToolId::Rose), "shutdown accepted work");
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "reinitialize after joined shutdown");
    Check(manager.Prepare(ToolId::Rose), "marker reuse queued");
    Check(Wait(manager, ToolId::Rose).ready, "marker reuse failed");
    manager.Shutdown();
}
void TestFailures() {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f;
        if (mode == 0)
            f.manifest["tools"][1]["files"][0]["sha256"] = std::string(64, '0');
        if (mode == 1)
            f.resources[3102] = FakeCab({{"f999999", "unexpected"}});
        if (mode == 2)
            f.resources[3102] = "not a cabinet";
        if (mode == 3)
            f.manifest["tools"][1]["files"][0]["size"] = 999;
        f.Update();
        ToolManager manager;
        Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "failure fixture initialize");
        Check(manager.Prepare(ToolId::Akari), "bad payload prepare not queued");
        auto state = Wait(manager, ToolId::Akari);
        Check(!state.ready && !state.error.empty(), "corrupt payload incorrectly ready");
        Check(GetFileAttributesW(Native(state.directory + L"\\.bbs-component.json").c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
              "failed prepare committed marker");
        manager.Shutdown();
    }
    Fixture f;
    f.manifest["tools"][0]["files"][0]["path"] = "../outside";
    f.Update();
    ToolManager manager;
    Check(!manager.Initialize(nullptr, nullptr, 0, f.root, true), "traversal manifest accepted");
}
void TestProcessActionsAndFollow() {
    Fixture f;
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "action initialize");
    Check(!manager.Launch(ToolId::Ltk), "empty LTK entry launched");
    Check(!manager.Snapshot()[2].error.empty(), "empty-entry rejection lacks explanation");
    Check(manager.Launch(ToolId::Rose), "launch not accepted");
    auto state = Wait(manager, ToolId::Rose);
    Check(state.ready && !state.running && state.error.find(L"1223") != std::wstring::npos,
          "UAC cancellation not reported truthfully");
    f.SetProcesses({{44, L"C:\\fake-other-install\\Rose.exe"},
                    {55, f.root + L"\\Game\\Rose.exe"},
                    {1234, state.directory + L"\\Rose.exe"}});
    Eventually([&] { return manager.Snapshot()[0].pid == 1234; }, "exact entry PID not observed");
    const auto launchesBeforeReopen = f.launches.load();
    Check(manager.Launch(ToolId::Rose), "running Rose launch rejected");
    state = Wait(manager, ToolId::Rose);
    Check(state.running && !state.error.empty() && f.launches == launchesBeforeReopen,
          "running Rose duplicate or missing client notice");
    Check(manager.RequestClose(ToolId::Rose), "close not accepted");
    state = Wait(manager, ToolId::Rose);
    Check(state.running && state.pid == 1234 && f.closes == 1, "close falsely claimed exit");
    Check(manager.ForceStop(ToolId::Rose), "force not accepted");
    Check(Wait(manager, ToolId::Rose).running && f.forces == 1, "force scope or running observation wrong");
    f.SetProcesses({{44, L"C:\\fake-other-install\\Rose.exe"}});
    Eventually([&] { return !manager.Snapshot()[0].running; }, "unrelated image counted as running");
    Check(!manager.Snapshot()[0].followLobby, "follow default must be false");
    Check(manager.SetFollowLobby(ToolId::Rose, true), "follow preference not accepted");
    Eventually(
        [&] {
            try {
                return Json::parse(ReadSmall(f.root + L"\\tool-center.json"))["followLobby"]["rose"] == true;
            } catch (...) {
                return false;
            }
        },
        "follow preference not persisted");
    unsigned before = f.launches;
    manager.ObserveLobby(-1);
    manager.ObserveLobby(1);
    Wait(manager, ToolId::Rose);
    Check(f.launches == before + 1, "online edge did not launch exactly once");
    manager.ObserveLobby(-1);
    manager.ObserveLobby(1);
    Check(f.launches == before + 1, "unknown observation reset edge baseline");
    manager.ObserveLobby(0);
    Wait(manager, ToolId::Rose);
    manager.Shutdown();
    unsigned closed = f.closes, forced = f.forces;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "preferences reload initialize");
    Eventually([&] { return manager.Snapshot()[0].followLobby; }, "follow preference reload failed");
    manager.Shutdown();
    Check(f.closes == closed && f.forces == forced, "shutdown closed third-party processes");
}
struct Gate {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, released = false;
        void Block() {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    ~Gate() {
        Release();
    }
    void Release() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
        state->changed.notify_all();
    }
    void WaitEntered() {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->changed.wait_for(lock, std::chrono::seconds(4), [&] { return state->entered; }))
            throw std::runtime_error("deterministic worker gate timed out");
    }
};
void TestFollowBusyReconciliation() {
    Fixture f;
    ToolManager manager;
    Gate preparation, launching;
    f.launchError = ERROR_SUCCESS;
    f.beforeResource = [gate = preparation.state](int id) {
        if (id == 3101)
            gate->Block();
    };
    f.beforeLaunch = [&, gate = launching.state](const std::wstring& path) {
        f.SetProcesses({{1234, path}});
        gate->Block();
    };
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "busy-follow initialize");
    Check(manager.SetFollowLobby(ToolId::Rose, true), "busy-follow enabled");
    manager.ObserveLobby(1);
    preparation.WaitEntered();
    manager.ObserveLobby(0); // Retain this edge while the automatic launch is preparing.
    preparation.Release();
    Eventually([&] { return manager.Snapshot()[0].state == L"Stopped"; },
               "offline intent lost while preparing");
    Check(f.launches == 0, "stale online command launched after offline preparation edge");
    manager.ObserveLobby(1);
    launching.WaitEntered();
    manager.ObserveLobby(0); // Launch has happened, but its command has not completed yet.
    launching.Release();
    Eventually([&] { return f.closes == 1 && !manager.Snapshot()[0].busy; },
               "offline intent lost while launch command was busy");
    Check(f.launches == 1 && manager.Snapshot()[0].running,
          "online recovery duplicated launch or hid running PID");
    Check(manager.SetFollowLobby(ToolId::Rose, false), "disable busy-follow");
    f.SetProcesses({});
    Eventually([&] { return !manager.Snapshot()[0].running; }, "fake process removal not observed");
    Check(manager.Launch(ToolId::Rose), "manual offline launch rejected");
    Wait(manager, ToolId::Rose);
    Check(f.launches == 2 && f.closes == 1, "manual launch incorrectly gated by offline follow state");
    manager.Shutdown();
}
void TestEmbeddedBinding() {
    Fixture f;
    std::string bytes = "inert patcher host";
    f.manifest["tools"][0]["files"].push_back({{"cab", "f000001"},
                                               {"path", "_internal/injection/tools/ltk_patcher_host.exe"},
                                               {"size", bytes.size()},
                                               {"sha256", Digest(INVALID_HANDLE_VALUE, &bytes)}});
    f.resources[3101] = FakeCab({{"f000000", "inert executable fixture"}, {"f000001", bytes}});
    f.Update();
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "overlay initialize");
    Check(manager.Prepare(ToolId::Rose), "overlay prepare accepted");
    auto state = Wait(manager, ToolId::Rose);
    Check(state.ready && state.error.empty(), "identical embedded binding was rejected");
    auto marker = Json::parse(ReadSmall(state.directory + L"\\.bbs-component.json"));
    Check(marker["files"] == 2, "overlay binding file count not deduplicated");
    manager.Shutdown();
}
// These small bytes are the ORIGINAL profile of this fake embedded manifest, never the
// approved official profile. Real signed payloads and their private positive probes stay local.
struct PairFixture : Fixture {
    std::string host = "inert original host pair", dll = "inert original dll pair!";
    std::string prefix = "_internal/injection/tools/";
    PairFixture() {
        auto file = [](const char* cab, const std::string& path, const std::string& bytes) {
            return Json{{"cab", cab},
                        {"path", path},
                        {"size", bytes.size()},
                        {"sha256", Digest(INVALID_HANDLE_VALUE, &bytes)}};
        };
        manifest["tools"][2]["files"] = Json::array(
            {file("f000000", "ltk_patcher_host.exe", host), file("f000001", "ltk_patcher_dll.dll", dll)});
        resources[3103] = FakeCab({{"f000000", host}, {"f000001", dll}});
        manifest["tools"][0]["files"].push_back(file("f000001", prefix + "ltk_patcher_host.exe", host));
        manifest["tools"][0]["files"].push_back(file("f000002", prefix + "ltk_patcher_dll.dll", dll));
        manifest["tools"][0]["bindings"].push_back(
            {{"tool", "ltk"}, {"from", "ltk_patcher_dll.dll"}, {"to", prefix + "ltk_patcher_dll.dll"}});
        resources[3101] =
            FakeCab({{"f000000", "inert executable fixture"}, {"f000001", host}, {"f000002", dll}});
        Update();
    }
    std::wstring Target(const ToolState& rose, const ToolState& ltk, int index) const {
        std::string name = index % 2 == 0 ? "ltk_patcher_host.exe" : "ltk_patcher_dll.dll";
        return (index < 2 ? rose.directory : ltk.directory) + L"\\" +
               Relative((index < 2 ? prefix : "") + name);
    }
    void Replace(const std::wstring& path, const std::string& bytes) {
        Check(path.find(root + L"\\components\\") == 0, "pair mutation escaped fixture");
        Handle output(CreateFileW(Native(path).c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        Check(output.value != INVALID_HANDLE_VALUE, "cannot mutate fixture runtime");
        DWORD written = 0;
        Check(WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size(),
              "cannot write fixture runtime");
    }
};
void TestOriginalRuntimePair() {
    PairFixture f;
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "pair initialize");
    Check(manager.Prepare(ToolId::Rose), "pair prepare queued");
    auto rose = Wait(manager, ToolId::Rose);
    auto ltk = manager.Snapshot()[2];
    Check(rose.ready && ltk.ready && rose.error.empty(), "complete original pair rejected");
    for (int index = 0; index < 4; ++index)
        Check(ReadSmall(f.Target(rose, ltk, index)) == (index % 2 == 0 ? f.host : f.dll),
              "original pair binding changed bytes");
    Check(manager.Prepare(ToolId::Rose), "cached pair reuse queued");
    Check(Wait(manager, ToolId::Rose).ready, "cached original pair reuse rejected");
    manager.Shutdown();
    // Marker reuse must validate the same four targets without rereading either CAB.
    f.beforeResource = [](int id) {
        if (id != 3100)
            throw std::runtime_error("marker reuse unexpectedly requested a cabinet");
    };
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "pair reinitialize");
    Check(manager.Prepare(ToolId::Rose), "original pair marker reuse queued");
    Check(Wait(manager, ToolId::Rose).ready && manager.Snapshot()[2].ready,
          "original pair marker reuse failed");
    Check(f.launches == 0, "pair preparation launched a process");
    manager.Shutdown();
}
void TestUnicodeAndLongPaths() {
    Fixture f;
    std::wstring root = f.root + L"\\中文";
    for (int i = 0; i < 12; ++i) root += L"\\long-component-directory";
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, root, true), "long Unicode initialize");
    Check(manager.Prepare(ToolId::Akari), "long Unicode prepare");
    Check(Wait(manager, ToolId::Akari).ready, "long Unicode preparation failed");
    manager.Shutdown();
}
void TestRuntimePairTampering() {
    for (int index = 0; index < 4; ++index) {
        PairFixture f;
        ToolManager manager;
        Check(manager.Initialize(nullptr, nullptr, 0, f.root), "1.29.1 tamper initialize");
        Check(manager.Prepare(ToolId::Rose), "1.29.1 pair prepare");
        auto rose = Wait(manager, ToolId::Rose);
        auto ltk = manager.Snapshot()[2];
        Check(rose.ready && ltk.ready, "1.29.1 pair baseline");
        auto bytes = index % 2 == 0 ? f.host : f.dll;
        bytes[0] ^= 1;
        f.Replace(f.Target(rose, ltk, index), bytes);
        Check(manager.Launch(ToolId::Rose), "1.29.1 tamper launch queued");
        auto failed = Wait(manager, ToolId::Rose);
        Check(!failed.ready && !failed.error.empty() && f.launches == 0, "1.29.1 same-size runtime tamper launched");
        manager.Shutdown();
    }
}
void TestRuntimePairTampering1290() {
    for (int index = 0; index < 4; ++index) {
        PairFixture f;
        f.manifest["tools"][2]["version"] = "1.29.0";
        f.Update();
        ToolManager manager;
        Check(manager.Initialize(nullptr, nullptr, 0, f.root), "1.29.0 tamper initialize");
        Check(manager.Prepare(ToolId::Rose), "1.29.0 pair prepare");
        auto rose = Wait(manager, ToolId::Rose);
        auto ltk = manager.Snapshot()[2];
        Check(rose.ready && ltk.ready, "1.29.0 pair baseline");
        auto bytes = index % 2 == 0 ? f.host : f.dll;
        bytes[0] ^= 1;
        f.Replace(f.Target(rose, ltk, index), bytes);
        Check(manager.Launch(ToolId::Rose), "1.29.0 tamper launch queued");
        auto failed = Wait(manager, ToolId::Rose);
        Check(!failed.ready && !failed.error.empty() && f.launches == 0, "1.29.0 same-size runtime tamper launched");
        manager.Shutdown();
    }
}
void TestStorageVersion() {
    Fixture f;
    auto parse = [&] { f.Update(); auto& bytes = f.resources[3100];
        return Parse({reinterpret_cast<const BYTE*>(bytes.data()), bytes.size()}, f.root); };
    auto initial = parse();
    Check(initial[0].storageVersion == L"1.4.9", "storage default changed");
    for (const char* value : {"../escape", "nested/leaf", "nested\\leaf", "C:/escape", "NUL", "leaf.", "leaf ", ""}) {
        f.manifest["tools"][0]["storage_version"] = value;
        Reject(parse, "unsafe storage_version accepted");
    }
    f.manifest["tools"][0]["storage_version"] = "1.4.9-ltk1.29.1";
    auto defs = parse();
    Check(defs[0].version == L"1.4.9" && defs[0].directory == f.root + L"\\components\\rose\\1.4.9-ltk1.29.1",
          "storage version changed application version or escaped root");
    { Directories dirs; dirs.Ensure(initial[0].directory, true); }
    WriteNew(initial[0].directory + L"\\user.txt", "old Rose userdata");
    WriteNew(f.root + L"\\tool-center.json", R"({"format":1,"followLobby":{"rose":true,"akari":false}})");
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "storage initialize");
    Eventually([&] { return manager.Snapshot()[0].followLobby; }, "root config was not loaded");
    Check(manager.Prepare(ToolId::Rose), "storage prepare queued");
    auto state = Wait(manager, ToolId::Rose);
    Check(state.ready && state.version == L"1.4.9", "fresh storage prepare failed");
    Check(ReadSmall(initial[0].directory + L"\\user.txt") == "old Rose userdata", "old storage modified");
    manager.Shutdown();
}
void TestLegacyAkari() {
    Fixture f;
    const auto source = std::filesystem::path(Wide(__FILE__)).parent_path().parent_path() / "resources" / "integrations" / "manifest.json";
    Json actual = Json::parse(ReadSmall(Absolute(source.wstring())));
    Json akari;
    for (const auto& item : actual.at("tools"))
        if (item.at("id") == "akari") akari = item;
    Check(!akari.is_null(), "canonical Akari metadata absent");
    f.manifest["tools"][1] = akari;
    f.Update();
    auto parse = [&] { auto bytes = f.manifest.dump();
        return Parse({reinterpret_cast<const BYTE*>(bytes.data()), bytes.size()}, f.root); };
    auto definitions = parse();
    auto d = definitions[1];
    Check(d.legacyAkari && FileCount(d) == 71, "unchanged Akari item not recognized");
    Json marker{{"format", 1}, {"files", 71}, {"stamp", "84ac9f2ff62ea41de1c1452790e32ee80ed52b56692a229dac5ff3c39e79a83d"}};
    Check(MarkerAccepted(d, marker), "exact legacy marker rejected");
    { Directories dirs; dirs.Ensure(d.directory, true); }
    const auto originalMarker = marker.dump();
    WriteNew(d.directory + L"\\.bbs-component.json", originalMarker);
    WriteNew(d.directory + L"\\user.txt", "legacy userdata");
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "legacy initialize");
    Check(manager.Prepare(ToolId::Akari), "legacy incomplete prepare queued");
    Check(!Wait(manager, ToolId::Akari).ready, "legacy marker bypassed full file checks");
    Check(ReadSmall(d.directory + L"\\.bbs-component.json") == originalMarker &&
          ReadSmall(d.directory + L"\\user.txt") == "legacy userdata", "legacy marker or userdata modified");
    manager.Shutdown();
    for (const char* key : {"name", "version", "storage_version"}) {
        f.manifest["tools"][1] = akari;
        f.manifest["tools"][1][key] = "changed";
        auto changed = parse()[1];
        Check(!changed.legacyAkari && !MarkerAccepted(changed, marker), "changed Akari accepted legacy stamp");
    }
    for (const char* key : {"format", "files", "stamp"}) {
        auto changed = marker;
        if (std::string(key) == "stamp") changed[key] = "unapproved";
        else changed[key] = 0;
        Check(!MarkerAccepted(d, changed), "inexact legacy marker accepted");
    }
}
void TestReopenWindowEligibility() {
    const RECT mainWindow{10, 20, 935, 590};
    const RECT emptyWidth{10, 20, 10, 590};
    const RECT emptyHeight{10, 20, 935, 20};
    auto eligible = [&](bool abc, bool owner, LONG_PTR style, int title, bool visible, bool valid, const RECT& rect) {
        return ReopenWindowEligible(owner, style, title, abc, visible, valid, rect);
    };
    Check(eligible(true, false, 0, 0, true, true, mainWindow), "ABC custom-drawn main window rejected");
    Check(!eligible(false, false, 0, 0, true, true, mainWindow), "Akari untitled filter relaxed");
    Check(eligible(false, false, 0, 1, false, false, emptyWidth), "Akari titled policy changed");
    Check(eligible(false, false, WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, 1, false, false, emptyWidth),
          "Akari existing titled extended-style policy changed");
    for (bool abc : {false, true}) {
        Check(!eligible(abc, true, 0, 1, true, true, mainWindow), "owned helper selected");
        Check(!eligible(abc, false, WS_EX_TOOLWINDOW, 1, true, true, mainWindow), "tool window selected");
    }
    Check(!eligible(true, true, 0, 0, true, true, mainWindow), "untitled owned helper selected");
    for (LONG_PTR style : {static_cast<LONG_PTR>(WS_EX_TOOLWINDOW), static_cast<LONG_PTR>(WS_EX_NOACTIVATE),
                           static_cast<LONG_PTR>(WS_EX_TRANSPARENT)})
        Check(!eligible(true, false, style, 0, true, true, mainWindow), "ABC untitled shadow/helper selected");
    Check(!eligible(true, false, 0, 0, false, true, mainWindow), "ABC hidden untitled helper selected");
    Check(!eligible(true, false, 0, 0, true, false, mainWindow), "ABC unknown rectangle accepted");
    Check(!eligible(true, false, 0, 0, true, true, emptyWidth), "ABC zero-width helper selected");
    Check(!eligible(true, false, 0, 0, true, true, emptyHeight), "ABC zero-height helper selected");
}
void TestReopenAndCachedLaunch() {
    Fixture f;
    ToolManager manager;
    f.launchError = ERROR_SUCCESS;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "reopen initialize");
    Check(manager.Prepare(ToolId::Akari), "reopen prepare");
    auto akari = Wait(manager, ToolId::Akari);
    unsigned reopened = 0;
    testHooks.reopen = [&](DWORD pid) { Check(pid == 1234, "reopen wrong PID"); ++reopened; return true; };
    f.SetProcesses({{1234, akari.directory + L"\\Akari.exe"}, {9999, L"C:\\unrelated\\Akari.exe"}});
    Check(manager.Launch(ToolId::Akari), "running Akari launch queued");
    Check(Wait(manager, ToolId::Akari).error.empty() && reopened == 1 && f.launches == 0,
          "running Akari did not restore owned window");
    testHooks.reopen = [](DWORD) { return false; };
    Check(manager.Launch(ToolId::Akari), "unverified fallback queued");
    Check(!Wait(manager, ToolId::Akari).error.empty() && f.launches == 0, "unverified single-instance fallback launched");
    f.SetProcesses({});
    f.beforeResource = [](int) { throw std::runtime_error("cached launch reread resource"); };
    Check(manager.Launch(ToolId::Akari), "cached launch queued");
    Check(Wait(manager, ToolId::Akari).error.empty() && f.launches == 1, "cached launch failed");
    manager.Shutdown();
}
void AddAbc(Fixture& f) {
    const std::string bytes = "inert ABC executable";
    f.manifest["tools"].push_back({{"id", "abc"}, {"name", "ABC"}, {"version", "local"},
        {"storage_version", "local-testhash"}, {"entry", "ABC.exe"}, {"cab_resource", 3104},
        {"elevated", false}, {"bindings", Json::array()}, {"files", Json::array({
            {{"cab", "f000000"}, {"path", "ABC.exe"}, {"size", bytes.size()},
             {"sha256", Digest(INVALID_HANDLE_VALUE, &bytes)}}})}});
    const std::string config = "clean seed";
    f.manifest["tools"][3]["files"].push_back({{"cab", "f000001"}, {"path", "data/config.json"},
        {"size", config.size()}, {"sha256", Digest(INVALID_HANDLE_VALUE, &config)}, {"mutable", true}});
    const std::string dll = "inert DLL fixture";
    f.manifest["tools"][3]["files"].push_back({{"cab", "f000002"}, {"path", "lib/runtime.dll"},
        {"size", dll.size()}, {"sha256", Digest(INVALID_HANDLE_VALUE, &dll)}});
    const std::string toml = "debug = false";
    f.manifest["tools"][3]["files"].push_back({{"cab", "f000003"}, {"path", "lib/config.toml"},
        {"size", toml.size()}, {"sha256", Digest(INVALID_HANDLE_VALUE, &toml)}, {"mutable", true}});
    f.resources[3104] = FakeCab({{"f000000", bytes}, {"f000001", config}, {"f000002", dll}, {"f000003", toml}});
    f.Update();
}
void TestAbc() {
    Check(Index(ToolId::Rose) == 0 && Index(ToolId::Akari) == 1 && Index(ToolId::Ltk) == 2 &&
          Index(ToolId::Abc) == 3 && Index("abc") == 3, "stable tool IDs");
    Fixture f;
    AddAbc(f);
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "ABC smoke initialize");
    Check(manager.Snapshot().size() == 4, "ABC fourth component absent");
    Check(!manager.Launch(ToolId::Abc) && !manager.RequestClose(ToolId::Abc) &&
          !manager.ForceStop(ToolId::Abc) && !manager.SetFollowLobby(ToolId::Abc, true), "ABC smoke guard");
    Check(manager.Prepare(ToolId::Abc), "ABC prepare queued");
    auto abc = Wait(manager, ToolId::Abc);
    Check(abc.ready && abc.version == L"local" && abc.directory.find(L"local-testhash") != std::wstring::npos,
          "ABC prepare/storage failed");
    const auto receipt = ReadSmall(abc.directory + L"\\.bbs-component.json");
    WriteNew(abc.directory + L"\\user-config.ini", "private mutable config");
    {
        Handle config(CreateFileW(Native(abc.directory + L"\\data\\config.json").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        const std::string modified = "user edited config with different size"; DWORD written = 0;
        Check(WriteFile(config.value, modified.data(), static_cast<DWORD>(modified.size()), &written, nullptr) && SetEndOfFile(config.value), "edit ABC fixture config");
    }
    {
        Handle file(CreateFileW(Native(abc.directory + L"\\lib\\config.toml").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        const std::string toml = "debug = true # changed by application"; DWORD written = 0;
        Check(WriteFile(file.value, toml.data(), static_cast<DWORD>(toml.size()), &written, nullptr) && SetEndOfFile(file.value), "edit inert TOML seed");
    }
    manager.Shutdown();
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "ABC lifecycle reinitialize");
    f.launchError = ERROR_SUCCESS;
    Check(manager.Launch(ToolId::Abc), "ABC launch queued");
    Check(Wait(manager, ToolId::Abc).error.empty() && f.launches == 1, "ABC launch failed");
    unsigned reopened = 0;
    testHooks.reopen = [&](DWORD pid) { Check(pid == 1234, "ABC reopen unrelated PID"); ++reopened; return true; };
    f.SetProcesses({{9999, L"C:\\unrelated\\ABC.exe"}, {1234, abc.directory + L"\\ABC.exe"}});
    Check(manager.Launch(ToolId::Abc), "ABC reopen queued");
    Check(Wait(manager, ToolId::Abc).error.empty() && reopened == 1 && f.launches == 1, "ABC owned reopen duplicated");
    Check(ReadSmall(abc.directory + L"\\.bbs-component.json") == receipt &&
          ReadSmall(abc.directory + L"\\user-config.ini") == "private mutable config", "ABC receipt/config overwritten");
    Check(ReadSmall(abc.directory + L"\\data\\config.json") == "user edited config with different size", "ABC mutable seed overwritten");
    Check(ReadSmall(abc.directory + L"\\lib\\config.toml") == "debug = true # changed by application", "ABC TOML seed overwritten");
    Check(!manager.SetFollowLobby(ToolId::Abc, true), "ABC lobby follow allowed");
    manager.ObserveLobby(1); manager.ObserveLobby(0);
    Check(manager.RequestClose(ToolId::Abc), "ABC close queued");
    Check(Wait(manager, ToolId::Abc).error.empty() && f.closes == 1 && f.forces == 0, "ABC close scope");
    manager.Shutdown();
    Fixture bad; AddAbc(bad); bad.resources[3104] = "malformed CAB";
    Check(manager.Initialize(nullptr, nullptr, 0, bad.root, true), "ABC malformed initialize");
    Check(manager.Prepare(ToolId::Abc), "ABC malformed queued");
    auto failed = Wait(manager, ToolId::Abc);
    Check(!failed.ready && !failed.error.empty() && bad.launches == 0, "ABC malformed CAB accepted");
    manager.Shutdown();
}
void TestAbcLogDirectories() {
    Fixture f; AddAbc(f);
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "ABC logs initialize");
    Check(manager.Prepare(ToolId::Abc), "ABC logs prepare queued");
    const auto state = Wait(manager, ToolId::Abc);
    Check(state.ready, "ABC logs prepare failed");
    for (const auto& suffix : {L"\\Log", L"\\lib\\logs"})
        Check(std::filesystem::is_empty(std::filesystem::path(Native(state.directory + suffix))), "ABC log directory not empty/new");
    WriteNew(state.directory + L"\\Log\\existing.log", "preserve app log");
    WriteNew(state.directory + L"\\lib\\logs\\existing.log", "preserve library log");
    const auto marker = ReadSmall(state.directory + L"\\.bbs-component.json");
    Check(manager.Prepare(ToolId::Abc), "ABC logs cached prepare queued");
    Check(Wait(manager, ToolId::Abc).ready, "ABC logs cached prepare failed");
    manager.Shutdown();
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "ABC logs owned cold initialize");
    Check(manager.Prepare(ToolId::Abc), "ABC logs owned cold prepare queued");
    Check(Wait(manager, ToolId::Abc).ready, "ABC logs owned cold failed");
    Check(ReadSmall(state.directory + L"\\Log\\existing.log") == "preserve app log" &&
          ReadSmall(state.directory + L"\\lib\\logs\\existing.log") == "preserve library log" &&
          ReadSmall(state.directory + L"\\.bbs-component.json") == marker, "ABC log/receipt overwritten");
    manager.Shutdown();
    Definition abc; abc.id = "abc";
    const auto blocked = f.root + L"\\blocked";
    { Directories dirs; dirs.Ensure(blocked, true); }
    WriteNew(blocked + L"\\Log", "not a directory");
    Reject([&] { EnsureAbcLogDirectories(abc, blocked); }, "ABC log non-directory accepted");
    const auto linked = f.root + L"\\linked";
    const auto target = f.root + L"\\outside";
    { Directories dirs; dirs.Ensure(linked, true); dirs.Ensure(target, true); }
    if (CreateSymbolicLinkW(Native(linked + L"\\Log").c_str(), Native(target).c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        Reject([&] { EnsureAbcLogDirectories(abc, linked); }, "ABC log reparse followed");
        Check(std::filesystem::is_empty(std::filesystem::path(Native(target))), "ABC log reparse target modified");
    } else {
        Check(GetLastError() == ERROR_PRIVILEGE_NOT_HELD || GetLastError() == ERROR_NOT_SUPPORTED, "unexpected inert symlink failure");
        std::cout << "ABC log reparse fixture skipped: symlink privilege unavailable\n";
    }
    Definition rose; rose.id = "rose";
    const auto untouched = f.root + L"\\other-component";
    EnsureAbcLogDirectories(rose, untouched);
    Check(GetFileAttributesW(Native(untouched).c_str()) == INVALID_FILE_ATTRIBUTES, "log dirs leaked to other component");
}
void TestAbcExclusiveDatabase() {
    Fixture f; AddAbc(f);
    f.manifest["tools"][3]["files"][1]["path"] = "data/user.db";
    f.Update();
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root), "exclusive database initialize");
    Check(manager.Prepare(ToolId::Abc), "exclusive database initial prepare queued");
    const auto state = Wait(manager, ToolId::Abc);
    Check(state.ready, "exclusive database initial prepare failed");
    const auto path = state.directory + L"\\data\\user.db";
    const auto marker = ReadSmall(state.directory + L"\\.bbs-component.json");
    const std::string changed = "vendor-owned database with changed length";
    {
        Handle database(CreateFileW(Native(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        Check(database.value != INVALID_HANDLE_VALUE, "exclusive database open failed");
        DWORD written = 0;
        Check(WriteFile(database.value, changed.data(), static_cast<DWORD>(changed.size()), &written, nullptr) &&
              written == changed.size() && SetEndOfFile(database.value), "exclusive database edit failed");
        Handle conflict(CreateFileW(Native(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr));
        Check(conflict.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
              "database fixture is not exclusively locked");
        Check(manager.Prepare(ToolId::Abc), "exclusive database cached prepare queued");
        Check(Wait(manager, ToolId::Abc).ready, "exclusive database cached prepare rejected");
        manager.Shutdown();
        Check(manager.Initialize(nullptr, nullptr, 0, f.root), "exclusive database cold initialize");
        Check(manager.Prepare(ToolId::Abc), "exclusive database cold prepare queued");
        Check(Wait(manager, ToolId::Abc).ready, "exclusive database cold prepare rejected");
        unsigned reopened = 0;
        testHooks.reopen = [&](DWORD pid) { Check(pid == 1234, "exclusive database wrong PID"); ++reopened; return true; };
        f.SetProcesses({{1234, state.directory + L"\\ABC.exe"}});
        Check(manager.Launch(ToolId::Abc), "exclusive database reopen queued");
        Check(Wait(manager, ToolId::Abc).error.empty() && reopened == 1 && f.launches == 0,
              "exclusive database reopen failed or duplicated");
        LARGE_INTEGER zero{};
        Check(SetFilePointerEx(database.value, zero, nullptr, FILE_BEGIN), "vendor handle invalidated");
        std::string observed(changed.size() + 1, '\0'); DWORD read = 0;
        Check(ReadFile(database.value, &observed[0], static_cast<DWORD>(observed.size()), &read, nullptr) &&
              observed.substr(0, read) == changed, "vendor database bytes changed");
    }
    manager.Shutdown();
    Check(ReadSmall(path) == changed && ReadSmall(state.directory + L"\\.bbs-component.json") == marker,
          "exclusive database or receipt replaced");
    Check(DeleteFileW(Native(path).c_str()) != FALSE, "exclusive database delete fixture failed");
    auto defs = Parse({reinterpret_cast<const BYTE*>(f.resources[3100].data()), f.resources[3100].size()}, f.root);
    std::atomic<bool> stop{false};
    Reject([&] { CheckFile(state.directory, defs[3].files[1], true, stop, true); }, "missing mutable database accepted");
    const auto target = f.root + L"\\database-target";
    WriteNew(target, "untouched target");
    if (CreateSymbolicLinkW(Native(path).c_str(), Native(target).c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        Reject([&] { CheckFile(state.directory, defs[3].files[1], true, stop, true); }, "reparse mutable database accepted");
        Check(ReadSmall(target) == "untouched target", "reparse database target changed");
        Check(DeleteFileW(Native(path).c_str()) != FALSE, "delete database reparse fixture failed");
    } else {
        std::cout << "ABC database reparse fixture skipped: symlink privilege unavailable\n";
    }
    Check(CreateHardLinkW(Native(path).c_str(), Native(target).c_str(), nullptr) != FALSE, "database hardlink fixture failed");
    Reject([&] { CheckFile(state.directory, defs[3].files[1], true, stop, true); }, "hardlinked mutable database accepted");
    Check(DeleteFileW(Native(path).c_str()) != FALSE, "delete database hardlink fixture failed");
    Check(ReadSmall(target) == "untouched target", "hardlinked database target changed");
}
void TestAbcMutableSafety() {
    {
        Fixture f; AddAbc(f);
        f.manifest["tools"][3]["files"].erase(3); // vary the approved seed path without duplicate TOML entry
        auto parse = [&] { auto bytes = f.manifest.dump(); return Parse({reinterpret_cast<const BYTE*>(bytes.data()), bytes.size()}, f.root); };
        for (const char* path : {"data/config.json", "data/user.db", "data/abc.ini", "data/updates.ini", "lib/config.toml"}) {
            f.manifest["tools"][3]["files"][1]["path"] = path;
            Check(parse()[3].files[1].mutableData, "approved mutable ABC seed rejected");
        }
        for (const char* path : {"ABC.exe", "lib/runtime.dll", "data/run.bat", "data/other.json"}) {
            f.manifest["tools"][3]["files"][1]["path"] = path;
            Reject(parse, "unapproved mutable ABC path accepted");
        }
        f.manifest["tools"][3]["files"][1]["path"] = "data/config.json";
        f.manifest["tools"][0]["files"][0]["mutable"] = true;
        Reject(parse, "mutable flag leaked to Rose");
    }
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f; AddAbc(f);
        if (mode == 0) f.resources[3104] = FakeCab({{"f000000", "inert ABC executable"}, {"f000001", "wrong seed"}, {"f000002", "inert DLL fixture"}, {"f000003", "debug = false"}});
        ToolManager manager;
        Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "ABC seed safety initialize");
        Check(manager.Prepare(ToolId::Abc), "ABC seed safety prepare queued");
        auto state = Wait(manager, ToolId::Abc);
        if (mode == 0) {
            Check(!state.ready && !state.error.empty(), "fresh mutable seed hash mismatch accepted");
            manager.Shutdown(); continue;
        }
        Check(state.ready, "ABC safety baseline prepare failed");
        const auto marker = ReadSmall(state.directory + L"\\.bbs-component.json");
        manager.Shutdown();
        if (mode == 1) {
            Check(DeleteFileW(Native(state.directory + L"\\data\\config.json").c_str()) != FALSE, "delete inert seed fixture");
        } else {
            Handle file(CreateFileW(Native(state.directory + L"\\lib\\runtime.dll").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            const std::string tampered = "INERT DLL fixture"; DWORD written = 0;
            Check(WriteFile(file.value, tampered.data(), static_cast<DWORD>(tampered.size()), &written, nullptr) && SetEndOfFile(file.value), "mutate inert DLL fixture");
        }
        Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "ABC safety reinitialize");
        Check(manager.Prepare(ToolId::Abc), "ABC missing/tampered prepare queued");
        auto failed = Wait(manager, ToolId::Abc);
        Check(!failed.ready && !failed.error.empty(), "ABC missing seed or DLL tamper accepted");
        Check(ReadSmall(state.directory + L"\\.bbs-component.json") == marker && f.launches == 0,
              "ABC safety failure changed receipt or launched process");
        if (mode == 1) Check(GetFileAttributesW(Native(state.directory + L"\\data\\config.json").c_str()) == INVALID_FILE_ATTRIBUTES,
                             "missing mutable seed silently recreated");
        manager.Shutdown();
    }
}
void Test336Receipts() {
    Fixture f;
    const auto source = std::filesystem::path(Wide(__FILE__)).parent_path().parent_path() / "resources" / "integrations" / "manifest.json";
    auto actual = Json::parse(ReadSmall(Absolute(source.wstring())));
    // This regression pins the unchanged original three specs; use an inert ABC spec below.
    actual["tools"].erase(std::remove_if(actual["tools"].begin(), actual["tools"].end(),
        [](const Json& j) { return j.at("id") == "abc"; }), actual["tools"].end());
    auto parse = [&] { auto bytes = actual.dump(); return Parse({reinterpret_cast<const BYTE*>(bytes.data()), bytes.size()}, f.root); };
    auto defs = parse();
    const std::string oldStamp = "5fe7b6700c9d074453a88994f0e30182057414e960f999918867ab4d7aef0a49";
    for (int i : {0, 1, 2}) {
        Json marker{{"format", 1}, {"files", FileCount(defs[i])}, {"stamp", oldStamp}};
        const auto unchanged = marker.dump();
        Check(defs[i].legacy336 && MarkerAccepted(defs[i], marker), "exact 3.3.6 marker rejected");
        Check(marker.dump() == unchanged, "3.3.6 marker mutated");
        auto changed = defs[i]; changed.legacy336 = false; changed.stamp = "other";
        Check(!MarkerAccepted(changed, marker), "unrelated spec accepted old marker");
    }
    AddAbc(f);
    actual["tools"].erase(std::remove_if(actual["tools"].begin(), actual["tools"].end(), [](const Json& j) { return j.at("id") == "abc"; }), actual["tools"].end());
    actual["tools"].push_back(f.manifest["tools"][3]);
    defs = parse();
    Check(defs[0].legacy336 && defs[1].legacy336 && defs[2].legacy336, "ABC addition invalidated old specs");
    Check(!MarkerAccepted(defs[3], {{"format", 1}, {"files", FileCount(defs[3])}, {"stamp", oldStamp}}), "ABC accepted old unrelated receipt");
    actual["tools"][2]["name"] = "changed";
    defs = parse();
    Check(!defs[0].legacy336 && !defs[2].legacy336, "changed LTK dependency adopted old Rose receipt");
}
void TestPreserveExisting() {
    Fixture f;
    std::wstring component = f.root + L"\\components\\akari\\1.5.1";
    {
        Directories dirs;
        dirs.Ensure(component, true);
        WriteNew(component + L"\\user.txt", "preserve me");
    }
    WriteNew(f.root + L"\\outside-user.txt", "outside preserve me");
    ToolManager manager;
    Check(manager.Initialize(nullptr, nullptr, 0, f.root, true), "preserve initialize");
    Check(manager.Prepare(ToolId::Akari), "preserve prepare accepted");
    auto state = Wait(manager, ToolId::Akari);
    Check(!state.ready && !state.error.empty(), "unmanaged existing version overwritten");
    Check(ReadSmall(component + L"\\user.txt") == "preserve me", "existing user data changed");
    Check(ReadSmall(f.root + L"\\outside-user.txt") == "outside preserve me", "outside data changed");
    manager.Shutdown();
}
} // namespace
// Keep the Unicode console entry compatible with the production compiler flags.
int main() {
    try {
        TestAbc();
        TestAbcLogDirectories();
        TestAbcMutableSafety();
        TestAbcExclusiveDatabase();
        Test336Receipts();
        TestAkariEnvironment();
        TestPaths();
        TestPrepareAndDryRun();
        TestFailures();
        TestProcessActionsAndFollow();
        TestPreserveExisting();
        TestUnicodeAndLongPaths();
        TestEmbeddedBinding();
        TestFollowBusyReconciliation();
        TestOriginalRuntimePair();
        TestRuntimePairTampering();
        TestRuntimePairTampering1290();
        TestStorageVersion();
        TestLegacyAkari();
        TestReopenWindowEligibility();
        TestReopenAndCachedLaunch();
        std::cout << "tool manager: " << checks << " checks passed (inert fixtures only)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "tool manager FAILED: " << e.what() << '\n';
        return 1;
    }
}
