#include "../src/core/tool_manager.h"
#include <iostream>
#include <string>
#include <windows.h>

// Explicit maintainer probe. Never starts the backup service or observes a lobby.
// Default only prepares Rose. Only explicit --launch-* requests open an application.
int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc == 4 ? argv[3] : L"";
    const bool launch = mode == L"--launch-rose" || mode == L"--launch-akari" || mode == L"--launch-abc";
    const bool abc = mode == L"--launch-abc" || mode == L"--prepare-abc";
    const bool akari = mode == L"--launch-akari" || mode == L"--prepare-akari";
    if ((argc != 3 && argc != 4) || (!mode.empty() && !launch && !akari && !abc)) {
        std::wcerr << L"Usage: probe <BBS.exe> <data-root> [--launch-rose|--launch-akari|--prepare-akari|--prepare-abc|--launch-abc]\n";
        return 2;
    }
    HMODULE module =
        LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module)
        return 2;
    int result = 1;
    {
        core::ToolManager manager;
        const auto target = abc ? core::ToolId::Abc : akari ? core::ToolId::Akari : core::ToolId::Rose;
        if (manager.Initialize(module, nullptr, 0, argv[2], false) &&
            (launch ? manager.Launch(target) : manager.Prepare(target))) {
            const ULONGLONG deadline = GetTickCount64() + 120000;
            ULONGLONG firstRunning = 0;
            while (GetTickCount64() < deadline) {
                bool failed = false;
                for (const auto& s : manager.Snapshot()) {
                    if (s.id != target || s.busy)
                        continue;
                    if (!s.error.empty()) {
                        failed = true;
                        break;
                    }
                    if (!launch && s.ready) {
                        result = 0;
                        break;
                    }
                    if (launch && s.running) {
                        if (!firstRunning)
                            firstRunning = GetTickCount64();
                        if (GetTickCount64() - firstRunning >= 2000)
                            result = 0;
                    } else
                        firstRunning = 0;
                }
                if (result == 0 || failed)
                    break;
                Sleep(40);
            }
            for (const auto& s : manager.Snapshot())
                std::wcout << L"tool=" << static_cast<int>(s.id) << L" ready=" << s.ready << L" busy="
                           << s.busy << L" running=" << s.running << L" pid=" << s.pid << L" directory="
                           << s.directory << L" state=" << s.state << L" error=" << s.error << L"\n";
        }
        manager.Shutdown();
    }
    FreeLibrary(module);
    std::wcout << L"service_started=0 explicit_launch_requests=" << launch << L" process_stops=0 result="
               << result << L"\n";
    return result;
}
