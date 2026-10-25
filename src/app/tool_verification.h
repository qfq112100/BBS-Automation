#pragma once
// Isolated package verification: extract trusted embedded apps, never launch them or start the service.
#include "../../third_party/json/json.hpp"
#include "../core/tool_manager.h"
#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

namespace integration_verification {
inline std::string Utf8(const std::wstring& value) {
    if (value.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "invalid UTF-16";
    std::string text(size, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), &text[0],
                        size, nullptr, nullptr);
    return text;
}
inline bool Report(const std::wstring& root, const nlohmann::json& data) {
    const std::wstring file = L"\\\\?\\" + root + L"\\verification.json";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const auto text = data.dump(2);
    DWORD done = 0;
    const bool okay = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &done, nullptr) &&
                      done == text.size();
    CloseHandle(handle);
    return okay;
}
inline int Run(HINSTANCE instance, const std::wstring& root) {
    using core::ToolId;
    core::ToolManager manager;
    nlohmann::json report{{"success", false},
                          {"dry_run", true},
                          {"applications_launched", 0},
                          {"tools", nlohmann::json::array()}};
    const ULONGLONG started = GetTickCount64();
    bool okay = manager.Initialize(instance, nullptr, 0, root, true);
    if (okay) {
        for (const ToolId id : {ToolId::Ltk, ToolId::Rose, ToolId::Akari}) {
            if (!manager.Prepare(id)) {
                okay = false;
                break;
            }
            const ULONGLONG deadline = GetTickCount64() + 300000;
            bool finished = false;
            while (GetTickCount64() < deadline) {
                const auto states = manager.Snapshot();
                auto state = std::find_if(states.begin(), states.end(),
                                          [id](const core::ToolState& item) { return item.id == id; });
                if (state != states.end() && !state->busy && state->ready) {
                    finished = true;
                    break;
                }
                if (state != states.end() && !state->busy && !state->error.empty())
                    break;
                Sleep(25);
            }
            if (!finished) {
                okay = false;
                break;
            }
        }
    }
    for (const auto& state : manager.Snapshot()) {
        report["tools"].push_back({{"id", static_cast<int>(state.id)},
                                   {"name", Utf8(state.name)},
                                   {"version", Utf8(state.version)},
                                   {"directory", Utf8(state.directory)},
                                   {"ready", state.ready},
                                   {"running", state.running},
                                   {"error", Utf8(state.error)}});
        if (!state.ready || state.running)
            okay = false;
    }
    manager.Shutdown();
    report["success"] = okay;
    report["elapsed_ms"] = GetTickCount64() - started;
    return Report(root, report) ? (okay ? 0 : 1) : 3;
}
} // namespace integration_verification
