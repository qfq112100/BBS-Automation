// Observations are installation-bound; lifecycle edges and write guards use distinct session policies.
#pragma once
#include <string>
#include <windows.h>
namespace core {
enum class LobbyState { Online, Offline, Unknown };
struct LobbyProbe {
    LobbyState state = LobbyState::Unknown;
    DWORD error = ERROR_SUCCESS;
    std::wstring message;
};
namespace monitor_detail {
// Pure policy seam: lifecycle edges are local, but any session may be using shared Game files.
constexpr bool IncludesSession(bool writeSafety, DWORD currentSession, DWORD candidateSession) noexcept {
    return writeSafety || candidateSession == currentSession;
}
} // namespace monitor_detail
class Monitor {
  public:
    // gameRoot is the configured ...\League of Legends\Game directory.
    // Unknown must NOT advance an offline/online edge baseline or authorize work.
    static LobbyProbe Probe(const std::wstring& gameRoot);
    // Actual match process at gameRoot\League of Legends.exe, current Windows session only.
    // This lifecycle probe must not let another session trigger this user's match-end backup.
    static LobbyProbe ProbeGame(const std::wstring& gameRoot);
    // Write/delete guard for shared Game files: includes every Windows session.
    // Unqueryable candidates fail closed; only a verified different installation is ignored.
    static LobbyProbe ProbeGameForWriteSafety(const std::wstring& gameRoot);
    // Compatibility only: current-session name scan, not installation validation.
    // Never use this legacy bool to authorize backup/restore or detect rising edges.
    static bool IsLobbyOnline();
};
} // namespace core
