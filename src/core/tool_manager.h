#pragma once
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace core {
enum class ToolId { Rose = 0, Akari = 1, Ltk = 2, Abc = 3 };
struct ToolState {
    ToolId id = ToolId::Rose;
    std::wstring name, version, directory, state, error;
    bool bundled = false, ready = false, running = false, busy = false;
    bool elevated = false, followLobby = false;
    DWORD pid = 0;
    int percent = -1;
};
class ToolManager {
  public:
    ToolManager();
    ~ToolManager();
    ToolManager(const ToolManager&) = delete;
    ToolManager& operator=(const ToolManager&) = delete;
    bool Initialize(HINSTANCE module, HWND notifyWindow, UINT notifyMessage, const std::wstring& dataRoot,
                    bool dryRun = false);
    std::vector<ToolState> Snapshot() const;
    bool Prepare(ToolId id);
    bool Launch(ToolId id);
    bool RequestClose(ToolId id);
    bool ForceStop(ToolId id);
    bool SetFollowLobby(ToolId id, bool enabled);
    void ObserveLobby(int rawState);
    void Shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace core
