// Native Windows desktop entry. No browser, runtime download, HTML or legacy tray host.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "../common/limits_shared.h"
#include "../common/platform.h"
#include "../common/window_geometry.h"
#include "../core/config.h"
#include "../core/file_ops.h"
#include "../core/game_detect.h"
#include "../core/logger.h"
#include "../core/service.h"
#include "../core/tool_manager.h"
#include "legal_zh.h"
#include "ui_assets.h"
#include "ui_geometry.h"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <dbghelp.h>
#include <deque>
#include <dwmapi.h>
#include <functional>
#include <gdiplus.h>
#include <map>
#include <memory>
#include <mutex>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <windowsx.h>
#include <winver.h>
#define JSON_DIAGNOSTICS 0
#include "../../third_party/json/json.hpp"

namespace nativeui {
using json = nlohmann::json;
constexpr wchar_t ClassName[] = L"LOLBackupV3.NativeWindow";
constexpr wchar_t AppTitle[] = L"BBS自动工具";
constexpr UINT ServiceEvents = WM_APP + 3, TrayEvent = WM_APP + 2;
constexpr UINT UiResultMessage = WM_APP + 10, WorkerStopped = WM_APP + 11;
constexpr UINT WelcomeCheck = WM_APP + 17, SingleInstanceWake = WM_APP + 18;
constexpr UINT ToolEvents = WM_APP + 23;
std::wstring ApplicationVersionLabel() {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT bytes = 0;
    if (!data || !VerQueryValueW(data, L"\\", reinterpret_cast<LPVOID*>(&info), &bytes) ||
        !info || bytes < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
        return L"版本未知";
    return L"v" + std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." +
           std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." +
           std::to_wstring(LOWORD(info->dwFileVersionLS));
}
constexpr int PageCount = 5;
constexpr int NavPageOrder[PageCount] = {0, 1, 2, 4, 3};
constexpr int NavPosition(int pageId) {
    for (int i = 0; i < PageCount; ++i)
        if (NavPageOrder[i] == pageId) return i;
    return pageId;
}
constexpr int ToolFirst = 600, ToolStride = 10;
constexpr int VisibleToolCount = 3;
constexpr core::ToolId ToolIds[VisibleToolCount] = {core::ToolId::Rose, core::ToolId::Akari, core::ToolId::Abc};
constexpr UINT_PTR RefreshTimer = 1, SmokeTimer = 2, CaptureTimer = 3;
constexpr int NavFirst = 100, ThemeQuick = 110, About = 111;
constexpr int QuickRose = 112, QuickAkari = 113, QuickAbc = 114, OpenDataRoot = 115, OpenManual = 116;
constexpr int WinMin = 120, WinMax = 121, WinClose = 122;
constexpr int Backup = 200, Restore = 201, Cancel = 202, Auto = 203;
constexpr int PickGame = 210, PickCache = 211;
constexpr int OpenGame = 220, OpenCache = 221, OpenLogs = 223;
constexpr int Import = 230, OldCache = 231, GoSettings = 232;
constexpr int RefreshLogs = 240, Save = 250;
constexpr int GameEdit = 300, CacheEdit = 301, LogEdit = 303;
constexpr int SyncEdit = 310, ThemeCombo = 312;
constexpr int CloseCombo = 313, StartupCombo = 314;
constexpr int TrayShow = 401, TrayAuto = 402, TrayExit = 403;
// Default isolated-preview runtime used by --smoke and by the preview command line.
constexpr int SmokePreviewDefaultMs = 5000;
// Accepted bounds for an explicit --smoke-ms override: five seconds to one hour.
constexpr int SmokePreviewMinMs = 5000;
constexpr int SmokePreviewMaxMs = 3600000;
// Delay before the UI reports that shutdown is still waiting for safe cancellation.
constexpr ULONGLONG ShutdownWarnMs = 5000;
constexpr int BodyTop = 160, BodyFooterInset = 48;
constexpr bool IsBodyControl(int id) {
    return id >= Backup && id != Save;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr,
                                nullptr);
    if (!n)
        throw std::runtime_error("Invalid UTF-16 text");
    std::string out(n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], n, nullptr,
                             nullptr))
        throw std::runtime_error("UTF-8 conversion failed");
    return out;
}
std::wstring Wide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 text");
    std::wstring out(n, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], n))
        throw std::runtime_error("UTF-16 conversion failed");
    return out;
}
std::wstring JString(const json& j, const char* key, const std::wstring& fallback = L"") {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? Wide(it->get<std::string>()) : fallback;
}
// Stable UI event codes; never serialize params/config, clipboard, file contents or exception text.
void UiLog(const wchar_t* code, const std::wstring& message, int severity = 0) noexcept {
    try {
        const std::wstring text = L"[" + std::wstring(code) + L"] " + message;
        if (severity >= 2)
            core::Log().Error(L"UI", text);
        else if (severity == 1)
            core::Log().Warn(L"UI", text);
        else
            core::Log().Info(L"UI", text);
    } catch (...) {
        // Logging must not change a UI decision, cancellation or shutdown path.
    }
}
bool LoggedUiMethod(const std::wstring& method) {
    return method == L"save_settings" || method == L"set_game_root" || method == L"set_cache_root" ||
           method == L"backup_now" || method == L"restore_now" || method == L"import_cache" ||
           method == L"set_auto" || method == L"cancel_op";
}
std::wstring UiRequestLabel(const std::wstring& method, int id) {
    return L"method=" + method + L" request_id=" + std::to_wstring(id);
}
std::wstring UiLocalDirectory(const std::wstring& path) {
    // Do not log UNC/URL/device paths or anything that could be a credential-bearing URI.
    if (path.size() < 3 || !iswalpha(path[0]) || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/'))
        return path.empty() ? L"（空）" : L"（非本地盘符目录，已省略）";
    std::wstring safe = path.substr(0, 1024);
    for (auto& c : safe)
        if (c < L' ' || c == 0x7f)
            c = L' ';
    if (safe.size() < path.size())
        safe += L"…";
    return safe;
}
std::wstring WinError(DWORD code = GetLastError()) {
    wchar_t* p = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPWSTR>(&p), 0, nullptr);
    std::wstring result = n && p ? std::wstring(p, n) : L"Windows 错误 " + std::to_wstring(code);
    if (p)
        LocalFree(p);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();
    return result;
}
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p);
    std::wstring result = SUCCEEDED(hr) && p ? p : L"";
    CoTaskMemFree(p);
    return result;
}
bool AbsolutePath(const std::wstring& path) {
    return (path.size() >= 3 && iswalpha(path[0]) && path[1] == L':' &&
            (path[2] == L'\\' || path[2] == L'/')) ||
           (path.size() > 3 && path[0] == L'\\' && path[1] == L'\\');
}

std::wstring FullPath(const std::wstring& path) {
    DWORD need = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!need || need > 32767)
        return {};
    std::vector<wchar_t> buffer(need + 1);
    DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return count && count < buffer.size() ? std::wstring(buffer.data(), count) : L"";
}
std::wstring ProcessUserSid(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (!bytes) {
        CloseHandle(token);
        return {};
    }
    std::vector<BYTE> data(bytes);
    if (!GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes)) {
        CloseHandle(token);
        return {};
    }
    LPWSTR sid = nullptr;
    bool ok = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid) != FALSE;
    CloseHandle(token);
    if (!ok || !sid)
        return {};
    std::wstring identity(sid);
    LocalFree(sid);
    return identity;
}
std::wstring InstanceMutexName() {
    const auto sid = ProcessUserSid(GetCurrentProcess());
    return sid.empty() ? L"" : L"Global\\LOLBackupV3." + sid + L".NativeUI";
}
struct InstanceWindowSearch {
    std::wstring sid;
    HWND window = nullptr;
    DWORD processId = 0;
};
BOOL CALLBACK FindInstanceWindow(HWND window, LPARAM data) {
    try {
        auto& search = *reinterpret_cast<InstanceWindowSearch*>(data);
        if (search.sid.empty())
            return FALSE;
        wchar_t klass[128]{};
        if (!GetClassNameW(window, klass, 128) || wcscmp(klass, ClassName) != 0)
            return TRUE;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        winutil::ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        // Failure to inspect an account is NOT permission to wake its window.
        if (!process.valid() || ProcessUserSid(process.get()) != search.sid)
            return TRUE;
        search.window = window;
        search.processId = processId;
        return FALSE;
    } catch (...) {
        return TRUE; // No allocation/Win32 callback exception may cross EnumWindows.
    }
}

// A missing settings DIRECTORY on first use is not a corrupt config. Existing files,
// inaccessible paths and invalid JSON still go through the strict locked reader.
bool LoadStartupConfig(const std::wstring& settings, core::Config& out) {
    const auto path = settings + L"\\config.json";
    if (GetFileAttributesW(winutil::LongPath(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            const DWORD attributes = GetFileAttributesW(winutil::LongPath(settings).c_str());
            const DWORD parentError = GetLastError();
            std::wstring error;
            if (attributes == INVALID_FILE_ATTRIBUTES &&
                (parentError == ERROR_FILE_NOT_FOUND || parentError == ERROR_PATH_NOT_FOUND) &&
                pathpolicy::ValidateRoot(settings, error)) {
                out = core::Config{}; // Service re-reads under lock after safely preparing its directory.
                return true;
            }
        }
    }
    return core::Config::LoadSafe(path, out);
}
struct BootstrapRootEdit {
    core::Config expected;
    int id;
    std::wstring path;
    std::atomic<bool>* cancelled;
    bool conflict = false;
};
bool ApplyBootstrapRoot(core::Config& value, void* data) {
    auto& edit = *static_cast<BootstrapRootEdit*>(data);
    if (edit.cancelled->load() || core::Service::Instance().IsRunning())
        return false;
    if (value.gameRoot != edit.expected.gameRoot || value.cacheRoot != edit.expected.cacheRoot) {
        edit.conflict = true;
        return false;
    }
    if (edit.id == PickGame)
        value.gameRoot = edit.path;
    else if (edit.id == PickCache)
        value.cacheRoot = edit.path;
    else
        return false;
    return true; // Only the selected root changes; latest preferences/unknown fields survive.
}

// All ownership is explicit. Selected objects are restored before deletion.
struct GdiObject {
    HGDIOBJ handle = nullptr;
    ~GdiObject() {
        if (handle)
            DeleteObject(handle);
    }
    void Reset(HGDIOBJ value = nullptr) {
        if (handle)
            DeleteObject(handle);
        handle = value;
    }
    GdiObject() = default;
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
};
struct PaintSession {
    HWND hwnd;
    PAINTSTRUCT ps{};
    HDC dc;
    explicit PaintSession(HWND w) : hwnd(w), dc(BeginPaint(w, &ps)) {}
    ~PaintSession() {
        EndPaint(hwnd, &ps);
    }
};
struct Buffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    Buffer(HDC source, int w, int h) {
        dc = CreateCompatibleDC(source);
        if (dc)
            bitmap = CreateCompatibleBitmap(source, std::max(1, w), std::max(1, h));
        if (bitmap)
            previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) {
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            bitmap = nullptr;
            dc = nullptr;
            previous = nullptr;
        }
    }
    ~Buffer() {
        if (previous)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
    }
};
template <class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p)
            p->Release();
    }
    T** Put() {
        return &p;
    }
    T* operator->() const {
        return p;
    }
};
struct WinRect {
    int x, y, w, h;
};
bool ApplyWinRect(core::Config& c, void* data) {
    const auto& r = *static_cast<WinRect*>(data);
    // Always return true: UpdateAtomic also reports false for a no-op.
    c.winX = r.x;
    c.winY = r.y;
    c.winW = r.w;
    c.winH = r.h;
    return true;
}

// SEH path uses only precomputed storage and raw APIs; never enters Logger or a C++ mutex.
wchar_t DumpPath[common::kMaxPathChars] = {};
LONG volatile CrashEntered = 0;
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedCompareExchange(&CrashEntered, 1, 0) != 0)
        return EXCEPTION_EXECUTE_HANDLER;
    if (DumpPath[0]) {
        HANDLE f = CreateFileW(DumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{};
            info.ThreadId = GetCurrentThreadId();
            info.ExceptionPointers = ep;
            info.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal,
                              ep ? &info : nullptr, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    OutputDebugStringW(L"LOL Backup: unhandled exception; see local application logs/crash.dmp.\n");
    return EXCEPTION_EXECUTE_HANDLER;
}

struct Palette {
    COLORREF bg, side, card, field, text, muted, border, accent, accentSoft, danger, good, hero;
};
Palette Colors(bool light) {
    if (light)
        return {RGB(247, 247, 251), RGB(240, 240, 246), RGB(255, 255, 255), RGB(247, 247, 251),
                RGB(32, 31, 43),    RGB(125, 123, 142), RGB(228, 227, 237), RGB(114, 83, 224),
                RGB(239, 234, 255), RGB(194, 65, 92),   RGB(31, 144, 117),  RGB(28, 23, 48)};
    return {RGB(17, 17, 21),    RGB(22, 22, 27),    RGB(25, 25, 31),   RGB(19, 19, 24),
            RGB(239, 238, 246), RGB(143, 141, 161), RGB(45, 44, 55),   RGB(166, 139, 250),
            RGB(44, 35, 66),    RGB(242, 127, 150), RGB(90, 203, 162), RGB(28, 23, 48)};
}
Gdiplus::Color GColor(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

struct UiResult {
    enum Kind { Boot, Recovery, Notice, Opened } kind = Notice;
    bool ok = false, configValid = false;
    std::wstring message, legacy;
    core::Config config;
};
struct Pending {
    std::wstring method, success;
    ULONGLONG sent = 0;
    bool silent = false, warned = false;
};
struct Control {
    HWND hwnd = nullptr;
    int id = 0;
    int page = -1; // -1: global
    int tone = 0;  // 0 secondary, 1 primary, 2 danger, 3 navigation
};

class App {
  public:
    HINSTANCE instance;
    HWND hwnd = nullptr;
    bool smoke = false;
    std::wstring root;
    explicit App(HINSTANCE h, std::wstring dataRoot, bool isSmoke, int initialPage, bool previewLight,
                 std::wstring captureFile, int clientWidth = 0, int clientHeight = 0)
        : instance(h), smoke(isSmoke), root(std::move(dataRoot)), page(initialPage), smokeTheme(previewLight),
          capturePath(std::move(captureFile)) {
        previewWidth = clientWidth;
        previewHeight = clientHeight;
        light = smokeTheme;
        palette = Colors(light);
    }
    ~App() {
        if (!stopping)
            UiLog(L"UI_EXIT_FALLBACK", L"主循环结束但未走常规退出；执行析构安全停止", 1);
        startupRepairCancelled = true;
        // Normally Stop has already completed on our worker. If the message pump fails,
        // this final guard must not depend on delivery of WorkerStopped or any other message.
        core::Service::Instance().RequestStop();
        if (worker.joinable()) {
            {
                std::lock_guard<std::mutex> lock(workMutex);
                if (!stopping)
                    work.clear();
                workerFinish = true;
            }
            workCv.notify_one();
            worker.join();
        }
        if (!serviceStopComplete.load()) {
            core::Service::Instance().Stop();
            serviceStopComplete = true;
        }
        toolManager.Shutdown(); // Fallback after the UI worker joined; safe when already stopped.
        MSG leftover{};
        while (PeekMessageW(&leftover, hwnd, UiResultMessage, UiResultMessage, PM_REMOVE))
            delete reinterpret_cast<UiResult*>(leftover.lParam);
        if (trayAdded) {
            Shell_NotifyIconW(NIM_DELETE, &tray);
            trayAdded = false;
        }
        if (trayMenu) {
            DestroyMenu(trayMenu);
            trayMenu = nullptr;
        }
        if (hwnd && IsWindow(hwnd)) {
            ShutdownBlockReasonDestroy(hwnd);
            DestroyWindow(hwnd);
        }
    }
    int Run(int show);
    void ConfigureSmokePreview(int forcedDpi, bool showWelcome, int durationMs) {
        if (smoke) {
            previewDpi = forcedDpi;
            previewWelcome = showWelcome;
            smokeDurationMs = durationMs;
        }
    }

  private:
    int dpi = 96, page = 0, width = 1240, height = 813, requestId = 1;
    int scrollY = 0, wheelRemainder = 0, scrollDrag = 0, paintOffset = 0;
    int previewWidth = 0, previewHeight = 0;
    int previewDpi = 0, smokeDurationMs = SmokePreviewDefaultMs;
    int smokeLobbyPreview = -2; // UI-only status override; ignored outside isolated smoke instances.
    int smokeAutoPreview = -1;  // Render-only automatic-state acceptance cases, never service mutation.
    bool previewWelcome = false;
    bool layingOut = false;
    HWND licenseWindow = nullptr, trayPopup = nullptr, modalWindow = nullptr;
    UiAssets assets;
    core::ToolManager toolManager;
    std::vector<core::ToolState> toolStates;
    bool toolsInitialized = false;
    std::wstring toolDirectoryPaths[VisibleToolCount];
    bool toolDirectoryAvailable[VisibleToolCount]{};
    ULONGLONG toolDirectoriesCheckedAt = 0;
    struct UiPerf {
        uint64_t paints = 0, textDraws = 0, textSkips = 0, logoDraws = 0;
        uint64_t directoryQueries = 0, dirtyPixels = 0, layouts = 0;
    } uiPerf;
    std::wstring toolsStartupError;
    bool light = false, initialized = false, stopping = false, serviceStarted = false;
    bool settingsDirty = false, syncingControls = false, errorNotice = false, trayAdded = false;
    int pendingPageAfterSave = -1;
    bool pendingCloseAfterSave = false;
    bool trayVersion4 = false;
    ULONGLONG trayMenuShownAt = 0;
    bool shutdownWarned = false, smokeTheme = false, captured = false, welcomeShown = false;
    bool windowShown = false, uiReady = false, welcomePending = false;
    bool bootstrapBusy = true, bootstrapFailed = false, bootstrapConfigValid = false;
    std::atomic<bool> startupRepairCancelled{false};
    struct DialogState {
        App* app;
        HWND window = nullptr;
        std::wstring title, body;
        int result = 0, hot = -1;
        bool closeChoice = false, failed = false, defaultCancel = false;
        bool allowRemember = false, remember = false;
        explicit DialogState(App* owner) : app(owner) {}
    };
    int initialShow = SW_SHOWNORMAL;
    ULONGLONG startedAt = 0;
    std::wstring capturePath;
    ULONGLONG noticeUntil = 0, stopSince = 0, lastLogsRequest = 0;
    std::wstring notice = L"正在启动本地服务…", legacyCache, lastError;
    core::Service::Status status;
    core::Config config;
    Palette palette = Colors(false);
    GdiObject normalFont, smallFont, titleFont, heroFont, metricFont, brandFont, iconFont, fieldBrush,
        sideBrush;
    std::unique_ptr<Buffer> paintBuffer;
    int paintBufferWidth = 0, paintBufferHeight = 0;
    std::unique_ptr<Gdiplus::Bitmap> heroPixels;
    int heroPixelWidth = 0, heroPixelHeight = 0;
    std::vector<Control> controls;
    std::map<int, Pending> pending;
    std::map<std::wstring, bool> pollFailed;
    unsigned bootstrapWakeCount = 0;
    bool serviceMessageMalformed = false, uiExceptionReported = false;
    HMENU trayMenu = nullptr;
    NOTIFYICONDATAW tray{};
    UINT taskbarCreated = 0;
    std::thread worker;
    std::mutex workMutex;
    std::condition_variable workCv;
    std::deque<std::function<void()>> work;
    bool workerFinish = false;
    std::atomic<bool> serviceStopComplete{false};
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK LicenseProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TrayMenuProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(UINT, WPARAM, LPARAM);
    int D(int n) const {
        return MulDiv(n, dpi, 96);
    }
    RECT R(int x, int y, int w, int h) const {
        return {D(x), D(y - paintOffset), D(x + w), D(y + h - paintOffset)};
    }
    int Left() const {
        return 256;
    }
    int ContentWidth() const {
        return width - Left() - 32;
    }
    int BodyBottom() const {
        return std::max(BodyTop, height - BodyFooterInset);
    }
    RECT BodyViewport() const {
        return {D(224), D(BodyTop), D(width), D(BodyBottom())};
    }
    HWND Item(int id) const;
    void MakeControl(int id, int controlPage, const wchar_t* klass, const wchar_t* text, DWORD style,
                     int tone = 0);
    void MakeControls();
    void Fonts();
    void ApplyTheme();
    void ApplyNativeCaption(HWND window) const;
    bool SettingsEdited() const;
    bool Confirm(const std::wstring& title, const std::wstring& body, bool defaultCancel = false);
    static LRESULT CALLBACK ConfirmProc(HWND, UINT, WPARAM, LPARAM);
    int AskCloseDialog();
    int AskUnsavedDialog();
    static LRESULT CALLBACK UnsavedAskProc(HWND, UINT, WPARAM, LPARAM);
    bool GuardUnsaved(int nextPage, bool closeAfter);
    int RunDialog(DialogState& state, WNDPROC procedure, const wchar_t* klass, int width, int height);
    void QueueWelcome();
    void MaybeShowWelcome();
    static LRESULT CALLBACK CloseAskProc(HWND, UINT, WPARAM, LPARAM);
    void Layout(bool scrolling = false);
    void SetPage(int value);
    void UpdateControls(bool force = false);
    void EnableControls();
    void Paint(HDC);
    void PrintClient(HDC);
    void DrawButton(DRAWITEMSTRUCT*);
    void PaintButton(DRAWITEMSTRUCT*);
    void Text(HDC, const std::wstring&, RECT, HFONT, COLORREF,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    void Round(HDC, RECT, COLORREF, COLORREF, int radius = 12);
    void Card(HDC, int x, int y, int w, int h, bool hero = false);
    void NavIcon(HDC, int nav, RECT, COLORREF);
    void Icon(HDC, wchar_t glyph, RECT, COLORREF);
    void Line(HDC, int x, int y, int w, COLORREF);
    void Hero(HDC, int x, int y, int w, int h);
    void InputFrame(HDC, int id, int x, int y, int w, int h = 40);
    void DrawSegments(DRAWITEMSTRUCT*);
    int PageBottom() const;
    int MaxScroll() const;
    void ScrollTo(int);
    void EnsureVisible(HWND);
    void ShowAbout();
    void ShowManual();
    std::wstring licenseText, licenseOriginalText, iconRenderDiagnostic;
    bool licenseShowingOriginal = false;
    void Notify(const std::wstring&, bool error = false, bool balloon = true);
    void Send(int id);
    int Request(const std::wstring&, const json& = json::object(), const std::wstring& success = L"",
                bool silent = false);
    bool HasPending(const std::wstring&) const;
    bool Busy() const;
    void Refresh();
    void RefreshTools();
    bool RefreshToolDirectories();
    void InvalidateLogScrollBar();
    const core::ToolState* ToolStatus(int index) const;
    void SendTool(int id);
    void PaintTools(HDC dc, int x, int cw);
    void Drain();
    void HandleEvent(const json&);
    void ShowLogs(const json&);
    void RequestLogs();
    bool SaveSettings();
    void Capture();
    std::wstring PickDirectory(const std::wstring&, const std::wstring& = L"");
    void Pick(int id);
    void Open(const std::wstring&);
    void Queue(std::function<void()>);
    void PostResult(std::unique_ptr<UiResult>);
    void StartWorker();
    core::Service::StartOptions StartupOptions() const;
    void EffectiveStartupRoots(core::Config&) const;
    void ShowFailedConfig();
    bool RetryStartup();
    void QueueRootRecovery(int, const std::wstring&, const core::Config&);
    void RecoverRoot(int, const std::wstring&, const core::Config&, const std::wstring&);
    void BootService(const std::wstring& fontDiagnostic);
    void RevealWindow();
    void Shutdown(const wchar_t* reason = L"界面退出");
    void ClosePolicy();
    bool InitTray();
    void ShowTrayMenu();
    void TrayBalloon(const std::wstring&, bool);
    WinRect Placement() const;
    void SavePlacement();
    void RestorePlacement(const core::Config&);
};

HWND App::Item(int id) const {
    for (const auto& c : controls)
        if (c.id == id)
            return c.hwnd;
    return nullptr;
}
void App::MakeControl(int id, int controlPage, const wchar_t* klass, const wchar_t* text, DWORD style,
                      int tone) {
    HWND child = CreateWindowExW(0, klass, text, WS_CHILD | WS_TABSTOP | style, 0, 0, 1, 1, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (!child)
        throw std::runtime_error("Failed to create a native control");
    controls.push_back({child, id, controlPage, tone});
    if (wcscmp(klass, L"BUTTON") == 0 &&
        !SetWindowSubclass(child, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this)))
        throw std::runtime_error("Failed to initialize button input");
    if (wcscmp(klass, L"EDIT") == 0 &&
        !SetWindowSubclass(child, EditProc, 1, reinterpret_cast<DWORD_PTR>(this)))
        throw std::runtime_error("Failed to initialize text input");
}
void App::MakeControls() {
    const wchar_t* nav[] = {L"概览", L"备份任务", L"运行日志", L"设置", L"工具中心"};
    for (int i = 0; i < PageCount; ++i)
        MakeControl(NavFirst + NavPageOrder[i], -1, L"BUTTON", nav[NavPageOrder[i]], BS_OWNERDRAW, 3);
    MakeControl(ThemeQuick, -1, L"BUTTON", L"浅色外观", BS_OWNERDRAW);
    MakeControl(OpenManual, -1, L"BUTTON", L"使用说明书", BS_OWNERDRAW);
    MakeControl(WinMin, -1, L"BUTTON", L"", BS_OWNERDRAW);
    MakeControl(WinMax, -1, L"BUTTON", L"", BS_OWNERDRAW);
    MakeControl(WinClose, -1, L"BUTTON", L"", BS_OWNERDRAW);
    MakeControl(About, -1, L"BUTTON", L"关于与开源许可", BS_OWNERDRAW);
    MakeControl(OpenDataRoot, -1, L"BUTTON", L"打开总目录", BS_OWNERDRAW);
    MakeControl(QuickAbc, 0, L"BUTTON", L"打开 ABC", BS_OWNERDRAW);
    MakeControl(QuickRose, 0, L"BUTTON", L"打开 Rose", BS_OWNERDRAW);
    MakeControl(QuickAkari, 0, L"BUTTON", L"打开 Akari", BS_OWNERDRAW);
    MakeControl(Backup, 0, L"BUTTON", L"立即备份", BS_OWNERDRAW, 1);
    MakeControl(PickGame, 0, L"BUTTON", L"选择目录…", BS_OWNERDRAW);
    MakeControl(OpenCache, 0, L"BUTTON", L"打开缓存", BS_OWNERDRAW);
    // Buttons shared between overview/task/settings are positioned and shown explicitly by Layout.
    MakeControl(Restore, 1, L"BUTTON", L"恢复到游戏…", BS_OWNERDRAW, 2);
    MakeControl(Cancel, 1, L"BUTTON", L"取消任务", BS_OWNERDRAW);
    MakeControl(Auto, 1, L"BUTTON", L"大厅联动已开启", BS_OWNERDRAW);
    MakeControl(Import, 1, L"BUTTON", L"导入已有缓存…", BS_OWNERDRAW);
    MakeControl(OldCache, 1, L"BUTTON", L"查看旧缓存", BS_OWNERDRAW);
    MakeControl(GoSettings, 1, L"BUTTON", L"调整计划", BS_OWNERDRAW);
    MakeControl(RefreshLogs, 2, L"BUTTON", L"刷新日志", BS_OWNERDRAW);
    MakeControl(OpenLogs, 2, L"BUTTON", L"打开日志文件夹", BS_OWNERDRAW);
    MakeControl(LogEdit, 2, L"EDIT", L"正在载入运行日志…", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
    for (int id : {GameEdit, CacheEdit})
        MakeControl(id, 3, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL);
    MakeControl(PickCache, 3, L"BUTTON", L"选择…", BS_OWNERDRAW);
    MakeControl(OpenGame, 3, L"BUTTON", L"打开", BS_OWNERDRAW);
    MakeControl(SyncEdit, 3, L"EDIT", L"5", ES_NUMBER | ES_AUTOHSCROLL);
    for (int id : {ThemeCombo, CloseCombo, StartupCombo})
        MakeControl(id, 3, L"BUTTON",
                    id == ThemeCombo   ? L"界面主题"
                    : id == CloseCombo ? L"关闭窗口时"
                                       : L"开机启动",
                    BS_OWNERDRAW);
    for (const auto* t : {L"深色", L"浅色"})
        SendMessageW(Item(ThemeCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    for (const auto* t : {L"每次询问", L"最小化到托盘", L"退出程序"})
        SendMessageW(Item(CloseCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    for (const auto* t : {L"关闭", L"开机启动"})
        SendMessageW(Item(StartupCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    MakeControl(Save, 3, L"BUTTON", L"保存设置", BS_OWNERDRAW, 1);
    for (int i = 0; i < VisibleToolCount; ++i) {
        const int base = ToolFirst + i * ToolStride;
        MakeControl(base, 4, L"BUTTON", L"打开", BS_OWNERDRAW, 1);
        MakeControl(base + 3, 4, L"BUTTON", L"打开目录", BS_OWNERDRAW);
    }
    SendMessageW(Item(SyncEdit), EM_SETLIMITTEXT, 4, 0);
    SendMessageW(Item(LogEdit), EM_SETLIMITTEXT, 2000000, 0);
    Fonts();
    initialized = true;
    UpdateControls(true);
    Layout();
}
void App::Fonts() {
    auto font = [this](int pixels, int weight, const wchar_t* family) -> HFONT {
        return CreateFontW(-D(pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, family);
    };
    // Full Windows CJK coverage prevents newly added labels from mixing subset glyphs with serif fallbacks.
    const wchar_t* family = L"Microsoft YaHei UI";
    normalFont.Reset(font(13, FW_NORMAL, family));
    smallFont.Reset(font(11, FW_NORMAL, family));
    titleFont.Reset(font(28, FW_BOLD, family));
    heroFont.Reset(font(26, FW_BOLD, family));
    metricFont.Reset(font(28, FW_BOLD, family));
    brandFont.Reset(font(16, FW_BOLD, family));
    iconFont.Reset(assets.HasIcons() ? font(17, FW_BLACK, assets.IconFamily()) : nullptr);
    iconRenderDiagnostic = L"图标 GDI 字形：font=" + std::wstring(iconFont.handle ? L"created" : L"none");
    if (iconFont.handle) {
        HDC probe = GetDC(hwnd);
        if (probe) {
            HGDIOBJ old = SelectObject(probe, iconFont.handle);
            wchar_t actualFace[LF_FACESIZE]{};
            GetTextFaceW(probe, LF_FACESIZE, actualFace);
            const wchar_t sample[] = {0xf132, 0xf009, 0xf0c7};
            WORD glyphs[3]{};
            DWORD result = GetGlyphIndicesW(probe, sample, 3, glyphs, GGI_MARK_NONEXISTING_GLYPHS);
            iconRenderDiagnostic += L" face=" + std::wstring(actualFace) + L" glyph-result=" +
                                    std::to_wstring(result) + L" shield/grid/save=" +
                                    std::to_wstring(glyphs[0]) + L"/" + std::to_wstring(glyphs[1]) + L"/" +
                                    std::to_wstring(glyphs[2]);
            if (old && old != HGDI_ERROR)
                SelectObject(probe, old);
            ReleaseDC(hwnd, probe);
        }
    }
    if (!normalFont.handle || !titleFont.handle || !heroFont.handle || !metricFont.handle ||
        !brandFont.handle || !smallFont.handle)
        Notify(L"系统字体创建失败，将使用 Windows 默认字体。", true, false);
    for (const auto& c : controls)
        SendMessageW(c.hwnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(normalFont.handle ? normalFont.handle
                                                                : GetStockObject(DEFAULT_GUI_FONT)),
                     FALSE);
    if (licenseWindow)
        for (int id : {1, 2, 3})
            SendMessageW(GetDlgItem(licenseWindow, id), WM_SETFONT,
                         reinterpret_cast<WPARAM>(normalFont.handle), FALSE);
    ApplyTheme();
}
void App::ApplyNativeCaption(HWND window) const {
    using DwmFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    bool ownDwm = false;
    if (!dwm) {
        dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ownDwm = dwm != nullptr;
    }
    auto attribute = dwm ? reinterpret_cast<DwmFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    BOOL dark = light ? FALSE : TRUE;
    if (attribute && FAILED(attribute(window, 20, &dark, sizeof(dark))))
        attribute(window, 19, &dark, sizeof(dark));
    if (ownDwm)
        FreeLibrary(dwm);
}
void App::ApplyTheme() {
    palette = Colors(light);
    fieldBrush.Reset(CreateSolidBrush(palette.field));
    sideBrush.Reset(CreateSolidBrush(palette.card));
    if (!fieldBrush.handle || !sideBrush.handle)
        Notify(L"无法创建界面画刷，请检查系统资源。", true, false);
    BOOL dark = !light;
    // Purely optional caption enhancement: no newer DWM entry point is imported.
    using DwmFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    bool ownDwm = false;
    if (!dwm) {
        dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ownDwm = dwm != nullptr;
    }
    auto attribute = dwm ? reinterpret_cast<DwmFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    if (attribute && FAILED(attribute(hwnd, 20, &dark, sizeof(dark))))
        attribute(hwnd, 19, &dark, sizeof(dark));
    if (ownDwm)
        FreeLibrary(dwm);
    SetWindowTextW(Item(ThemeQuick), light ? L"深色外观" : L"浅色外观");
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (licenseWindow) {
        ApplyNativeCaption(licenseWindow);
        RedrawWindow(licenseWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}
int App::PageBottom() const {
    return page == 0 ? 720 : page == 1 ? 728 : page == 3 ? 752
           : page == 4 ? 160 + VisibleToolCount * 232 : BodyBottom();
}
int App::MaxScroll() const {
    return std::max(0, PageBottom() - BodyBottom());
}
void App::ScrollTo(int value) {
    value = std::max(0, std::min(MaxScroll(), value));
    if (value == scrollY)
        return;
    scrollY = value;
    Layout(true);
}
void App::EnsureVisible(HWND child) {
    if (layingOut || GetParent(child) != hwnd)
        return;
    int id = GetDlgCtrlID(child);
    if (!IsBodyControl(id))
        return;
    RECT rect{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rect), 2);
    int top = MulDiv(rect.top, 96, dpi), bottom = MulDiv(rect.bottom, 96, dpi);
    if (top < BodyTop + 8)
        ScrollTo(scrollY + top - (BodyTop + 16));
    else if (bottom > BodyBottom() - 8)
        ScrollTo(scrollY + bottom - (BodyBottom() - 16));
}
void App::Layout(bool scrolling) {
    if (smoke)
        ++uiPerf.layouts;
    if (!initialized || layingOut)
        return;
    layingOut = true;
    RECT client{};
    GetClientRect(hwnd, &client);
    width = MulDiv(client.right, 96, dpi);
    height = MulDiv(client.bottom, 96, dpi);
    scrollY = std::max(0, std::min(MaxScroll(), scrollY));
    int x = Left(), cw = ContentWidth();
    struct ChildPlacement {
        HWND child;
        int x, y, w, h;
        RECT clip;
    };
    std::vector<ChildPlacement> placements;
    placements.reserve(controls.size());
    // Compute the final visible set first. Shared PickGame/OpenCache/Backup HWNDs must not be
    // hidden according to their creation page and re-shown on every wheel event (or lose focus).
    auto put = [this, &placements](int id, int x, int y, int w, int h, bool show = true) {
        HWND child = Item(id);
        if (!child || !uiReady || !show)
            return;
        const bool fixed = !IsBodyControl(id);
        if (!fixed)
            y -= scrollY;
        const int px = D(x), py = D(y), pw = D(w), ph = D(h);
        const RECT bounds{px, py, px + pw, py + ph};
        const RECT clip = fixed ? RECT{0, 0, pw, ph} : ChildViewportClip(bounds, BodyViewport());
        placements.push_back({child, px, py, pw, ph, clip});
    };
    auto caption = [this](int id, const wchar_t* text) {
        wchar_t current[64]{};
        HWND child = Item(id);
        GetWindowTextW(child, current, 64);
        if (wcscmp(current, text) != 0)
            SetWindowTextW(child, text);
    };
    for (int i = 0; i < PageCount; ++i)
        put(NavFirst + NavPageOrder[i], 16, 160 + i * 48, 192, 40);
    put(ThemeQuick, width - 292, 12, 104, 32);
    put(OpenManual, width - 432, 12, 128, 32);
    put(WinMin, width - 176, 10, 44, 36);
    put(WinMax, width - 132, 10, 44, 36);
    put(WinClose, width - 88, 10, 60, 36);
    put(About, 16, height - 104, 192, 32);
    put(OpenDataRoot, 16, height - 172, 192, 36);
    if (page == 0) {
        // Header HWNDs stay pinned above BodyTop, including at minimum window width.
        put(QuickAbc, width - 384, 88, 112, 32);
        put(QuickRose, x + cw - 232, 88, 112, 32);
        put(QuickAkari, x + cw - 112, 88, 112, 32);
        put(Backup, x + 32, 288, 144, 40, status.gameOk);
        put(PickGame, status.gameOk ? x + cw - 144 : x + 32, status.gameOk ? 568 : 288,
            status.gameOk ? 112 : 144, status.gameOk ? 32 : 40);
        caption(PickGame, status.gameOk ? L"选择目录…" : L"选择游戏目录");
        put(OpenCache, x + cw - 144, 640, 112, 32);
    } else if (page == 1) {
        int left = (cw - 16) * 3 / 5, right = cw - left - 16, rx = x + left + 16;
        put(Backup, x + 24, 384, 144, 40);
        put(Cancel, x + left - 128, 384, 104, 40);
        put(Auto, rx + 24, 328, right - 48, 40);
        put(GoSettings, rx + 24, 394, right - 48, 32);
        put(Restore, x + cw - 184, 504, 160, 40);
        put(Import, x + cw - 184, 640, 160, 40);
        put(OldCache, x + cw - 312, 640, 112, 40, !legacyCache.empty());
    } else if (page == 2) {
        put(RefreshLogs, x + 24, 176, 104, 32);
        put(OpenLogs, x + 140, 176, 152, 32);
        put(LogEdit, x + 24, 244, cw - 64, std::max(80, height - 324));
        RECT rect{0, 0, D(cw - 64), D(std::max(80, height - 324))};
        SendMessageW(Item(LogEdit), EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&rect));
    } else if (page == 3) {
        int pathWidth = cw - 224;
        caption(PickGame, L"选择…");
        put(GameEdit, x + 36, 242, pathWidth - 24, 22);
        put(CacheEdit, x + 36, 314, pathWidth - 24, 22);
        put(PickGame, x + cw - 184, 232, 88, 40);
        put(PickCache, x + cw - 184, 304, 88, 40);
        put(OpenGame, x + cw - 88, 232, 64, 40);
        put(OpenCache, x + cw - 88, 304, 64, 40);
        if (!bootstrapFailed) {
            put(SyncEdit, x + 36, 453, cw - 72, 22);
            int themeWidth = 176, closeWidth = cw - 392;
            put(ThemeCombo, x + 24, 584, themeWidth, 40);
            put(CloseCombo, x + 216, 584, closeWidth, 40);
            put(StartupCombo, x + cw - 160, 584, 136, 40);
        }
        put(Save, x + cw - 136, 88, 136, 40);
    } else if (page == 4) {
        for (int i = 0; i < VisibleToolCount; ++i) {
            const int base = ToolFirst + i * ToolStride, y = 160 + i * 232;
            put(base, x + 24, y + 176, 112, 32);
            put(base + 3, x + 148, y + 176, 112, 32);
        }
    }
    struct PositionChange {
        HWND child;
        int x, y, w, h;
        UINT flags;
    };
    std::vector<PositionChange> changes;
    changes.reserve(controls.size());
    constexpr UINT moveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
    for (const auto& control : controls) {
        const auto placement = std::find_if(placements.begin(), placements.end(),
                                            [&](const ChildPlacement& p) { return p.child == control.hwnd; });
        // Read WS_VISIBLE itself: IsWindowVisible is false while the first-frame parent is hidden.
        const bool wasVisible = (GetWindowLongPtrW(control.hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
        if (placement == placements.end()) {
            if (wasVisible)
                changes.push_back(
                    {control.hwnd, 0, 0, 0, 0, moveFlags | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW});
            continue;
        }
        const auto& p = *placement;
        UINT flags = moveFlags | (wasVisible ? 0u : SWP_SHOWWINDOW);
        RECT current{};
        if (GetWindowRect(p.child, &current)) {
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&current), 2);
            if (current.left == p.x && current.top == p.y)
                flags |= SWP_NOMOVE;
            if (current.right - current.left == p.w && current.bottom - current.top == p.h)
                flags |= SWP_NOSIZE;
        }
        if (!wasVisible || (flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
            changes.push_back({p.child, p.x, p.y, p.w, p.h, flags});
    }
    if (!changes.empty()) {
        HDWP batch = BeginDeferWindowPos(static_cast<int>(changes.size()));
        for (const auto& change : changes) {
            if (!batch)
                break;
            batch = DeferWindowPos(batch, change.child, nullptr, change.x, change.y, change.w, change.h,
                                   change.flags);
        }
        if (!batch || !EndDeferWindowPos(batch)) {
            // Allocation failure must not leave a half-laid-out page; this fallback uses the same
            // no-redraw/no-copy flags and the same single final parent/children repaint below.
            for (const auto& change : changes)
                SetWindowPos(change.child, nullptr, change.x, change.y, change.w, change.h, change.flags);
        }
    }
    // Commit geometry first, then its matching local region. Installing the future clip on
    // the old position temporarily exposes the wrong header/footer pixels during movement.
    for (const auto& p : placements) {
        HRGN region = CreateRectRgnIndirect(&p.clip);
        HRGN previous = CreateRectRgn(0, 0, 0, 0);
        const bool same =
            region && previous && GetWindowRgn(p.child, previous) != ERROR && EqualRgn(region, previous);
        if (region && (same || !SetWindowRgn(p.child, region, FALSE)))
            DeleteObject(region); // Successful SetWindowRgn transfers ownership to USER.
        if (previous)
            DeleteObject(previous);
    }
    EnableControls();
    // WS_CLIPCHILDREN deliberately excludes the native HWNDs from the parent's double buffer.
    // Commit their final themed frames before returning from a wheel/resize event, rather than
    // leaving default-erased button rectangles pending until the low-priority WM_PAINT queue.
    // Repair fixed heading/footer pixels too; body-only invalidation leaves old spill pixels.
    // Navigation/caption windows are outside this repaint and remain unmoved and unerased.
    const RECT dirty{D(224), D(56), client.right, client.bottom};
    // Fixed caption/navigation HWNDs were neither moved nor hidden.
    // Do not request RDW_ERASE (or cancel native EDIT erases with a global RDW_NOERASE).
    // ButtonProc handles the owner-draw erase contract without changing editable field behavior.
    RedrawWindow(hwnd, scrolling ? &dirty : nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    layingOut = false;
}
void App::SetPage(int value) {
    if (value < 0 || value >= PageCount || stopping)
        return;
    if (value != page && page == 3 && !GuardUnsaved(value, false))
        return;
    if (page != value) {
        static const wchar_t* names[] = {L"总览", L"备份任务", L"日志", L"设置", L"工具中心"};
        UiLog(L"UI_PAGE_CHANGED", L"页面切换：" + std::wstring(names[page]) + L" → " + names[value]);
        scrollY = 0;
    }
    page = value;
    Layout();
    if (page == 2)
        RequestLogs();
}
void App::Text(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT flags) {
    if (text.empty() || r.right <= r.left || r.bottom <= r.top || !RectVisible(dc, &r)) {
        if (smoke)
            ++uiPerf.textSkips;
        return;
    }
    if (smoke)
        ++uiPerf.textDraws;
    HGDIOBJ old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    if (old && old != HGDI_ERROR)
        SelectObject(dc, old);
}
void App::Round(HDC dc, RECT r, COLORREF fill, COLORREF line, int radius) {
    if (r.right <= r.left || r.bottom <= r.top || !RectVisible(dc, &r))
        return;
    Gdiplus::Graphics g(dc);
    // GDI+ does not inherit the HDC application/window region automatically.
    RECT nativeClip{};
    if (GetClipBox(dc, &nativeClip) != ERROR)
        g.SetClip(Gdiplus::Rect(nativeClip.left, nativeClip.top, nativeClip.right - nativeClip.left,
                                nativeClip.bottom - nativeClip.top),
                  Gdiplus::CombineModeIntersect);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    float x = (float)r.left + 0.5f, y = (float)r.top + 0.5f, w = (float)(r.right - r.left) - 1,
          h = (float)(r.bottom - r.top) - 1;
    float rr = (float)std::min(D(radius) * 2, std::min((int)w, (int)h));
    path.AddArc(x, y, rr, rr, 180, 90);
    path.AddArc(x + w - rr, y, rr, rr, 270, 90);
    path.AddArc(x + w - rr, y + h - rr, rr, rr, 0, 90);
    path.AddArc(x, y + h - rr, rr, rr, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(GColor(fill));
    Gdiplus::Pen pen(GColor(line), 1.0f);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
}
void App::Card(HDC dc, int x, int y, int w, int h, bool hero) {
    Round(dc, R(x, y, w, h), hero ? palette.hero : palette.card, hero ? palette.accentSoft : palette.border,
          14);
}
void App::Icon(HDC dc, wchar_t glyph, RECT rc, COLORREF color) {
    if (iconFont.handle)
        Text(dc, std::wstring(1, glyph), rc, (HFONT)iconFont.handle, color,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void App::NavIcon(HDC dc, int nav, RECT rc, COLORREF color) {
    const wchar_t glyphs[] = {0xf009, 0xf187, 0xf0f6, 0xf1de, 0xf0ad};
    rc.right = rc.left + D(20);
    rc.bottom = rc.top + D(20);
    Icon(dc, glyphs[std::clamp(nav, 0, PageCount - 1)], rc, color);
}
void App::Line(HDC dc, int x, int y, int w, COLORREF color) {
    RECT r = R(x, y, w, 1);
    HBRUSH brush = CreateSolidBrush(color);
    if (brush) {
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }
}
void App::InputFrame(HDC dc, int id, int x, int y, int w, int h) {
    bool focus = GetFocus() == Item(id);
    if (focus)
        Round(dc, R(x - 2, y - 2, w + 4, h + 4), palette.accentSoft, palette.accentSoft, 10);
    Round(dc, R(x, y, w, h), palette.field, focus ? palette.accent : palette.border, 8);
}
void App::Hero(HDC dc, int x, int y, int w, int h) {
    RECT rc = R(x, y, w, h);
    if (!RectVisible(dc, &rc))
        return;
    Round(dc, rc, palette.hero, RGB(65, 51, 91), 16);
    if (!assets.Hero())
        return; // No generated substitute artwork: a plain UI surface is the safe fallback.
    Gdiplus::Graphics g(dc);
    // GDI+ does not inherit the HDC application/window region automatically.
    RECT nativeClip{};
    if (GetClipBox(dc, &nativeClip) != ERROR)
        g.SetClip(Gdiplus::Rect(nativeClip.left, nativeClip.top, nativeClip.right - nativeClip.left,
                                nativeClip.bottom - nativeClip.top),
                  Gdiplus::CombineModeIntersect);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    Gdiplus::GraphicsPath clip;
    float xx = (float)rc.left + 1, yy = (float)rc.top + 1;
    float ww = (float)(rc.right - rc.left) - 2, hh = (float)(rc.bottom - rc.top) - 2, rr = (float)D(30);
    clip.AddArc(xx, yy, rr, rr, 180, 90);
    clip.AddArc(xx + ww - rr, yy, rr, rr, 270, 90);
    clip.AddArc(xx + ww - rr, yy + hh - rr, rr, rr, 0, 90);
    clip.AddArc(xx, yy + hh - rr, rr, rr, 90, 90);
    clip.CloseFigure();
    g.SetClip(&clip, Gdiplus::CombineModeIntersect);
    auto* image = assets.Hero();
    const int pixelWidth = std::max(1, static_cast<int>(ww)), pixelHeight = std::max(1, static_cast<int>(hh));
    if (!heroPixels || heroPixelWidth != pixelWidth || heroPixelHeight != pixelHeight) {
        auto pixels = std::make_unique<Gdiplus::Bitmap>(pixelWidth, pixelHeight, PixelFormat32bppPARGB);
        if (pixels->GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics cache(pixels.get());
            cache.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            cache.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            const float iw = static_cast<float>(image->GetWidth()),
                        ih = static_cast<float>(image->GetHeight());
            const float sourceHeight = iw * pixelHeight / pixelWidth,
                        sourceWidth = ih * pixelWidth / pixelHeight;
            const auto drawn =
                sourceHeight <= ih
                    ? cache.DrawImage(image, Gdiplus::Rect(0, 0, pixelWidth, pixelHeight), 0,
                                      (ih - sourceHeight) / 2, iw, sourceHeight, Gdiplus::UnitPixel)
                    : cache.DrawImage(image, Gdiplus::Rect(0, 0, pixelWidth, pixelHeight),
                                      (iw - sourceWidth) / 2, 0, sourceWidth, ih, Gdiplus::UnitPixel);
            cache.Flush(Gdiplus::FlushIntentionSync);
            if (drawn == Gdiplus::Ok) {
                heroPixels = std::move(pixels);
                heroPixelWidth = pixelWidth;
                heroPixelHeight = pixelHeight;
            }
        }
    }
    if (heroPixels && heroPixelWidth == pixelWidth && heroPixelHeight == pixelHeight) {
        // Crop/resample only when size or DPI changes, not on every hover/status repaint.
        g.DrawImage(heroPixels.get(), Gdiplus::RectF(xx, yy, ww, hh), 0.0f, 0.0f,
                    static_cast<float>(pixelWidth), static_cast<float>(pixelHeight), Gdiplus::UnitPixel);
    } else {
        g.DrawImage(image, Gdiplus::RectF(xx, yy, ww, hh));
    }
    Gdiplus::LinearGradientBrush veil(Gdiplus::PointF(xx, yy), Gdiplus::PointF(xx + ww, yy),
                                      Gdiplus::Color(255, 25, 20, 43), Gdiplus::Color(16, 25, 20, 43));
    Gdiplus::Color colors[] = {Gdiplus::Color(255, 25, 20, 43), Gdiplus::Color(244, 25, 20, 43),
                               Gdiplus::Color(102, 25, 20, 43), Gdiplus::Color(12, 25, 20, 43)};
    Gdiplus::REAL positions[] = {0.0f, 0.42f, 0.74f, 1.0f};
    veil.SetInterpolationColors(colors, positions, 4);
    g.FillRectangle(&veil, xx, yy, ww, hh);
}
void App::Paint(HDC dc) {
    RECT all{};
    GetClientRect(hwnd, &all);
    HBRUSH bg = CreateSolidBrush(palette.bg), side = CreateSolidBrush(palette.side);
    if (bg) {
        FillRect(dc, &all, bg);
        DeleteObject(bg);
    }
    RECT sidebar = R(0, 0, 224, height);
    if (side) {
        FillRect(dc, &sidebar, side);
        DeleteObject(side);
    }
    RECT separator = R(223, 0, 1, height);
    HBRUSH border = CreateSolidBrush(palette.border);
    if (border) {
        FillRect(dc, &separator, border);
        DeleteObject(border);
    }
    const RECT brand = R(20, 24, 44, 44);
    if (RectVisible(dc, &brand)) {
        if (smoke)
            ++uiPerf.logoDraws;
        if (Gdiplus::Image* logo = assets.Logo(!light)) {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(logo, Gdiplus::Rect(brand.left, brand.top, brand.right - brand.left,
                                            brand.bottom - brand.top));
        } else {
            Round(dc, brand, palette.accentSoft, palette.accentSoft, 10);
            Icon(dc, 0xf132, brand, palette.accent);
        }
    }
    Text(dc, L"BBS自动工具", R(72, 27, 148, 26), (HFONT)brandFont.handle, palette.text);
    Text(dc, L"QQ1121003655", R(72, 53, 148, 18), (HFONT)smallFont.handle, palette.muted);
    Text(dc, L"工作空间", R(32, 120, 160, 20), (HFONT)smallFont.handle, palette.muted);
    // No enclosing card: a quiet heading and a readable state pill in the free sidebar space.
    // Lobby presence remains independent of whether automatic backup is enabled.
    {
        const int freeTop = 160 + PageCount * 48;
        const int freeBottom = std::max(freeTop + 64, height - 200);
        const int indicatorY = (freeTop + freeBottom - 76) / 2;
        const bool preview = smoke && smokeLobbyPreview != -2;
        const bool known =
            preview ? smokeLobbyPreview >= 0 : serviceStarted && !stopping && status.monitorKnown;
        const bool online = known && (preview ? smokeLobbyPreview == 1 : status.lobbyOnline);
        const COLORREF heading = light ? RGB(93, 94, 112) : palette.muted;
        const COLORREF stateColor = online  ? (light ? RGB(21, 116, 72) : RGB(103, 225, 168))
                                    : known ? (light ? RGB(93, 98, 116) : RGB(171, 174, 193))
                                    : light ? RGB(140, 91, 10)
                                            : RGB(238, 186, 74);
        const COLORREF pillFill = online  ? (light ? RGB(222, 244, 232) : RGB(24, 53, 41))
                                  : known ? (light ? RGB(229, 231, 239) : RGB(36, 37, 45))
                                  : light ? RGB(250, 239, 214)
                                          : RGB(61, 48, 26);
        const COLORREF pillBorder = online  ? (light ? RGB(164, 211, 183) : RGB(45, 94, 67))
                                    : known ? (light ? RGB(204, 208, 222) : RGB(62, 65, 80))
                                    : light ? RGB(224, 195, 134)
                                            : RGB(116, 89, 38);
        Icon(dc, 0xf108, R(56, indicatorY + 2, 20, 20), heading);
        Text(dc, L"大厅状态", R(84, indicatorY, 108, 24), (HFONT)normalFont.handle, heading);
        const wchar_t* state = stopping          ? L"正在退出"
                               : !serviceStarted ? (bootstrapBusy ? L"连接中" : L"未启动")
                               : !known          ? L"检测中"
                               : online          ? L"在线"
                                                 : L"离线";
        const int pillWidth = stopping ? 112 : known ? 80 : 96;
        const RECT pill = R(56, indicatorY + 38, pillWidth, 34);
        Round(dc, pill, pillFill, pillBorder, 17);
        Text(dc, state, pill, (HFONT)brandFont.handle, stateColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    Text(dc, L"复制总目录文件夹可进行数据迁移", R(24, height - 134, 192, 22),
         (HFONT)smallFont.handle, palette.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Round(dc, R(16, height - 64, 192, 40), palette.card, palette.border, 9);
    Icon(dc, serviceStarted ? 0xf058 : 0xf017, R(28, height - 54, 20, 20),
         serviceStarted && !stopping ? palette.good : palette.muted);
    Text(dc,
         stopping         ? L"正在安全退出"
         : serviceStarted ? L"本地服务已连接"
         : bootstrapBusy  ? L"正在连接服务"
                          : L"服务未启动",
         R(56, height - 58, 140, 28), (HFONT)smallFont.handle, palette.text);
    Line(dc, 224, 55, width - 224, palette.border);
    Icon(dc, 0xf108, R(256, 18, 20, 20), palette.muted);
    Text(dc, L"本地工作空间", R(288, 12, 132, 32), (HFONT)smallFont.handle, palette.text);
    Text(dc, L"/", R(408, 12, 24, 32), (HFONT)smallFont.handle, palette.muted,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const wchar_t* titles[] = {L"旗舰版", L"备份任务", L"运行日志", L"设置", L"工具中心"};
    Text(dc, titles[page], R(440, 12, 140, 32), (HFONT)smallFont.handle, palette.muted);
    int x = Left(), cw = ContentWidth();
    Text(dc, titles[page], R(x, 80, cw - 160, 40), (HFONT)titleFont.handle, palette.text);
    const wchar_t* subtitles[] = {
        L"一眼掌握配置状态，让每一次开局都有备而来。", L"有序执行每一次备份，谨慎对待每一次恢复。",
        L"重要操作有迹可循，所有记录仅保存在这台电脑。", L"为你的使用习惯，安排恰到好处的守护。",
        L"Rose、League Akari 与 ABC；点击打开即可使用。"};
    Text(dc, subtitles[page], R(x, 126, cw, 22), (HFONT)normalFont.handle, palette.muted);
    const int saved = SaveDC(dc);
    const RECT body = BodyViewport();
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    // Explicit device-coordinate offset works for both GDI text and GDI+ paths on every supported OS.
    paintOffset = scrollY;
    bool busy = Busy();
    // Render the current service operation and lobby scheduling state.
    const std::wstring autoTitle = [&]() -> const wchar_t* {
        if (status.autoState == L"active")
            return L"已调度";
        if (status.autoState == L"restore_failed")
            return L"还原未完成";
        if (status.autoState == L"restore_in_progress")
            return L"正在还原 Game";
        if (status.autoState == L"restore_pending")
            return L"启动还原待执行";
        if (status.autoState == L"offline")
            return L"等待大厅";
        if (status.autoState == L"unknown_lobby")
            return L"检测中";
        if (status.autoState == L"missing_game")
            return L"未选Game";
        if (status.autoState == L"disabled")
            return L"联动关闭";
        if (status.autoState == L"scheduling_disabled")
            return L"调度未启用";
        if (status.autoState == L"busy")
            return L"任务执行中";
        if (status.autoState == L"stopped")
            return L"服务停止";
        return L"等待服务状态";
    }();
    const bool autoReady = status.periodicReady;
    const std::wstring autoReason =
        status.autoReason.empty() ? L"自动计划以服务实际状态为准。" : status.autoReason;
    if (page == 0) {
        Hero(dc, x, 160, cw, 192);
        Icon(dc, 0xf132, R(x + 32, 183, 16, 20), RGB(190, 170, 255));
        Text(dc,
             stopping               ? L"正在安全结束任务"
             : busy                 ? L"配置守护引擎 · 正在处理"
             : status.periodicReady ? L"在线镜像已调度 · 每 " + std::to_wstring(status.syncMinutes) + L" 分钟"
                                    : L"自动联动 · " + autoTitle,
             R(x + 56, 182, cw - 88, 22), (HFONT)smallFont.handle, RGB(206, 192, 242));
        Text(dc,
             stopping ? L"正在安全结束任务"
             : busy   ? (status.autoState == L"restore_in_progress" ? L"正在从缓存还原 Game"
                                                                    : L"正在同步 Game 到缓存")
                      : autoTitle,
             R(x + 32, 210, cw - 64, 36), (HFONT)heroFont.handle, RGB(250, 247, 255));
        // Two bounded lines end above the existing action buttons at y=288.
        Text(dc, autoReason, R(x + 32, 248, cw - 64, 36), (HFONT)normalFont.handle, RGB(183, 174, 205),
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        Text(dc, L"LOCAL FIRST / ALWAYS YOURS", R(x + 196, 298, cw - 228, 20), (HFONT)smallFont.handle,
             RGB(169, 156, 195));
        int metric = (cw - 32) / 3;
        const wchar_t* captions[] = {L"自动调度", L"缓存条目", L"最近同步"};
        const wchar_t glyphs[] = {0xf021, 0xf187, 0xf017};
        const std::wstring notes[] = {status.periodicReady ? L"下次镜像：" + status.nextSync
                                                           : L"当前等待原因见「备份任务」",
                                      L"当前本地缓存中的文件", L"以服务完成记录为准"};
        std::wstring values[] = {autoTitle, std::to_wstring(status.cacheEntries),
                                 status.lastSync == L"--" ? L"暂无同步" : status.lastSync};
        for (int i = 0; i < 3; ++i) {
            int mx = x + i * (metric + 16), mw = i == 2 ? x + cw - mx : metric;
            Card(dc, mx, 376, mw, 112);
            Text(dc, captions[i], R(mx + 20, 390, mw - 68, 20), (HFONT)smallFont.handle, palette.muted);
            Icon(dc, glyphs[i], R(mx + mw - 40, 392, 20, 20), palette.muted);
            Text(dc, values[i], R(mx + 20, 414, mw - 40, 36),
                 (HFONT)(i == 1 ? metricFont.handle : brandFont.handle),
                 i == 0 && autoReady ? palette.good : palette.text);
            Text(dc, notes[i], R(mx + 20, 456, mw - 40, 18), (HFONT)smallFont.handle, palette.muted);
        }
        Card(dc, x, 512, cw, 184);
        Text(dc, L"存储位置", R(x + 24, 528, cw - 48, 22), (HFONT)brandFont.handle, palette.text);
        Icon(dc, 0xf11b, R(x + 24, 574, 24, 24), palette.muted);
        Text(dc, L"游戏目录", R(x + 64, 556, cw - 240, 20), (HFONT)smallFont.handle, palette.muted);
        Text(dc, status.gameRoot.empty() ? L"尚未配置 · 请选择外服 LOL 的 Game 文件夹" : status.gameRoot,
             R(x + 64, 580, cw - 240, 24), (HFONT)normalFont.handle, palette.text);
        Line(dc, x + 24, 616, cw - 48, palette.border);
        Icon(dc, 0xf0a0, R(x + 24, 646, 24, 24), palette.muted);
        Text(dc, L"本地缓存", R(x + 64, 628, cw - 240, 20), (HFONT)smallFont.handle, palette.muted);
        Text(dc, status.cacheRoot.empty() ? L"尚未配置 · 请选择缓存文件夹" : status.cacheRoot, R(x + 64, 652, cw - 240, 24),
             (HFONT)normalFont.handle, palette.text);
    } else if (page == 1) {
        int left = (cw - 16) * 3 / 5, right = cw - left - 16, rx = x + left + 16;
        Card(dc, x, 160, left, 288);
        Text(dc, L"执行状态", R(x + 24, 180, left - 152, 28), (HFONT)brandFont.handle, palette.text);
        Round(dc, R(x + left - 100, 180, 76, 24), busy ? palette.accentSoft : palette.field, palette.border,
              12);
        std::wstring badge =
            busy ? (status.backupState == L"running" && status.progressPercent >= 0
                        ? std::to_wstring(std::max(0, std::min(100, status.progressPercent))) + L"%"
                        : L"处理中")
                 : L"待命中";
        Text(dc, badge, R(x + left - 100, 180, 76, 24), (HFONT)smallFont.handle,
             busy ? palette.accent : palette.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const wchar_t* steps[] = {L"目录检查", L"文件任务", L"最终回执"};
        std::wstring details[] = {
            status.gameOk ? L"游戏目录已配置" : L"请先在设置中选择游戏目录",
            busy ? (status.progressLabel.empty() ? L"任务正在后台处理" : status.progressLabel)
                 : L"等待手动操作或自动计划",
            busy ? L"完成状态以服务最终响应为准" : L"所有操作均等待真实服务响应"};
        for (int i = 0; i < 3; ++i) {
            int yy = 226 + i * 48;
            if (i < 2) {
                RECT connector = R(x + 33, yy + 24, 1, 25);
                HBRUSH brush = CreateSolidBrush(palette.border);
                if (brush) {
                    FillRect(dc, &connector, brush);
                    DeleteObject(brush);
                }
            }
            Icon(dc,
                 i == 0 && status.gameOk ? 0xf058
                 : i == 1 && busy        ? 0xf021
                                         : 0xf111,
                 R(x + 24, yy, 20, 20),
                 i == 0 && status.gameOk ? palette.good
                 : i == 1 && busy        ? palette.accent
                                         : palette.muted);
            Text(dc, steps[i], R(x + 56, yy - 2, left - 80, 22), (HFONT)normalFont.handle, palette.text);
            Text(dc, details[i], R(x + 56, yy + 20, left - 80, 20), (HFONT)smallFont.handle, palette.muted);
        }
        int percent = std::max(0, std::min(100, status.progressPercent));
        Round(dc, R(x + 24, 370, left - 48, 3), palette.field, palette.field, 1);
        if (status.backupState == L"running" && percent > 0)
            Round(dc, R(x + 24, 370, std::max(4, (left - 48) * percent / 100), 3), palette.accent,
                  palette.accent, 1);
        Card(dc, rx, 160, right, 288);
        Icon(dc, 0xf021, R(rx + 24, 184, 20, 20), palette.accent);
        Text(dc, L"大厅联动", R(rx + 56, 180, right - 80, 28), (HFONT)brandFont.handle, palette.text);
        Text(dc, autoTitle, R(rx + 24, 214, right - 48, 24), (HFONT)brandFont.handle,
             autoReady ? palette.good : palette.text);
        // Reserve separate bounded regions for long Chinese reasons and the next-run state.
        Text(dc, autoReason, R(rx + 24, 244, right - 48, 48), (HFONT)smallFont.handle, palette.muted,
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        Text(dc, L"下次镜像：" + status.nextSync, R(rx + 24, 294, right - 48, 30), (HFONT)smallFont.handle,
             status.periodicReady ? palette.good : palette.muted, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        COLORREF dangerSurface = light ? RGB(255, 249, 250) : RGB(32, 24, 30);
        Round(dc, R(x, 472, cw, 112), dangerSurface, light ? RGB(238, 213, 220) : RGB(75, 42, 53), 14);
        Icon(dc, 0xf071, R(x + 24, 496, 24, 24), palette.danger);
        Text(dc, L"恢复到游戏 · 谨慎操作", R(x + 64, 492, cw - 288, 28), (HFONT)brandFont.handle,
             palette.danger);
        std::wstring warning =
            L"直接覆盖 Game 中的受管文件，不等待对局、不做哈希校验；失败可能留下部分文件。";
        Text(dc, warning, R(x + 64, 528, cw - 288, 40), (HFONT)smallFont.handle, palette.muted,
             DT_LEFT | DT_WORDBREAK);
        Card(dc, x, 608, cw, 96);
        Icon(dc, 0xf090, R(x + 24, 632, 24, 24), palette.muted);
        Text(dc, L"已有旧版缓存？", R(x + 64, 628, cw - 392, 24), (HFONT)brandFont.handle, palette.text);
        Text(dc, L"手动选择，安全导入；不自动覆盖旧目录。", R(x + 64, 660, cw - 392, 28),
             (HFONT)smallFont.handle, palette.muted, DT_LEFT | DT_WORDBREAK);
    } else if (page == 2) {
        Card(dc, x, 160, cw, std::max(160, height - 224));
        Text(dc, L"最近 200 条 · 已合并内部诊断", R(x + cw - 304, 176, 280, 32), (HFONT)smallFont.handle,
             palette.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        Line(dc, x + 24, 224, cw - 48, palette.border);
        Round(dc, R(x + 16, 232, cw - 32, std::max(96, height - 312)), palette.field, palette.field, 8);
        int total = (int)SendMessageW(Item(LogEdit), EM_GETLINECOUNT, 0, 0);
        int first = (int)SendMessageW(Item(LogEdit), EM_GETFIRSTVISIBLELINE, 0, 0);
        int track = std::max(48, height - 344), visible = std::max(1, (height - 324) / 20);
        int thumb = std::min(track, std::max(32, track * visible / std::max(visible, total)));
        int offset =
            total > visible ? (track - thumb) * std::min(first, total - visible) / (total - visible) : 0;
        Round(dc, R(x + cw - 29, 248, 3, track), palette.field, palette.field, 1);
        Round(dc, R(x + cw - 29, 248 + offset, 3, thumb), palette.border, palette.border, 1);
    } else if (page == 4) {
        PaintTools(dc, x, cw);
    } else if (page == 3) {
        Card(dc, x, 160, cw, 200);
        Icon(dc, 0xf07b, R(x + 24, 177, 20, 24), palette.accent);
        Text(dc, L"目录与存储", R(x + 56, 176, cw - 80, 26), (HFONT)brandFont.handle, palette.text);
        int pathWidth = cw - 224;
        const wchar_t* labels[] = {status.gameRoot.empty() ? L"游戏目录 · 请选择外服 Game 文件夹" : L"游戏目录",
                                   status.cacheRoot.empty() ? L"缓存目录 · 请选择备份存储文件夹" : L"缓存目录"};
        const int ids[] = {GameEdit, CacheEdit};
        for (int i = 0; i < 2; ++i) {
            Text(dc, labels[i], R(x + 24, 208 + i * 72, cw - 48, 18), (HFONT)smallFont.handle, palette.muted);
            InputFrame(dc, ids[i], x + 24, 232 + i * 72, pathWidth);
        }
        if (bootstrapFailed) {
            Card(dc, x, 448, cw, 192);
            Text(dc, L"服务未启动 · 可在此修复目录", R(x + 24, 468, cw - 48, 28), (HFONT)brandFont.handle,
                 palette.text);
            Text(dc,
                 bootstrapConfigValid
                     ? L"以上为安全读取的实际配置。可重新选择目录，或接回原磁盘后按 F5 / "
                       L"右上角“重试启动”。\r\n\r\n"
                       L"目录选择将先验证隔离、授权与独占使用，再只更新所选路径；不会搬移旧文件。"
                     : L"配置未能安全读取，目录修改已禁用，原文件不会被默认值覆盖。\r\n\r\n"
                       L"请检查配置、目录权限或从备份恢复后，按 F5 / “重试启动”重新读取。",
                 R(x + 24, 510, cw - 48, 108), (HFONT)normalFont.handle, palette.muted,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        } else {
            Card(dc, x, 376, cw, 120);
            Icon(dc, 0xf017, R(x + 24, 393, 20, 24), palette.accent);
            Text(dc, L"备份节奏", R(x + 56, 392, cw - 80, 26), (HFONT)brandFont.handle, palette.text);
            Text(dc, L"同步间隔 / 分钟 · 1–1440（大厅在线时生效）", R(x + 24, 424, cw - 48, 18),
                 (HFONT)smallFont.handle, palette.muted);
            InputFrame(dc, SyncEdit, x + 24, 448, cw - 48, 32);
            Card(dc, x, 512, cw, 128);
            Icon(dc, 0xf1de, R(x + 24, 529, 20, 24), palette.accent);
            Text(dc, L"桌面偏好", R(x + 56, 528, cw - 80, 26), (HFONT)brandFont.handle, palette.text);
            Text(dc, L"界面主题", R(x + 24, 560, 176, 20), (HFONT)smallFont.handle, palette.muted);
            Text(dc, L"关闭窗口时（含 Alt+F4）", R(x + 216, 560, cw - 392, 20), (HFONT)smallFont.handle,
                 palette.muted);
            Text(dc, L"开机启动", R(x + cw - 160, 560, 136, 20), (HFONT)smallFont.handle, palette.muted);
            const bool unsaved = initialized && serviceStarted && !stopping && SettingsEdited();
            Text(dc,
                 smoke     ? L"隔离预览：监控、计划与系统自启动均已禁用。"
                 : unsaved ? L"有未保存的修改：点右上角「保存修改」或按 Ctrl+S 才会生效。"
                           : L"大厅联动：启动还原 · 在线每 " + std::to_wstring(status.syncMinutes) +
                                 L" 分钟全量覆盖 · 下线仅停止同步；其他偏好修改后请点右上角保存。",
                 R(x, 664, cw, 24), (HFONT)smallFont.handle, unsaved ? palette.accent : palette.muted);
        }
    }
    paintOffset = 0;
    if (saved)
        RestoreDC(dc, saved);
    if (MaxScroll() > 0) {
        int track = std::max(32, height - 224), visible = height - 208;
        int thumb = std::max(32, track * visible / std::max(1, PageBottom() - 160));
        thumb = std::min(thumb, track);
        int offset = (track - thumb) * scrollY / MaxScroll();
        Round(dc, R(width - 11, 168 + offset, 3, thumb), palette.border, palette.border, 1);
    }
    Line(dc, 224, height - 40, width - 224, palette.border);
    std::wstring message;
    if (stopping)
        message = shutdownWarned ? L"正在等待文件任务安全结束，窗口仍可响应；不会强制中断写入。"
                                 : L"正在取消任务并安全退出…";
    else if (!notice.empty() && GetTickCount64() < noticeUntil)
        message = notice;
    else if (!lastError.empty())
        message = lastError;
    else if (!legacyCache.empty())
        message = L"发现旧桌面缓存，可在「备份任务」中手动导入。";
    else
        message = serviceStarted  ? L"本地服务运行中 · Ctrl+1–5 切换页面 · F5 刷新"
                  : bootstrapBusy ? L"正在启动本地服务…"
                                  : L"服务未启动 · 在设置修复目录或按 F5 重试";
    Icon(dc, errorNotice ? 0xf06a : 0xf132, R(x, height - 29, 16, 18),
         errorNotice ? palette.danger : palette.muted);
    Text(dc, message, R(x + 28, height - 36, cw - 184, 32), (HFONT)smallFont.handle,
         errorNotice && !stopping ? palette.danger : palette.muted,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    static const std::wstring versionLabel = ApplicationVersionLabel();
    Text(dc, versionLabel, R(x + cw - 128, height - 36, 128, 32),
         (HFONT)smallFont.handle, palette.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
void App::PrintClient(HDC dc) {
    Paint(dc);
    const RECT body = BodyViewport();
    // Compose each child exactly once. WM_PRINT does not reliably honor SetWindowRgn,
    // so intersect destinations explicitly instead of relying on a native print clip.
    for (const auto& control : controls) {
        if (!IsWindowVisible(control.hwnd))
            continue;
        RECT rect{};
        if (!GetWindowRect(control.hwnd, &rect))
            continue;
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rect), 2);
        RECT visible = rect;
        const bool scrolling = IsBodyControl(control.id);
        if (scrolling && !IntersectRect(&visible, &rect, &body))
            continue;
        Buffer child(dc, rect.right - rect.left, rect.bottom - rect.top);
        if (!child.dc)
            continue;
        RECT local{0, 0, rect.right - rect.left, rect.bottom - rect.top};
        COLORREF background = control.tone == 3   ? palette.side
                              : control.id >= 300 ? palette.field
                                                  : palette.card;
        HBRUSH brush = CreateSolidBrush(background);
        if (brush) {
            FillRect(child.dc, &local, brush);
            DeleteObject(brush);
        }
        SendMessageW(control.hwnd, WM_PRINT, reinterpret_cast<WPARAM>(child.dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        const int saved = SaveDC(dc);
        if (scrolling)
            IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
        BitBlt(dc, visible.left, visible.top, visible.right - visible.left, visible.bottom - visible.top,
               child.dc, visible.left - rect.left, visible.top - rect.top, SRCCOPY);
        if (saved)
            RestoreDC(dc, saved);
    }
}
void App::DrawSegments(DRAWITEMSTRUCT* draw) {
    const int id = (int)draw->CtlID;
    const bool enabled = !(draw->itemState & ODS_DISABLED), focus = (draw->itemState & ODS_FOCUS) != 0;
    const int selection = (int)SendMessageW(draw->hwndItem, CB_GETCURSEL, 0, 0);
    const RECT rc = draw->rcItem;
    HBRUSH brush = CreateSolidBrush(palette.card);
    if (brush) {
        FillRect(draw->hDC, &rc, brush);
        DeleteObject(brush);
    }
    Round(draw->hDC, rc, palette.field, focus ? palette.accent : palette.border, 8);
    if (id == StartupCombo) {
        RECT label = rc;
        label.left += D(12);
        label.right -= D(56);
        Text(draw->hDC, selection ? L"已开启" : L"已关闭", label, (HFONT)smallFont.handle,
             enabled ? palette.text : palette.muted);
        RECT track{rc.right - D(52), rc.top + D(9), rc.right - D(12), rc.top + D(31)};
        COLORREF fill = selection && enabled ? palette.accent : palette.border;
        Round(draw->hDC, track, fill, fill, 11);
        int left = track.left + D(selection ? 21 : 3);
        RECT thumb{left, track.top + D(3), left + D(16), track.top + D(19)};
        Round(draw->hDC, thumb, enabled ? RGB(255, 255, 255) : palette.muted,
              enabled ? RGB(255, 255, 255) : palette.muted, 8);
        return;
    }
    const wchar_t* theme[] = {L"深色", L"浅色"};
    const wchar_t* close[] = {L"询问", L"托盘", L"退出"};
    int count = id == CloseCombo ? 3 : 2, inner = rc.right - rc.left - D(8);
    for (int i = 0; i < count; ++i) {
        RECT part{rc.left + D(4) + inner * i / count, rc.top + D(4), rc.left + D(4) + inner * (i + 1) / count,
                  rc.bottom - D(4)};
        if (i == selection)
            Round(draw->hDC, part, enabled ? palette.accentSoft : palette.card,
                  enabled ? palette.accentSoft : palette.border, 6);
        Text(draw->hDC, id == ThemeCombo ? theme[i] : close[i], part, (HFONT)normalFont.handle,
             enabled && i == selection ? palette.accent : palette.muted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
void App::DrawButton(DRAWITEMSTRUCT* draw) {
    if (!draw || !draw->hDC)
        return;
    struct DrawClip {
        HDC dc;
        int saved;
        ~DrawClip() {
            if (saved)
                RestoreDC(dc, saved);
        }
    } clip{draw->hDC, 0};
    if (IsBodyControl(static_cast<int>(draw->CtlID))) {
        clip.saved = SaveDC(draw->hDC);
        if (!clip.saved)
            return; // Never render an unclipped scrolling control on GDI resource failure.
        RECT viewport = BodyViewport();
        MapWindowPoints(hwnd, draw->hwndItem, reinterpret_cast<POINT*>(&viewport), 2);
        IntersectClipRect(draw->hDC, viewport.left, viewport.top, viewport.right, viewport.bottom);
    }
    const RECT rect = draw->rcItem;
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0)
        return;
    Buffer frame(draw->hDC, w, h);
    if (!frame.dc) {
        PaintButton(draw);
        return;
    }
    DRAWITEMSTRUCT buffered = *draw;
    buffered.hDC = frame.dc;
    buffered.rcItem = {0, 0, w, h};
    PaintButton(&buffered); // background, rounded surface, label and focus state are one frame
    // The native destination DC retains its window/update region, including partial body clipping.
    BitBlt(draw->hDC, rect.left, rect.top, w, h, frame.dc, 0, 0, SRCCOPY);
}
void App::PaintButton(DRAWITEMSTRUCT* draw) {
    if ((int)draw->CtlID >= ThemeCombo && (int)draw->CtlID <= StartupCombo) {
        DrawSegments(draw);
        return;
    }
    const Control* control = nullptr;
    for (const auto& c : controls)
        if (c.id == (int)draw->CtlID) {
            control = &c;
            break;
        }
    if (!control)
        return;
    bool enabled = !(draw->itemState & ODS_DISABLED), pressed = (draw->itemState & ODS_SELECTED) != 0;
    bool hover = GetPropW(draw->hwndItem, L"LOL.Hover") != nullptr;
    bool focus = (draw->itemState & ODS_FOCUS) && !(draw->itemState & ODS_NOFOCUSRECT);
    if (control->id == QuickRose || control->id == QuickAkari || control->id == QuickAbc) {
        // Quiet inline actions, not dashboard cards. Match the header exactly at rest.
        HBRUSH background = CreateSolidBrush(palette.bg);
        if (background) {
            FillRect(draw->hDC, &draw->rcItem, background);
            DeleteObject(background);
        }
        const bool hot = enabled && (hover || pressed);
        if (hot || focus)
            Round(draw->hDC, draw->rcItem, hot ? palette.accentSoft : palette.bg,
                  focus ? palette.accent : palette.accentSoft, 7);
        const bool rose = control->id == QuickRose, abc = control->id == QuickAbc;
        const COLORREF ink = enabled ? palette.text : palette.muted;
        const COLORREF tint = !enabled ? palette.muted
                              : rose   ? (light ? RGB(170, 76, 113) : RGB(224, 146, 177))
                              : abc    ? (light ? RGB(0, 124, 145) : RGB(91, 207, 218))
                                       : palette.accent;
        {
            Gdiplus::Graphics g(draw->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            // Work in logical pixels so the compact line icons remain crisp at each DPI.
            g.TranslateTransform((Gdiplus::REAL)draw->rcItem.left, (Gdiplus::REAL)draw->rcItem.top);
            g.ScaleTransform(dpi / 96.0f, dpi / 96.0f);
            auto color = [](COLORREF c) {
                return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
            };
            Gdiplus::Pen pen(color(tint), 1.35f);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            if (rose) {
                const Gdiplus::PointF petals[] = {{18, 10}, {22, 13}, {20.5f, 18}, {15.5f, 18}, {14, 13}};
                for (const auto& p : petals)
                    g.DrawEllipse(&pen, p.X - 3, p.Y - 3, 6.0f, 6.0f);
                g.DrawLine(&pen, 18.0f, 19.0f, 18.0f, 24.0f);
                g.DrawLine(&pen, 18.0f, 23.0f, 22.0f, 21.0f);
            } else if (abc) {
                // Native vector A: distinct from Rose petals and Akari star, no raster asset.
                const Gdiplus::PointF a[] = {{10, 24}, {18, 8}, {26, 24}};
                g.DrawLines(&pen, a, 3);
                g.DrawLine(&pen, 13.5f, 18.0f, 22.5f, 18.0f);
            } else {
                const Gdiplus::PointF star[] = {{18, 7},  {20.5f, 13.5f}, {27, 16}, {20.5f, 18.5f},
                                                {18, 25}, {15.5f, 18.5f}, {9, 16},  {15.5f, 13.5f}};
                g.DrawPolygon(&pen, star, 8);
                g.DrawLine(&pen, 26.0f, 7.0f, 26.0f, 11.0f);
                g.DrawLine(&pen, 24.0f, 9.0f, 28.0f, 9.0f);
            }
            Gdiplus::Pen arrow(color(enabled && hot ? palette.accent : palette.muted), 1.2f);
            arrow.SetStartCap(Gdiplus::LineCapRound);
            arrow.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&arrow, 91.0f, 19.0f, 98.0f, 12.0f);
            g.DrawLine(&arrow, 92.0f, 12.0f, 98.0f, 12.0f);
            g.DrawLine(&arrow, 98.0f, 12.0f, 98.0f, 18.0f);
        }
        RECT label = draw->rcItem;
        label.left += D(36);
        label.right -= D(24);
        Text(draw->hDC, rose ? L"Rose" : abc ? L"ABC" : L"Akari", label, (HFONT)normalFont.handle, ink);
        return;
    }
    if (control->id >= WinMin && control->id <= WinClose) {
        const bool hot = enabled && (hover || pressed);
        const COLORREF bg = control->id == WinClose && hot ? RGB(196, 43, 61)
                            : hot                          ? palette.accentSoft
                                                           : palette.bg;
        HBRUSH windowBrush = CreateSolidBrush(bg);
        if (windowBrush) {
            FillRect(draw->hDC, &draw->rcItem, windowBrush);
            DeleteObject(windowBrush);
        }
        const wchar_t glyph = control->id == WinMin   ? 0xf068
                              : control->id == WinMax ? (IsZoomed(hwnd) ? 0xf2d2 : 0xf2d0)
                                                      : 0xf00d;
        Icon(draw->hDC, glyph, draw->rcItem,
             control->id == WinClose && hot
                 ? RGB(255, 255, 255)
                 : (control->id == WinMax && IsZoomed(hwnd) ? palette.accent : palette.muted));
        return;
    }
    bool nav = control->tone == 3, active = nav && control->id == NavFirst + page;
    bool primary = control->tone == 1 || (control->id == PickGame && page == 0 && !status.gameOk);
    COLORREF fill = nav ? (active ? palette.accentSoft : palette.side) : palette.card;
    COLORREF text = enabled ? palette.text : palette.muted, line = nav ? fill : palette.border;
    if (control->id == About || control->id == OpenDataRoot) {
        fill = palette.side;
        line = fill;
        text = control->id == OpenDataRoot && enabled ? palette.text : palette.muted;
    }
    if (primary && enabled) {
        fill = pressed ? RGB(111, 78, 207) : hover ? RGB(151, 119, 246) : RGB(132, 94, 230);
        text = RGB(255, 255, 255);
        line = fill;
    }
    if (control->tone == 2) {
        fill = light ? RGB(255, 249, 250) : RGB(32, 24, 30);
        line = light ? RGB(238, 213, 220) : RGB(88, 46, 59);
        if (enabled)
            text = palette.danger;
    }
    if (active)
        text = palette.accent;
    if (enabled && (hover || pressed) && !primary) {
        fill = palette.accentSoft;
        line = nav ? fill : palette.border;
    }
    if (focus)
        line = palette.accent;
    COLORREF parent = nav || control->id == About || control->id == OpenDataRoot                        ? palette.side
                      : control->id == ThemeQuick || control->id == Save || control->id == OpenManual ? palette.bg
                      : (control->id == Backup || control->id == PickGame) && page == 0 &&
                              (control->id == Backup || !status.gameOk)
                          ? palette.hero
                      : control->id == Restore ? (light ? RGB(255, 249, 250) : RGB(32, 24, 30))
                                               : palette.card;
    HBRUSH brush = CreateSolidBrush(parent);
    if (brush) {
        FillRect(draw->hDC, &draw->rcItem, brush);
        DeleteObject(brush);
    }
    Round(draw->hDC, draw->rcItem, fill, line, 8);
    wchar_t caption[256]{};
    GetWindowTextW(draw->hwndItem, caption, 256);
    RECT textRect = draw->rcItem;
    if (nav) {
        RECT icon = textRect;
        icon.left += D(16);
        icon.top += D(10);
        NavIcon(draw->hDC, control->id - NavFirst, icon, text);
        textRect.left += D(52);
        textRect.right -= D(36);
        Text(draw->hDC, caption, textRect, (HFONT)normalFont.handle, text);
        Text(draw->hDC, std::to_wstring(NavPosition(control->id - NavFirst) + 1),
             RECT{draw->rcItem.right - D(32), D(4), draw->rcItem.right - D(12), draw->rcItem.bottom - D(4)},
             (HFONT)smallFont.handle, active ? palette.accent : palette.muted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (control->id == Auto) {
        textRect.left += D(16);
        textRect.right -= D(64);
        Text(draw->hDC, L"大厅联动", textRect, (HFONT)normalFont.handle, text);
        RECT track{draw->rcItem.right - D(56), D(9), draw->rcItem.right - D(16), D(31)};
        COLORREF trackFill = status.autoOn && enabled ? palette.accent : palette.border;
        Round(draw->hDC, track, trackFill, trackFill, 11);
        int left = track.left + D(status.autoOn ? 21 : 3);
        RECT thumb{left, D(12), left + D(16), D(28)};
        Round(draw->hDC, thumb, enabled ? RGB(255, 255, 255) : palette.muted,
              enabled ? RGB(255, 255, 255) : palette.muted, 8);
        return;
    }
    wchar_t glyph = 0;
    switch (control->id) {
    case Backup:
        glyph = 0xf0c7;
        break;
    case Restore:
        glyph = 0xf112;
        break;
    case Cancel:
        glyph = 0xf00d;
        break;
    case Import:
        glyph = 0xf090;
        break;
    case Save:
        glyph = 0xf00c;
        break;
    case RefreshLogs:
        glyph = 0xf021;
        break;
    case ThemeQuick:
        glyph = light ? 0xf186 : 0xf185;
        break;
    case OpenDataRoot:
        glyph = 0xf07c;
        break;
    case OpenManual:
    case About:
        glyph = 0xf05a;
        break;
    default:
        break;
    }
    if (glyph && iconFont.handle) {
        HGDIOBJ old = SelectObject(draw->hDC, normalFont.handle);
        SIZE size{};
        GetTextExtentPoint32W(draw->hDC, caption, lstrlenW(caption), &size);
        if (old && old != HGDI_ERROR)
            SelectObject(draw->hDC, old);
        int total = std::min((int)(draw->rcItem.right - draw->rcItem.left) - D(16), (int)size.cx + D(28));
        int start = draw->rcItem.left + (draw->rcItem.right - draw->rcItem.left - total) / 2;
        Icon(draw->hDC, glyph, RECT{start, draw->rcItem.top, start + D(18), draw->rcItem.bottom}, text);
        textRect.left = start + D(28);
        textRect.right = start + total;
        Text(draw->hDC, caption, textRect, (HFONT)normalFont.handle, text);
    } else
        Text(draw->hDC, caption, textRect, (HFONT)normalFont.handle, text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
LRESULT CALLBACK App::ButtonProc(HWND child, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (app && app->smoke && GetPropW(app->hwnd, L"BBS.ScrollProbe") &&
        (GetDlgCtrlID(child) == PickGame || GetDlgCtrlID(child) == OpenCache)) {
        const wchar_t* key = msg == WM_SHOWWINDOW   ? (wp ? L"BBS.ProbeShows" : L"BBS.ProbeHides")
                             : msg == WM_ERASEBKGND ? L"BBS.ProbeErases"
                                                    : nullptr;
        if (key) {
            const auto count = reinterpret_cast<INT_PTR>(GetPropW(app->hwnd, key));
            SetPropW(app->hwnd, key, reinterpret_cast<HANDLE>(count + 1));
        }
    }
    // Every main-window BUTTON is BS_OWNERDRAW and PaintButton fills its entire client rect.
    // The parent's WM_ERASEBKGND/double buffer does not cover WS_CLIPCHILDREN HWNDs: allowing
    // the native button erase here exposes the system COLOR_BTNFACE before WM_DRAWITEM arrives.
    if (msg == WM_ERASEBKGND)
        return 1;
    int id = GetDlgCtrlID(child);
    bool segments = id >= ThemeCombo && id <= StartupCombo;
    if (segments) {
        int count = id == CloseCombo ? 3 : 2;
        auto selected = [&]() { return std::max(0, (int)(INT_PTR)GetPropW(child, L"LOL.Selection") - 1); };
        auto choose = [&](int value, bool notify) {
            value = std::max(0, std::min(count - 1, value));
            bool changed = selected() != value;
            SetPropW(child, L"LOL.Selection", reinterpret_cast<HANDLE>((INT_PTR)value + 1));
            const wchar_t* themeNames[] = {L"界面主题：深色", L"界面主题：浅色"};
            const wchar_t* closeNames[] = {L"关闭窗口时：每次询问", L"关闭窗口时：最小化到托盘",
                                           L"关闭窗口时：退出程序"};
            SetWindowTextW(child, id == ThemeCombo   ? themeNames[value]
                                  : id == CloseCombo ? closeNames[value]
                                  : value            ? L"开机启动：开启"
                                                     : L"开机启动：关闭");
            InvalidateRect(child, nullptr, FALSE);
            if (changed && notify)
                SendMessageW(app->hwnd, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE),
                             reinterpret_cast<LPARAM>(child));
        };
        if (msg == CB_ADDSTRING)
            return 0;
        if (msg == CB_GETCOUNT)
            return count;
        if (msg == CB_GETCURSEL)
            return selected();
        if (msg == CB_SETCURSEL) {
            choose((int)wp, false);
            return selected();
        }
        if (msg == WM_GETDLGCODE)
            return DefSubclassProc(child, msg, wp, lp) | DLGC_WANTARROWS;
        if (msg == WM_KEYDOWN) {
            int value = selected();
            if (wp == VK_LEFT || wp == VK_UP)
                choose((value + count - 1) % count, true);
            else if (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_SPACE || wp == VK_RETURN)
                choose((value + 1) % count, true);
            else if (wp == VK_HOME)
                choose(0, true);
            else if (wp == VK_END)
                choose(count - 1, true);
            else
                return DefSubclassProc(child, msg, wp, lp);
            return 0;
        }
        if ((msg == WM_KEYUP || msg == WM_CHAR) && (wp == VK_SPACE || wp == VK_RETURN))
            return 0;
        if (msg == WM_LBUTTONUP) {
            bool pushed = (SendMessageW(child, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
            LRESULT result = DefSubclassProc(child, msg, wp, lp);
            RECT rc{};
            GetClientRect(child, &rc);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pushed && PtInRect(&rc, point))
                choose(id == StartupCombo ? 1 - selected()
                                          : std::min(count - 1, (int)(std::max(0L, point.x) * count /
                                                                      std::max(1L, rc.right))),
                       true);
            return result;
        }
    }
    if (msg == WM_MOUSEMOVE && !GetPropW(child, L"LOL.Hover")) {
        SetPropW(child, L"LOL.Hover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, child, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(child, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(child, L"LOL.Hover");
        InvalidateRect(child, nullptr, FALSE);
    } else if (msg == WM_MOUSEWHEEL) {
        return SendMessageW(app->hwnd, msg, wp, lp);
    } else if (msg == WM_SETFOCUS) {
        PostMessageW(app->hwnd, WM_APP + 12, reinterpret_cast<WPARAM>(child), 0);
        InvalidateRect(child, nullptr, FALSE);
    } else if (msg == WM_KILLFOCUS) {
        InvalidateRect(child, nullptr, FALSE);
    } else if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
        if (id >= NavFirst && id < NavFirst + PageCount) {
            int p = NavPageOrder[(NavPosition(id - NavFirst) + (wp == VK_DOWN ? 1 : PageCount - 1)) % PageCount];
            app->SetPage(p);
            SetFocus(app->Item(NavFirst + p));
            return 0;
        }
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(child, L"LOL.Hover");
        RemovePropW(child, L"LOL.Selection");
        RemoveWindowSubclass(child, ButtonProc, 1);
    }
    return DefSubclassProc(child, msg, wp, lp);
}
LRESULT CALLBACK App::EditProc(HWND child, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    const bool auditableCopy = GetDlgCtrlID(child) == LogEdit || GetParent(child) == app->licenseWindow;
    if (auditableCopy && msg == WM_KEYDOWN && (wp == 'C' || wp == VK_INSERT) &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        // Native EDIT may copy internally without a WM_COPY message. Route this user gesture through
        // the same WM_COPY path exactly once; never fetch the clipboard or read the selected text.
        if (!(lp & (1LL << 30)))
            SendMessageW(child, WM_COPY, 0, 0);
        return 0;
    }
    if (msg == WM_COPY && auditableCopy) {
        DWORD first = 0, last = 0;
        SendMessageW(child, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        UiLog(GetDlgCtrlID(child) == LogEdit ? L"UI_LOG_COPY" : L"UI_LICENSE_COPY",
              std::wstring(GetDlgCtrlID(child) == LogEdit ? L"复制日志选区" : L"复制许可选区") + L" 字符数=" +
                  std::to_wstring(last >= first ? last - first : 0));
        // WM_COPY has no success result; record intent only, never inspect clipboard contents.
    }
    if (msg == WM_MOUSEWHEEL) {
        if (GetWindowLongPtrW(child, GWL_STYLE) & ES_MULTILINE) {
            int remainder = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(child, L"BBS.Wheel")));
            const int ticks = AccumulateWheelTicks(remainder, GET_WHEEL_DELTA_WPARAM(wp));
            if (remainder)
                SetPropW(child, L"BBS.Wheel", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(remainder)));
            else
                RemovePropW(child, L"BBS.Wheel");
            if (ticks) {
                SendMessageW(child, EM_LINESCROLL, 0, -ticks * 3);
                app->InvalidateLogScrollBar();
            }
            return 0;
        }
        return SendMessageW(app->hwnd, msg, wp, lp);
    }
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(child, EM_SETSEL, 0, -1);
        return 0;
    }
    LRESULT result = DefSubclassProc(child, msg, wp, lp);
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_KEYDOWN || msg == WM_LBUTTONUP) {
        InvalidateRect(GetParent(child), nullptr, FALSE);
        if (msg == WM_SETFOCUS)
            PostMessageW(app->hwnd, WM_APP + 12, reinterpret_cast<WPARAM>(child), 0);
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(child, EditProc, 1);
    return result;
}
int App::RunDialog(DialogState& state, WNDPROC procedure, const wchar_t* klass, int width, int height) {
    const HWND owner = hwnd;
    if (stopping || !owner || !IsWindow(owner)) {
        state.failed = true;
        UiLog(L"UI_DIALOG_REJECTED", L"提示窗口未打开：正在退出或主窗口不可用", 1);
        return 0;
    }
    if (modalWindow && IsWindow(modalWindow)) {
        state.failed = true;
        UiLog(L"UI_DIALOG_REJECTED", L"提示窗口未打开：已有对话框", 1);
        SetForegroundWindow(modalWindow);
        return 0;
    }
    if (trayPopup && IsWindow(trayPopup))
        DestroyWindow(trayPopup);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance;
    wc.lpszClassName = klass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state.failed = true;
        UiLog(L"UI_DIALOG_FAILED", L"注册提示窗口失败 error=" + std::to_wstring(GetLastError()), 2);
        Notify(L"无法创建提示窗口，请检查系统资源。", true, false);
        return 0;
    }
    RECT ownerRect{};
    if (!GetWindowRect(owner, &ownerRect)) {
        state.failed = true;
        UiLog(L"UI_DIALOG_FAILED", L"读取主窗口位置失败 error=" + std::to_wstring(GetLastError()), 2);
        return 0;
    }
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        state.failed = true;
        UiLog(L"UI_DIALOG_FAILED", L"读取提示窗口显示区域失败 error=" + std::to_wstring(GetLastError()), 2);
        return 0;
    }
    width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    const int x =
        std::clamp(static_cast<int>(ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2),
                   static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width);
    const int y =
        std::clamp(static_cast<int>(ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2),
                   static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height);
    // No WS_BORDER / WS_EX_DLGMODALFRAME: those are painted by Windows with the SYSTEM palette.
    // This popup draws its own one-physical-pixel edge using the APPLICATION palette instead.
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, klass, state.title.c_str(), WS_POPUP, x, y, width, height,
                                  owner, nullptr, instance, &state);
    if (!dialog) {
        state.failed = true;
        UiLog(L"UI_DIALOG_FAILED", L"创建提示窗口失败 error=" + std::to_wstring(GetLastError()), 2);
        return 0;
    }
    modalWindow = dialog;
    const bool ownerWasEnabled = IsWindowEnabled(owner) != FALSE;
    if (ownerWasEnabled)
        EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UiLog(procedure == ConfirmProc ? L"UI_CONFIRM_SHOWN" : L"UI_DIALOG_SHOWN", state.title);
    SetForegroundWindow(dialog);
    SetFocus(dialog);
    UpdateWindow(dialog);
    bool quit = false, pumpFailed = false;
    int quitCode = 0;
    while (state.window && IsWindow(state.window)) {
        MSG message{};
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            quit = received == 0;
            pumpFailed = received == -1;
            quitCode = quit ? static_cast<int>(message.wParam) : 1;
            break;
        }
        const bool ownMessage = message.hwnd == dialog || (state.window && IsChild(dialog, message.hwnd));
        if (ownMessage && message.message == WM_KEYDOWN &&
            (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN)) {
            state.result = message.wParam == VK_RETURN && !state.closeChoice && !state.defaultCancel ? 1 : 0;
            DestroyWindow(dialog);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.window)
        DestroyWindow(state.window); // Never let GWLP_USERDATA outlive this stack-owned state.
    if (modalWindow == dialog)
        modalWindow = nullptr;
    if (hwnd == owner && IsWindow(owner) && ownerWasEnabled)
        EnableWindow(owner, TRUE);
    if (!quit && !pumpFailed && !stopping && hwnd == owner && IsWindowVisible(owner) && !IsIconic(owner))
        SetForegroundWindow(owner);
    if (quit)
        PostQuitMessage(quitCode); // Nested loops must not consume the main loop's WM_QUIT.
    if (pumpFailed) {
        state.failed = true;
        UiLog(L"UI_DIALOG_FAILED", L"提示窗口消息循环失败", 2);
        if (!stopping)
            Shutdown(L"对话框消息循环失败");
    }
    const int result = !quit && !pumpFailed && !stopping ? state.result : 0;
    UiLog(L"UI_DIALOG_RESULT", state.title + L" result=" + std::to_wstring(result));
    return result;
}
bool App::GuardUnsaved(int nextPage, bool closeAfter) {
    if (!initialized || stopping || !serviceStarted || page != 3 || !SettingsEdited())
        return true;
    const int choice = AskUnsavedDialog(); // 0 cancel, 1 discard, 2 save and continue
    UiLog(L"UI_UNSAVED_CHOICE", choice == 2   ? L"未保存设置：保存并继续"
                                : choice == 1 ? L"未保存设置：放弃修改"
                                              : L"未保存设置：取消离开");
    if (choice == 0)
        return false;
    if (choice == 1) {
        settingsDirty = false;
        UpdateControls(true);
        Notify(L"已放弃未保存的设置修改。", false, false);
        return true;
    }
    if (!SaveSettings())
        return false;
    if (closeAfter)
        pendingCloseAfterSave = true;
    else
        pendingPageAfterSave = nextPage;
    return false; // resume the original action once the service confirms the save
}
int App::AskUnsavedDialog() {
    DialogState state{this};
    state.title = L"设置有未保存的修改";
    state.body = L"当前“设置”页有修改还没保存。\r\n\r\n保存并继续：先保存这些修改，再继续刚才的操作。\r\n"
                 L"放弃修改：丢弃这次修改，恢复为上次保存的值。\r\n取消：留在设置页继续编辑。";
    state.closeChoice = true;
    return RunDialog(state, UnsavedAskProc, L"BBS.UnsavedAsk", D(470), D(286));
}
LRESULT CALLBACK App::UnsavedAskProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state || !state->app)
        return DefWindowProcW(window, msg, wp, lp);
    App* app = state->app;
    if (msg == WM_NCDESTROY) {
        if (app->modalWindow == window)
            app->modalWindow = nullptr;
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, msg, wp, lp);
    }
    RECT client{};
    GetClientRect(window, &client);
    const int w = client.right, h = client.bottom;
    RECT saveRect{w - app->D(28) - app->D(118), h - app->D(28) - app->D(38), w - app->D(28), h - app->D(28)};
    RECT discardRect{saveRect.left - app->D(10) - app->D(104), saveRect.top, saveRect.left - app->D(10),
                     saveRect.bottom};
    RECT cancelRect{discardRect.left - app->D(10) - app->D(76), discardRect.top,
                    discardRect.left - app->D(10), discardRect.bottom};
    auto hit = [&](POINT point) {
        return PtInRect(&saveRect, point)      ? 2
               : PtInRect(&discardRect, point) ? 1
               : PtInRect(&cancelRect, point)  ? 0
                                               : -1;
    };
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int choice = hit(point);
        if (choice >= 0) {
            state->result = choice;
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_PAINT: {
        PaintSession paint(window);
        HBRUSH background = CreateSolidBrush(app->palette.card);
        HBRUSH border = CreateSolidBrush(app->palette.border);
        if (background) {
            FillRect(paint.dc, &client, background);
            DeleteObject(background);
        }
        if (border) {
            FrameRect(paint.dc, &client, border);
            DeleteObject(border);
        }
        app->Text(paint.dc, state->title, RECT{app->D(28), app->D(24), w - app->D(28), app->D(54)},
                  (HFONT)app->brandFont.handle, app->palette.text);
        app->Text(paint.dc, state->body,
                  RECT{app->D(28), app->D(62), w - app->D(28), saveRect.top - app->D(16)},
                  (HFONT)app->normalFont.handle, app->palette.muted, DT_LEFT | DT_WORDBREAK);
        auto button = [&](RECT rect, const wchar_t* caption, bool accent) {
            app->Round(paint.dc, rect, accent ? app->palette.accent : app->palette.field,
                       accent ? app->palette.accent : app->palette.border, 8);
            app->Text(paint.dc, caption, rect, (HFONT)app->normalFont.handle,
                      accent ? RGB(255, 255, 255) : app->palette.text,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        button(cancelRect, L"取消", false);
        button(discardRect, L"放弃修改", false);
        button(saveRect, L"保存并继续", true);
        return 0;
    }
    case WM_CLOSE:
        state->result = 0;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
int App::AskCloseDialog() {
    DialogState state{this};
    state.title = L"关闭 BBS自动工具";
    state.body = L"要如何处理这个窗口？\r\n\r\n安全退出：取消后台任务后关闭程序。\r\n"
                 L"最小化到托盘：窗口隐藏，大厅联动继续。\r\n取消：返回窗口。";
    state.closeChoice = true;
    state.allowRemember = true;
    const int choice = RunDialog(state, CloseAskProc, L"BBS.CloseAsk", D(470), D(292));
    UiLog(L"UI_CLOSE_CHOICE", std::wstring(choice == 2   ? L"关闭确认：安全退出"
                                           : choice == 1 ? L"关闭确认：隐藏到托盘"
                                                         : L"关闭确认：取消") +
                                  L" 记住=" + (state.remember && choice > 0 ? L"是" : L"否"));
    if (choice > 0 && state.remember && serviceStarted && !stopping) {
        // Remember only an actual decision (tray or exit), never the Cancel path.
        status.closeAction = choice;
        UiLog(L"UI_SETTINGS_SAVE_INTENT", L"保存关闭偏好意图");
        if (Request(L"save_settings", {{"close_action", choice}}, L"已记住关闭方式。", true) == 0)
            Notify(L"暂时无法保存关闭偏好，下次仍会询问。", true, false);
        else
            Notify(choice == 1 ? L"以后关闭窗口将直接隐藏到托盘。" : L"以后关闭窗口将直接安全退出。", false,
                   false);
    }
    return choice;
}
LRESULT CALLBACK App::CloseAskProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state || !state->app)
        return DefWindowProcW(window, msg, wp, lp);
    App* app = state->app;
    if (msg == WM_NCDESTROY) {
        if (app->modalWindow == window)
            app->modalWindow = nullptr;
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, msg, wp, lp);
    }
    RECT client{};
    GetClientRect(window, &client);
    const int w = client.right, h = client.bottom;
    RECT exitRect{w - app->D(28) - app->D(120), h - app->D(28) - app->D(38), w - app->D(28), h - app->D(28)};
    RECT trayRect{exitRect.left - app->D(12) - app->D(140), exitRect.top, exitRect.left - app->D(12),
                  exitRect.bottom};
    RECT cancelRect{trayRect.left - app->D(12) - app->D(80), exitRect.top, trayRect.left - app->D(12),
                    exitRect.bottom};
    RECT rememberBox{app->D(28), exitRect.top - app->D(34), app->D(28) + app->D(16),
                     exitRect.top - app->D(34) + app->D(16)};
    RECT rememberRow{rememberBox.left, rememberBox.top - app->D(5), w - app->D(28),
                     rememberBox.bottom + app->D(5)};
    auto hit = [&](POINT point) {
        if (state->allowRemember && PtInRect(&rememberRow, point))
            return -2;
        return PtInRect(&exitRect, point)     ? 2
               : PtInRect(&trayRect, point)   ? 1
               : PtInRect(&cancelRect, point) ? 0
                                              : -1;
    };
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hot = hit(point);
        if (hot != state->hot) {
            state->hot = hot;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int choice = hit(point);
        if (choice == -2) {
            state->remember = !state->remember;
            InvalidateRect(window, nullptr, FALSE);
        } else if (choice >= 0) {
            state->result = choice;
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_PAINT: {
        PaintSession paint(window);
        HBRUSH background = CreateSolidBrush(app->palette.card);
        HBRUSH border = CreateSolidBrush(app->palette.border);
        if (background) {
            FillRect(paint.dc, &client, background);
            DeleteObject(background);
        }
        if (border) {
            FrameRect(paint.dc, &client, border);
            DeleteObject(border);
        }
        app->Text(paint.dc, state->title, RECT{app->D(28), app->D(24), w - app->D(28), app->D(54)},
                  (HFONT)app->brandFont.handle, app->palette.text);
        app->Text(paint.dc, state->body,
                  RECT{app->D(28), app->D(62), w - app->D(28), rememberRow.top - app->D(6)},
                  (HFONT)app->normalFont.handle, app->palette.muted, DT_LEFT | DT_WORDBREAK);
        if (state->allowRemember) {
            app->Round(paint.dc, rememberBox, state->remember ? app->palette.accent : app->palette.field,
                       state->remember ? app->palette.accent : app->palette.border, 4);
            if (state->remember)
                app->Text(paint.dc, L"✓", rememberBox, (HFONT)app->smallFont.handle, RGB(255, 255, 255),
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            app->Text(
                paint.dc, L"不再提示（记住这次选择）",
                RECT{rememberBox.right + app->D(8), rememberRow.top, w - app->D(28), rememberRow.bottom},
                (HFONT)app->smallFont.handle, state->hot == -2 ? app->palette.accent : app->palette.muted,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        auto button = [&](RECT rect, const wchar_t* caption, bool accent, int id) {
            app->Round(paint.dc, rect, accent ? app->palette.accent : app->palette.field,
                       accent ? app->palette.accent : app->palette.border, 8);
            app->Text(paint.dc, caption, rect, (HFONT)app->normalFont.handle,
                      accent ? RGB(255, 255, 255)
                             : (state->hot == id ? app->palette.accent : app->palette.text),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        button(cancelRect, L"取消", false, 0);
        button(trayRect, L"最小化到托盘", false, 1);
        button(exitRect, L"安全退出", true, 2);
        return 0;
    }
    case WM_CLOSE:
        state->result = 0;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
bool App::Confirm(const std::wstring& title, const std::wstring& body, bool defaultCancel) {
    DialogState state{this};
    state.title = title;
    state.body = body;
    state.defaultCancel = defaultCancel;
    const int width = D(defaultCancel ? 580 : 470);
    HDC dc = GetDC(hwnd);
    if (!dc) {
        UiLog(L"UI_DIALOG_FAILED", L"确认框测量失败 error=" + std::to_wstring(GetLastError()), 2);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, normalFont.handle);
    RECT calc{0, 0, width - D(60), 0};
    DrawTextW(dc, body.c_str(), static_cast<int>(body.size()), &calc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (old && old != HGDI_ERROR)
        SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    const bool accepted =
        RunDialog(state, ConfirmProc, L"BBS.ThemedConfirm", width,
                  std::max(D(224), D(118) + static_cast<int>(calc.bottom - calc.top) + D(78))) == 1;
    UiLog(state.failed || stopping ? L"UI_CONFIRM_ABORTED"
          : accepted               ? L"UI_CONFIRM_ACCEPTED"
                                   : L"UI_CONFIRM_CANCELLED",
          title);
    return accepted;
}
LRESULT CALLBACK App::ConfirmProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state || !state->app)
        return DefWindowProcW(window, msg, wp, lp);
    App* app = state->app;
    if (msg == WM_NCDESTROY) {
        if (app->modalWindow == window)
            app->modalWindow = nullptr;
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, msg, wp, lp);
    }
    RECT client{};
    GetClientRect(window, &client);
    const int w = client.right, h = client.bottom;
    RECT cancel{w - app->D(30) - app->D(96), h - app->D(30) - app->D(38), w - app->D(30), h - app->D(30)};
    RECT accept{cancel.left - app->D(12) - app->D(96), cancel.top, cancel.left - app->D(12), cancel.bottom};
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PaintSession paint(window);
        HBRUSH background = CreateSolidBrush(app->palette.card);
        HBRUSH border = CreateSolidBrush(app->palette.border);
        if (background) {
            FillRect(paint.dc, &client, background);
            DeleteObject(background);
        }
        if (border) {
            FrameRect(paint.dc, &client, border);
            DeleteObject(border);
        }
        app->Text(paint.dc, state->title, RECT{app->D(30), app->D(26), w - app->D(30), app->D(58)},
                  (HFONT)app->brandFont.handle, app->palette.text);
        app->Text(paint.dc, state->body,
                  RECT{app->D(30), app->D(64), w - app->D(30), cancel.top - app->D(16)},
                  (HFONT)app->normalFont.handle, app->palette.muted, DT_LEFT | DT_WORDBREAK);
        app->Round(paint.dc, accept, state->defaultCancel ? app->palette.field : app->palette.accent,
                   state->defaultCancel ? app->palette.border : app->palette.accent, 8);
        app->Round(paint.dc, cancel, state->defaultCancel ? app->palette.accent : app->palette.field,
                   state->defaultCancel ? app->palette.accent : app->palette.border, 8);
        app->Text(paint.dc, state->defaultCancel ? L"确认重绑" : L"确定", accept,
                  (HFONT)app->normalFont.handle,
                  state->defaultCancel ? app->palette.text : RGB(255, 255, 255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        app->Text(paint.dc, state->defaultCancel ? L"取消（默认）" : L"取消", cancel,
                  (HFONT)app->normalFont.handle,
                  state->defaultCancel ? RGB(255, 255, 255) : app->palette.text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&accept, point) || PtInRect(&cancel, point)) {
            state->result = PtInRect(&accept, point) ? 1 : 0;
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_CLOSE:
        state->result = 0;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

void App::ShowManual() {
    if (smoke) {
        Notify(L"隔离预览不打开外部窗口。", false, false);
        return;
    }
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(3200), RT_RCDATA);
    DWORD bytes = resource ? SizeofResource(module, resource) : 0;
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !bytes) {
        Notify(L"使用说明书资源不可用。", true, false);
        return;
    }
    const std::wstring path = root + L"\\BBS使用说明书.html";
    const std::wstring temporary = root + L"\\.bbs-manual-" + std::to_wstring(GetCurrentProcessId()) +
                                   L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Notify(L"无法在总目录保存使用说明书，请检查目录权限。", true, false);
        return;
    }
    DWORD written = 0;
    const bool copied = WriteFile(file, data, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    CloseHandle(file);
    if (!copied || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        Notify(L"使用说明书保存失败，请关闭占用文件的程序后重试。", true, false);
        return;
    }
    Notify(L"正在打开使用说明书…", false, false);
    Queue([this, path] {
        auto result = std::make_unique<UiResult>();
        result->kind = UiResult::Opened;
        const INT_PTR code = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", path.c_str(), nullptr,
                                                                   nullptr, SW_SHOWNORMAL));
        result->ok = code > 32;
        result->message = result->ok ? L"使用说明书已交给默认浏览器打开。"
                                    : L"无法打开说明书，请检查HTML文件的默认打开程序。";
        PostResult(std::move(result));
    });
}

void App::ShowAbout() {
    UiLog(L"UI_LICENSE_OPEN", L"打开关于与开源许可");
    if (licenseWindow) {
        UiLog(L"UI_LICENSE_WAKE", L"唤醒已有许可窗口");
        ShowWindow(licenseWindow, SW_RESTORE);
        SetForegroundWindow(licenseWindow);
        return;
    }
    std::vector<BYTE> bytes;
    licenseShowingOriginal = false;
    licenseText.clear();
    const std::wstring chinese = ChineseLicenseIntroduction();
    for (size_t i = 0; i < chinese.size(); ++i) {
        if (chinese[i] == L'\n' && (i == 0 || chinese[i - 1] != L'\r'))
            licenseText += L'\r';
        licenseText += chinese[i];
    }
    licenseOriginalText.clear();
    if (UiAssets::LoadBytes(instance, 205, bytes)) {
        std::string utf8(bytes.begin(), bytes.end());
        if (utf8.size() >= 3 && (BYTE)utf8[0] == 0xef && (BYTE)utf8[1] == 0xbb && (BYTE)utf8[2] == 0xbf)
            utf8.erase(0, 3);
        std::wstring decoded = Wide(utf8);
        for (size_t i = 0; i < decoded.size(); ++i) {
            if (decoded[i] == L'\n' && (i == 0 || decoded[i - 1] != L'\r'))
                licenseOriginalText += L'\r';
            licenseOriginalText += decoded[i];
        }
    } else {
        licenseOriginalText = L"内嵌许可资源未能载入，请联系发布者取得完整开源许可。";
        UiLog(L"UI_LICENSE_RESOURCE_FAILED", L"内嵌授权原文资源载入失败", 2);
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LicenseProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LOLBackupV3.Licenses";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    ATOM registered = RegisterClassExW(&wc);
    DWORD registrationError = registered ? ERROR_SUCCESS : GetLastError();
    if (!registered && registrationError != ERROR_CLASS_ALREADY_EXISTS) {
        UiLog(L"UI_LICENSE_WINDOW_FAILED", L"注册许可窗口失败 error=" + std::to_wstring(registrationError),
              2);
        Notify(L"无法打开开源许可窗口。", true, false);
        return;
    }
    RECT main{};
    GetWindowRect(hwnd, &main);
    licenseWindow = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"关于与开源许可 · BBS自动工具",
                                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, main.left + D(48),
                                    main.top + D(48), D(760), D(600), hwnd, nullptr, instance, this);
    const DWORD creationError = licenseWindow ? ERROR_SUCCESS : GetLastError();
    if (!licenseWindow)
        UiLog(L"UI_LICENSE_WINDOW_FAILED", L"创建许可窗口失败 error=" + std::to_wstring(creationError), 2);
    if (licenseWindow) {
        ApplyNativeCaption(licenseWindow);
        ShowWindow(licenseWindow, SW_SHOW);
        UiLog(L"UI_LICENSE_SHOWN", L"许可窗口已显示：默认中文说明");
        UpdateWindow(licenseWindow);
    } else
        Notify(L"无法打开开源许可窗口。", true, false);
}
LRESULT CALLBACK App::LicenseProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_CREATE: {
        auto traceCreation = [](const std::wstring& phase, HWND child, DWORD error) {
            if (!child)
                UiLog(L"UI_LICENSE_CONTROL_FAILED", phase + L"创建失败 error=" + std::to_wstring(error), 2);
        };
        // A standard EDIT has its default creation text limit before EM_SETLIMITTEXT can be sent.
        // Create empty first, raise the limit, and only then populate the complete embedded license.
        SetLastError(ERROR_SUCCESS);
        HWND edit = CreateWindowExW(
            0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 1, 1, window, reinterpret_cast<HMENU>(1), app->instance, nullptr);
        const DWORD editError = edit ? ERROR_SUCCESS : GetLastError();
        traceCreation(L"许可正文", edit, editError);
        if (!edit) {
            SetLastError(editError);
            return -1;
        }
        SendMessageW(edit, EM_SETLIMITTEXT, 2000000, 0);
        SetLastError(ERROR_SUCCESS);
        const BOOL textSet = SetWindowTextW(edit, app->licenseText.c_str());
        const DWORD textError = textSet ? ERROR_SUCCESS : GetLastError();
        const int actualCharacters = GetWindowTextLengthW(edit);
        const size_t expectedCharacters = app->licenseText.size();
        UiLog(textSet ? L"UI_LICENSE_TEXT_READY" : L"UI_LICENSE_TEXT_FAILED",
              L"中文许可说明载入 字符数=" + std::to_wstring(actualCharacters) + L" 预期=" +
                  std::to_wstring(expectedCharacters) + L" error=" + std::to_wstring(textError),
              textSet ? 0 : 2);
        if (!textSet) {
            SetLastError(textError);
            return -1;
        }
        SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(app));
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(app->normalFont.handle), FALSE);
        for (int id : {4, 2, 3}) {
            SetLastError(ERROR_SUCCESS);
            HWND button =
                CreateWindowExW(0, L"BUTTON",
                                id == 2   ? L"复制当前内容"
                                : id == 4 ? L"查看授权原文"
                                          : L"关闭",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, window,
                                reinterpret_cast<HMENU>((INT_PTR)id), app->instance, nullptr);
            const DWORD buttonError = button ? ERROR_SUCCESS : GetLastError();
            traceCreation(L"许可按钮" + std::to_wstring(id), button, buttonError);
            if (!button) {
                SetLastError(buttonError);
                return -1;
            }
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(app->normalFont.handle), FALSE);
        }
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        SetWindowPos(GetDlgItem(window, 1), nullptr, app->D(36), app->D(108), std::max(1, w - app->D(72)),
                     std::max(1, h - app->D(192)), SWP_NOZORDER);
        SetWindowPos(GetDlgItem(window, 4), nullptr, w - app->D(424), h - app->D(60), app->D(144), app->D(36),
                     SWP_NOZORDER);
        SetWindowPos(GetDlgItem(window, 2), nullptr, w - app->D(264), h - app->D(60), app->D(144), app->D(36),
                     SWP_NOZORDER);
        SetWindowPos(GetDlgItem(window, 3), nullptr, w - app->D(104), h - app->D(60), app->D(80), app->D(36),
                     SWP_NOZORDER);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {app->D(560), app->D(400)};
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PaintSession paint(window);
        RECT rc{};
        GetClientRect(window, &rc);
        HBRUSH bg = CreateSolidBrush(app->palette.bg);
        if (bg) {
            FillRect(paint.dc, &rc, bg);
            DeleteObject(bg);
        }
        app->Text(paint.dc, L"BBS自动工具", app->R(24, 20, 400, 36), (HFONT)app->titleFont.handle,
                  app->palette.text);
        app->Text(paint.dc,
                  app->licenseShowingOriginal ? L"授权原文（完整保留） · 版权与完整条款可复制核验"
                                              : L"中文说明（参考摘要） · 完整条款请查看授权原文",
                  RECT{app->D(24), app->D(64), rc.right - app->D(24), app->D(88)},
                  (HFONT)app->smallFont.handle, app->palette.muted);
        app->Round(paint.dc, RECT{app->D(24), app->D(96), rc.right - app->D(24), rc.bottom - app->D(72)},
                   app->palette.field, app->palette.border, 10);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), app->palette.text);
        SetBkColor(reinterpret_cast<HDC>(wp), app->palette.field);
        return reinterpret_cast<LRESULT>(app->fieldBrush.handle);
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        app->Round(draw->hDC, draw->rcItem, app->palette.card,
                   draw->itemState & ODS_FOCUS ? app->palette.accent : app->palette.border, 8);
        wchar_t text[128]{};
        GetWindowTextW(draw->hwndItem, text, 128);
        app->Text(draw->hDC, text, draw->rcItem, (HFONT)app->normalFont.handle, app->palette.text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 3)
            DestroyWindow(window);
        else if (LOWORD(wp) == 4) {
            const bool original = !app->licenseShowingOriginal;
            const std::wstring& text = original ? app->licenseOriginalText : app->licenseText;
            HWND edit = GetDlgItem(window, 1);
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowTextW(edit, text.c_str())) {
                UiLog(L"UI_LICENSE_VIEW_FAILED", L"切换许可正文失败 error=" + std::to_wstring(GetLastError()),
                      2);
                app->Notify(L"许可正文切换失败，请重新打开窗口。", true, false);
                return 0;
            }
            app->licenseShowingOriginal = original;
            SetWindowTextW(GetDlgItem(window, 4), original ? L"返回中文说明" : L"查看授权原文");
            SetWindowTextW(window, original ? L"授权原文 · BBS自动工具" : L"关于与开源许可 · BBS自动工具");
            SendMessageW(edit, EM_SETSEL, 0, 0);
            SendMessageW(edit, EM_SCROLLCARET, 0, 0);
            UiLog(L"UI_LICENSE_VIEW_CHANGED",
                  std::wstring(original ? L"查看完整授权原文" : L"返回中文许可说明") + L" 字符数=" +
                      std::to_wstring(GetWindowTextLengthW(edit)));
            InvalidateRect(window, nullptr, FALSE);
        } else if (LOWORD(wp) == 2) {
            UiLog(L"UI_LICENSE_COPY_INTENT",
                  app->licenseShowingOriginal ? L"复制当前完整授权原文" : L"复制当前中文许可说明");
            HWND edit = GetDlgItem(window, 1);
            DWORD start = 0, end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, WM_COPY, 0, 0);
            SendMessageW(edit, EM_SETSEL, start, end);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (app->licenseWindow == window) {
            UiLog(L"UI_LICENSE_CLOSED", L"许可窗口已关闭");
            app->licenseWindow = nullptr;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
const core::ToolState* App::ToolStatus(int index) const {
    if (index < 0 || index >= VisibleToolCount)
        return nullptr;
    for (const auto& state : toolStates)
        if (state.id == ToolIds[index])
            return &state;
    return nullptr;
}
void App::RefreshTools() {
    toolStates = toolManager.Snapshot();
    RefreshToolDirectories();
}
bool App::RefreshToolDirectories() {
    const ULONGLONG now = GetTickCount64();
    const bool due = !toolDirectoriesCheckedAt || now - toolDirectoriesCheckedAt >= 2000;
    bool changed = false;
    for (int i = 0; i < VisibleToolCount; ++i) {
        const auto* state = ToolStatus(i);
        const std::wstring path = state ? state->directory : L"";
        if (!due && path == toolDirectoryPaths[i])
            continue;
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        if (!path.empty()) {
            if (smoke)
                ++uiPerf.directoryQueries;
            attributes = GetFileAttributesW(path.c_str());
        }
        const bool available =
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        changed = changed || available != toolDirectoryAvailable[i] || path != toolDirectoryPaths[i];
        toolDirectoryPaths[i] = path;
        toolDirectoryAvailable[i] = available;
    }
    if (due)
        toolDirectoriesCheckedAt = now;
    return changed;
}
void App::InvalidateLogScrollBar() {
    // Include the complete old/new thumb and antialiased track edges, not the fixed frame.
    RECT track = R(Left() + ContentWidth() - 36, 244, 20, std::max(48, height - 344) + 8);
    InvalidateRect(hwnd, &track, FALSE);
}
void App::SendTool(int id) {
    const int index = (id - ToolFirst) / ToolStride, action = (id - ToolFirst) % ToolStride;
    if (id < ToolFirst || index >= VisibleToolCount || (action != 0 && action != 3))
        return;
    const auto* state = ToolStatus(index);
    if (stopping || !toolsInitialized || !state)
        return;
    if (action == 3) {
        const DWORD attributes =
            state->directory.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(state->directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Notify(L"应用目录尚不可用；首次点击“打开”后再试。", false, false);
        } else {
            Open(state->directory);
        }
        return;
    }
    // Defense in depth: injected WM_COMMAND must never prepare or launch in smoke mode.
    if (smoke) {
        Notify(L"隔离预览不会准备或打开第三方应用。", false, false);
        return;
    }
    if (!state->bundled || state->busy)
        return;
    if (!toolManager.Launch(ToolIds[index]))
        Notify(L"暂时无法打开应用，请查看下方提示后重试。", true, false);
    else if (state->running && ToolIds[index] == core::ToolId::Rose)
        Notify(L"Rose正在运行，请在游戏客户端中使用", false, false);
    RefreshTools();
    EnableControls();
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void App::PaintTools(HDC dc, int x, int cw) {
    const UINT wrap = DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS;
    const wchar_t* names[VisibleToolCount] = {L"Rose · 外服换肤 · 可手动更新",
                                              L"League Akari · 战绩助手 · 可手动更新",
                                              L"ABC · 外服账号管理工具 · 可手动更新"};
    const wchar_t* descriptions[VisibleToolCount] = {
        L"完整官方 Rose；首次打开自动准备内置文件，启动可能需要管理员确认（UAC）。\n"
        L"LTK 只支持 1.29.0 或之前的版本；新 LTK 不再支持 Rose 这种第三方宿主。",
        L"完整 League Akari，在独立窗口使用；面向 Riot 客户端，不适用于腾讯国服客户端。",
        L"用户提供的软件，首次打开自动准备程序文件；汉化缓存由 ABC 按自身功能管理。"};
    for (int i = 0; i < VisibleToolCount; ++i) {
        const int y = 160 + i * 232;
        const auto* state = ToolStatus(i);
        Card(dc, x, y, cw, 216);
        Text(dc, names[i], R(x + 24, y + 16, cw - 48, 26), (HFONT)brandFont.handle, palette.text);
        Text(dc, descriptions[i], R(x + 24, y + 48, cw - 48, 36), (HFONT)normalFont.handle, palette.muted,
             wrap);
        std::wstring line = !state                  ? L"正在载入应用信息"
                            : state->busy           ? L"正在打开，请稍候…"
                            : state->running        ? (ToolIds[i] == core::ToolId::Rose
                                                           ? L"Rose正在运行，请在游戏客户端中使用"
                                                       : ToolIds[i] == core::ToolId::Akari
                                                           ? L"Akari 正在运行，请在应用中使用"
                                                           : L"ABC 正在运行，请在应用中使用")
                            : !state->error.empty() ? L"上次打开未完成，可重试"
                            : state->bundled        ? L"点击打开即可使用"
                                                    : L"当前分发包未包含此应用";
        Text(dc, line, R(x + 24, y + 88, cw - 48, 22), (HFONT)normalFont.handle,
             state && state->running ? palette.good : palette.muted);
        const bool hasDirectory = toolDirectoryAvailable[i];
        Text(dc, hasDirectory ? L"应用目录：" + state->directory : L"应用目录尚不可用，首次打开后自动创建。",
             R(x + 24, y + 114, cw - 48, 20), (HFONT)smallFont.handle, palette.muted);
        const std::wstring error = !toolsStartupError.empty() ? toolsStartupError
                                   : state                    ? state->error
                                                              : L"";
        const std::wstring detail = !error.empty() ? L"提示：" + error
                                    : smoke
                                        ? L"隔离预览：不会准备或打开第三方应用。"
                                    : ToolIds[i] == core::ToolId::Abc
                                        ? L"仅在点击打开时启动，不自动跟随大厅。"
                                        : L"Rose 在游戏客户端中使用；Akari 在独立窗口使用，不自动跟随大厅。";
        Text(dc, detail, R(x + 24, y + 138, cw - 48, 32), (HFONT)smallFont.handle,
             error.empty() ? palette.muted : palette.danger, wrap);
    }
}
bool App::HasPending(const std::wstring& method) const {
    for (const auto& pair : pending)
        if (pair.second.method == method)
            return true;
    return false;
}
bool App::Busy() const {
    return status.backupState == L"running" || HasPending(L"backup_now") || HasPending(L"restore_now") ||
           HasPending(L"import_cache");
}
void App::EnableControls() {
    bool ready = serviceStarted && !stopping, busy = Busy();
    const bool rootsReady = status.gameOk && !status.gameRoot.empty() && !status.cacheRoot.empty();
    for (const auto& c : controls) {
        bool enabled = !stopping;
        if (c.id >= Backup && c.id < 300)
            enabled = ready;
        if (c.id == Backup)
            enabled = ready && !busy && rootsReady;
        if (c.id == Restore)
            enabled = ready && !busy && rootsReady;

        if (c.id == Cancel)
            enabled = ready && busy && !HasPending(L"cancel_op");
        if (c.id == Import)
            enabled = ready && !busy && !status.cacheRoot.empty();
        if (c.id == PickGame || c.id == PickCache)
            enabled =
                (ready && !busy) || (!stopping && bootstrapFailed && !bootstrapBusy && bootstrapConfigValid);
        if (c.id == Auto)
            enabled = ready && !HasPending(L"set_auto") && (status.autoOn || rootsReady);
        if (c.id == Save)
            enabled =
                (ready && !HasPending(L"save_settings")) || (!stopping && bootstrapFailed && !bootstrapBusy);
        if (c.id == ThemeQuick)
            enabled = ready && !HasPending(L"save_settings");
        if (c.id == RefreshLogs)
            enabled = ready && !HasPending(L"get_logs");
        if (c.id == OpenLogs)
            enabled = !stopping;
        if (c.id == OldCache)
            enabled = ready && !legacyCache.empty();
        if (c.id == StartupCombo && smoke)
            enabled = false;
        if (c.id == QuickRose || c.id == QuickAkari || c.id == QuickAbc) {
            const auto* state = ToolStatus(c.id == QuickAbc ? 2 : c.id == QuickAkari ? 1 : 0);
            enabled = !stopping && toolsInitialized && state && !smoke && state->bundled && !state->busy;
        }
        if (c.id >= ToolFirst && c.id < ToolFirst + VisibleToolCount * ToolStride) {
            const auto* state = ToolStatus((c.id - ToolFirst) / ToolStride);
            const int action = (c.id - ToolFirst) % ToolStride;
            enabled = !stopping && toolsInitialized && state;
            if (enabled && action == 0)
                enabled = !smoke && state->bundled && !state->busy;
            else if (enabled && action == 3) {
                enabled = toolDirectoryAvailable[(c.id - ToolFirst) / ToolStride];
            } else {
                enabled = false;
            }
        }
        if ((IsWindowEnabled(c.hwnd) != FALSE) != enabled)
            EnableWindow(c.hwnd, enabled);
    }
}
bool App::SettingsEdited() const {
    if (!initialized || !hwnd || !serviceStarted)
        return false;
    wchar_t buffer[16]{};
    GetWindowTextW(Item(SyncEdit), buffer, 16);
    if (std::wstring(buffer) != std::to_wstring(status.syncMinutes))
        return true;
    const int theme = static_cast<int>(SendMessageW(Item(ThemeCombo), CB_GETCURSEL, 0, 0));
    const int close = static_cast<int>(SendMessageW(Item(CloseCombo), CB_GETCURSEL, 0, 0));
    const int startup = static_cast<int>(SendMessageW(Item(StartupCombo), CB_GETCURSEL, 0, 0));
    const int expectedTheme = (smoke ? smokeTheme : status.theme == 1) ? 1 : 0;
    if (theme >= 0 && theme != expectedTheme)
        return true;
    if (close >= 0 && close != status.closeAction)
        return true;
    if (startup >= 0 && startup != (status.autostart ? 1 : 0))
        return true;
    return false;
}
void App::UpdateControls(bool force) {
    if (!initialized)
        return;
    syncingControls = true;
    auto setPath = [this](int id, const std::wstring& path) {
        int len = GetWindowTextLengthW(Item(id));
        std::wstring old(len + 1, L'\0');
        GetWindowTextW(Item(id), &old[0], len + 1);
        old.resize(len);
        if (old != path)
            SetWindowTextW(Item(id), path.c_str());
    };
    setPath(ThemeQuick, light ? L"切换深色" : L"切换浅色");
    setPath(Auto, status.autoOn ? L"大厅联动：已开启" : L"大厅联动：已暂停");
    setPath(Backup, HasPending(L"backup_now") ? L"正在备份…" : L"立即备份");
    setPath(Restore, HasPending(L"restore_now") ? L"正在恢复…" : L"恢复到游戏…");
    setPath(Save, !serviceStarted                ? (bootstrapBusy ? L"正在启动…" : L"重试启动")
                  : HasPending(L"save_settings") ? L"正在保存…"
                                                 : (SettingsEdited() ? L"保存修改 •" : L"保存设置"));
    const bool unreadable = bootstrapFailed && !bootstrapConfigValid;
    setPath(GameEdit, unreadable ? L"（配置未能安全读取）" : status.gameRoot);
    setPath(CacheEdit, unreadable ? L"（配置未能安全读取）" : status.cacheRoot);
    if (force || !SettingsEdited()) {
        setPath(SyncEdit, std::to_wstring(std::max(1, status.syncMinutes)));
        auto select = [this](int id, int value) {
            if (SendMessageW(Item(id), CB_GETCURSEL, 0, 0) != value)
                SendMessageW(Item(id), CB_SETCURSEL, value, 0);
        };
        select(ThemeCombo, (smoke ? smokeTheme : status.theme == 1) ? 1 : 0);
        select(CloseCombo, std::max(0, std::min(2, status.closeAction)));
        select(StartupCombo, status.autostart ? 1 : 0);
    }
    syncingControls = false;
    EnableControls();
}
void App::Notify(const std::wstring& message, bool error, bool balloon) {
    notice = message;
    errorNotice = error;
    noticeUntil = GetTickCount64() + 15000;
    InvalidateRect(hwnd, nullptr, FALSE);
    if (balloon && !IsWindowVisible(hwnd))
        TrayBalloon(message, error);
}
int App::Request(const std::wstring& method, const json& params, const std::wstring& success, bool silent) {
    const bool logged = LoggedUiMethod(method);
    if (!serviceStarted || stopping) {
        if (logged)
            UiLog(L"UI_REQUEST_REJECTED", L"请求未发送：服务未就绪或正在退出 method=" + method, 1);
        return 0;
    }
    if (HasPending(method)) {
        if (logged)
            UiLog(L"UI_REQUEST_REJECTED", L"请求未发送：同类请求仍在处理 method=" + method, 1);
        if (!silent)
            Notify(L"同类请求正在处理中，请等待返回结果。", false, false);
        return 0;
    }
    int id = requestId++;
    core::ScopedLogContext trace(L"request:" + std::to_wstring(id));
    // Serialize the real JSON object; log only an audited method/id and optional local directory.
    std::wstring serialized;
    try {
        serialized = Wide(params.dump());
    } catch (...) {
        if (logged)
            UiLog(L"UI_REQUEST_FAILED", L"请求序列化失败 " + UiRequestLabel(method, id), 2);
        throw;
    }
    Pending operation;
    operation.method = method;
    operation.success = success;
    operation.sent = GetTickCount64();
    operation.silent = silent;
    pending.emplace(id, std::move(operation));
    if (logged) {
        std::wstring detail = L"发送服务请求 " + UiRequestLabel(method, id);
        if (method == L"set_game_root" || method == L"set_cache_root" || method == L"import_cache")
            detail += L" 目录=" + UiLocalDirectory(JString(params, "path"));
        UiLog(L"UI_REQUEST_SENT", detail);
    }
    core::Service::Instance().PostCommand(id, method, serialized);
    if (!silent)
        Notify(L"请求已发送，正在等待服务处理…", false, false);
    UpdateControls();
    return id;
}
void App::Refresh() {
    if (!serviceStarted || stopping)
        return;
    auto previous = status;
    status = core::Service::Instance().GetStatus();
    if (smoke && smokeAutoPreview >= 0) {
        static const wchar_t* codes[] = {
            L"active",        L"offline",        L"restore_failed", L"disabled", L"restore_in_progress",
            L"unknown_lobby", L"restore_pending"};
        static const wchar_t* reasons[] = {L"大厅在线，按设定间隔镜像 Game 到缓存",
                                           L"大厅已离线，等待下次启动",
                                           L"上次还原未完成，请查看文件错误并重试",
                                           L"大厅联动已关闭，仅可手动备份或恢复",
                                           L"正在将缓存直接复制到 Game",
                                           L"正在检测大厅状态",
                                           L"大厅启动还原已排队，等待当前任务结束"};
        status.autoState = codes[smokeAutoPreview];
        status.autoReason = reasons[smokeAutoPreview];
        status.lobbyCycle = status.autoReason;
        status.periodicReady = smokeAutoPreview == 0;
        status.matchBackupReady = false;
        status.nextSync = status.periodicReady ? L"4分59秒" : status.autoReason;
    }
    if (previous.backupState != status.backupState || previous.autoOn != status.autoOn ||
        previous.monitorKnown != status.monitorKnown || previous.lobbyOnline != status.lobbyOnline) {
        UiLog(L"UI_STATE_CHANGED", L"界面状态变更：任务运行=" +
                                       std::wstring(status.backupState == L"running" ? L"是" : L"否") +
                                       L" 联动=" + (status.autoOn ? L"开" : L"关") + L" 监控=" +
                                       (status.monitorKnown ? L"已知" : L"未知") + L" 大厅在线=" +
                                       (status.lobbyOnline ? L"是" : L"否"));
    }
    lastError = status.lastError;
    bool desiredLight = smoke ? smokeTheme : status.theme == 1;
    if (light != desiredLight) {
        light = desiredLight;
        ApplyTheme();
        UiLog(L"UI_THEME_APPLIED",
              std::wstring(L"服务设置已应用到界面：") + (light ? L"浅色主题" : L"深色主题"));
    }
    if (previous.backupState != status.backupState || previous.progressPercent != status.progressPercent ||
        previous.progressLabel != status.progressLabel || previous.lastSync != status.lastSync ||
        previous.cacheEntries != status.cacheEntries || previous.lobbyCycle != status.lobbyCycle ||
        previous.lobbyOnline != status.lobbyOnline || previous.monitorKnown != status.monitorKnown ||
        previous.autoOn != status.autoOn || previous.syncMinutes != status.syncMinutes ||
        previous.periodicReady != status.periodicReady ||
        previous.matchBackupReady != status.matchBackupReady || previous.autoState != status.autoState ||
        previous.autoReason != status.autoReason || previous.nextSync != status.nextSync)
        InvalidateRect(hwnd, nullptr, FALSE);
    UpdateControls();
    if (previous.gameOk != status.gameOk)
        Layout();
    QueueWelcome();
}
void App::Drain() {
    for (auto& event : core::Service::Instance().DrainEvents()) {
        try {
            HandleEvent(json::parse(Utf8(event.json)));
            // Parse-error logging is latched for this UI session; unrelated status events must not
            // rearm it and turn a malformed polling reply into repetitive log-page writes.
        } catch (const std::exception&) {
            if (!serviceMessageMalformed)
                UiLog(L"UI_SERVICE_MESSAGE_FAILED", L"服务消息解析或处理失败；原始内容不写入日志", 2);
            serviceMessageMalformed = true;
            Notify(L"收到无法解析的服务消息，请查看日志。", true);
        }
    }
    Refresh();
}
void App::HandleEvent(const json& event) {
    if (!event.is_object())
        return;
    if (event.contains("id") && event["id"].is_number_integer() && event.contains("ok") &&
        event["ok"].is_boolean()) {
        int id = event["id"].get<int>();
        auto it = pending.find(id);
        if (it == pending.end())
            return;
        Pending operation = it->second;
        pending.erase(it);
        bool ok = event["ok"].get<bool>();
        {
            core::ScopedLogContext trace(L"request:" + std::to_wstring(id));
            if (LoggedUiMethod(operation.method)) {
                const auto dataIt = event.find("data");
                const bool queued = ok && dataIt != event.end() && dataIt->is_object() &&
                                    ((dataIt->contains("queued") && (*dataIt)["queued"] == true) ||
                                     (dataIt->contains("accepted") && (*dataIt)["accepted"] == true));
                UiLog(ok ? (queued ? L"UI_REQUEST_ACCEPTED" : L"UI_REQUEST_SUCCEEDED") : L"UI_REQUEST_FAILED",
                      std::wstring(ok ? (queued ? L"服务已接收，尚非执行完成 " : L"服务请求成功 ")
                                      : L"服务请求失败 ") +
                          UiRequestLabel(operation.method, id),
                      ok ? 0 : 2);
                if (!ok && (operation.method == L"set_game_root" || operation.method == L"set_cache_root" ||
                            operation.method == L"import_cache"))
                    UiLog(L"UI_DIRECTORY_REJECTED",
                          L"目录请求未生效：服务拒绝或验证失败 " + UiRequestLabel(operation.method, id), 1);
            } else if (operation.method == L"get_status" || operation.method == L"get_logs") {
                // Automatic reads must not log request/response bodies or self-amplify the log page.
                bool& failed = pollFailed[operation.method];
                if (failed == ok)
                    UiLog(ok ? L"UI_POLL_RECOVERED" : L"UI_POLL_FAILED",
                          std::wstring(ok ? L"自动读取已恢复 " : L"自动读取首次失败 ") +
                              UiRequestLabel(operation.method, id),
                          ok ? 0 : 1);
                failed = !ok;
            }
        }
        if (ok) {
            json data = event.value("data", json::object());
            if (operation.method == L"get_logs")
                ShowLogs(data);
            else if (operation.method == L"save_settings") {
                settingsDirty = false;
                Refresh();
                UpdateControls(true);
                if (pendingCloseAfterSave) {
                    pendingCloseAfterSave = false;
                    ClosePolicy();
                } else if (pendingPageAfterSave >= 0) {
                    const int target = pendingPageAfterSave;
                    pendingPageAfterSave = -1;
                    SetPage(target);
                }
            }
            // ACK means success only after Service has returned it; queued is never completion.
            bool queued = data.is_object() && ((data.contains("queued") && data["queued"] == true) ||
                                               (data.contains("accepted") && data["accepted"] == true));
            if (!operation.silent) {
                std::wstring message = queued ? L"任务已排队，完成后会通知你。" : operation.success;
                if (data.is_object())
                    message = JString(data, "message", JString(data, "summary", message));
                if (message.empty())
                    message = L"操作已完成。";
                Notify(message);
            }
            lastError.clear();
        } else {
            json error = event.value("error", json::object());
            std::wstring message = error.is_string() ? Wide(error.get<std::string>())
                                                     : JString(error, "message", L"操作未完成，请查看日志。");
            lastError = message;
            pendingCloseAfterSave = false;
            pendingPageAfterSave = -1;
            Notify(message, true);
        }
        Refresh();
        UpdateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    std::wstring type = JString(event, "event");
    json data = event.value("data", json::object());
    if (type == L"status" && data.is_object()) {
        if (data.contains("last_error"))
            lastError = JString(data, "last_error");
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (type == L"toast" && data.is_object()) {
        std::wstring body = JString(data, "body", JString(data, "message")), title = JString(data, "title");
        std::wstring level = JString(data, "kind", JString(data, "type", JString(data, "level")));
        Notify(title.empty() ? body : title + (body.empty() ? L"" : L" · " + body),
               level == L"error" || level == L"danger");
    } else if (type == L"progress")
        InvalidateRect(hwnd, nullptr, FALSE);
}
void App::ShowLogs(const json& data) {
    std::wstring text;
    const json* structured = nullptr;
    if (data.is_array())
        structured = &data;
    else if (data.is_object() && data.contains("logs") && data["logs"].is_array())
        structured = &data["logs"];
    if (!structured && data.is_string())
        text = Wide(data.get<std::string>());
    else if (!structured && data.is_object() && data.contains("text") && data["text"].is_string())
        text = Wide(data["text"].get<std::string>());
    if (structured) {
        const json* list = structured;
        if (list) {
            // One line per meaningful event: HH:mm:ss + short level + message. Internal asset/probe
            // chatter is hidden unless it is a warning or error, and repeats collapse into one line.
            std::vector<std::pair<std::wstring, int>> lines;
            for (const auto& entry : *list) {
                if (!entry.is_object())
                    continue;
                std::wstring level = JString(entry, "level"), module = JString(entry, "module");
                const std::wstring message = JString(entry, "msg", JString(entry, "message"));
                const bool error = level == L"error" || level == L"fatal";
                const bool warn = level == L"warn";
                if (!error && !warn) {
                    const bool internal =
                        module == L"ui-assets" || module == L"ui-glyphs" || module == L"ui-about";
                    const bool lifecycle = message == L"原生桌面界面启动" ||
                                           message == L"界面请求安全退出，等待后台文件任务结束" ||
                                           message.rfind(L"原生服务已启动", 0) == 0;
                    if (internal || lifecycle)
                        continue;
                }
                std::wstring stamp = JString(entry, "ts");
                if (stamp.size() >= 19)
                    stamp = stamp.substr(11, 8);
                std::wstring line = stamp + L"  ·  ";
                if (error)
                    line += L"错误：";
                else if (warn)
                    line += L"提醒：";
                line += message;
                if (!lines.empty() && lines.back().first == line)
                    ++lines.back().second;
                else
                    lines.push_back({line, 1});
            }
            // Newest first: the latest event is what a user looks at first.
            for (auto it = lines.rbegin(); it != lines.rend(); ++it)
                text += it->first +
                        (it->second > 1 ? L"（重复 " + std::to_wstring(it->second) + L" 次）" : L"") +
                        L"\r\n";
        }
    }
    if (text.empty())
        text = L"尚无运行日志。完成首次配置后，重要操作会记录在这里。";
    // Keep the NEWEST prefix. Taking the last 200 lines after reverse-order output discarded the
    // latest events whenever an older multi-line message was large.
    size_t cut = std::min<size_t>(text.size(), 200000);
    int displayLines = 0;
    for (size_t i = 0; i < cut; ++i) {
        if (text[i] == L'\n' && ++displayLines >= 200) {
            cut = i + 1;
            break;
        }
    }
    if (cut < text.size()) {
        if (cut && text[cut - 1] >= 0xd800 && text[cut - 1] <= 0xdbff)
            --cut;
        text.resize(cut);
        text += L"\r\n（较早记录已省略）";
    }
    // Standard EDIT expects CRLF, including text returned by future service versions.
    std::wstring normalized;
    normalized.reserve(text.size() + 100);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
            normalized += L'\r';
        normalized += text[i];
    }
    SetWindowTextW(Item(LogEdit), normalized.c_str());
}
void App::RequestLogs() {
    if (!serviceStarted || HasPending(L"get_logs"))
        return;
    lastLogsRequest = GetTickCount64();
    Request(L"get_logs", {{"limit", 200}}, L"", true);
}
void App::Capture() {
    if (!smoke || captured || capturePath.empty())
        return;
    captured = true;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        Notify(L"无法读取截图窗口尺寸。", true, false);
        return;
    }
    HDC source = GetDC(hwnd);
    if (!source) {
        Notify(L"无法创建截图绘图上下文。", true, false);
        return;
    }
    Buffer bitmap(source, client.right, client.bottom);
    ReleaseDC(hwnd, source);
    if (!bitmap.dc || !bitmap.bitmap) {
        Notify(L"无法创建窗口截图位图。", true, false);
        return;
    }
    // Use the same native renderer and actual child HWNDs as WM_PRINTCLIENT.
    // Avoid PrintWindow compositor passes that may reprint unclipped children.
    // No desktop DC or other application window is read.
    PrintClient(bitmap.dc);
    SelectObject(bitmap.dc, bitmap.previous);
    bitmap.previous = nullptr;
    std::shared_ptr<void> owned(bitmap.bitmap, [](void* p) { DeleteObject(static_cast<HBITMAP>(p)); });
    bitmap.bitmap = nullptr;
    std::wstring path = capturePath;
    Queue([this, owned, path] {
        Gdiplus::Bitmap image(static_cast<HBITMAP>(owned.get()), nullptr);
        const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        auto result = std::make_unique<UiResult>();
        result->kind = UiResult::Notice;
        Gdiplus::Status saved = image.GetLastStatus();
        if (saved == Gdiplus::Ok)
            saved = image.Save(path.c_str(), &png, nullptr);
        result->ok = saved == Gdiplus::Ok;
        result->message = result->ok ? L"当前窗口预览已保存为 PNG。"
                                     : L"窗口预览保存失败（GDI+ " + std::to_wstring((int)saved) + L"）。";
        PostResult(std::move(result));
    });
}
bool App::SaveSettings() {
    if (!serviceStarted)
        return RetryStartup();
    UiLog(L"UI_SETTINGS_SAVE_INTENT", L"保存设置意图；不记录完整配置");
    wchar_t minutesText[16]{};
    GetWindowTextW(Item(SyncEdit), minutesText, 16);
    wchar_t* end = nullptr;
    long minutes = wcstol(minutesText, &end, 10);
    if (!minutesText[0] || !end || *end || minutes < 1 || minutes > 1440) {
        UiLog(L"UI_SETTINGS_REJECTED", L"设置验证拒绝：同步间隔超出范围或格式无效", 1);
        Notify(L"同步间隔请输入 1–1440 分钟。", true);
        SetFocus(Item(SyncEdit));
        return false;
    }
    int theme = (int)SendMessageW(Item(ThemeCombo), CB_GETCURSEL, 0, 0),
        close = (int)SendMessageW(Item(CloseCombo), CB_GETCURSEL, 0, 0),
        startup = (int)SendMessageW(Item(StartupCombo), CB_GETCURSEL, 0, 0);
    if (theme < 0 || close < 0 || startup < 0) {
        UiLog(L"UI_SETTINGS_REJECTED", L"设置验证拒绝：分段偏好未完整选择", 1);
        Notify(L"请完整选择主题、关闭行为和自启动选项。", true);
        return false;
    }
    json params = {{"sync_minutes", minutes}, {"theme", theme}, {"close_action", close}};
    if (!smoke)
        params["autostart"] = startup == 1;
    return Request(L"save_settings", params, L"设置已保存并生效。") != 0;
}
std::wstring App::PickDirectory(const std::wstring& title, const std::wstring& initial) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.Put()));
    if (FAILED(hr)) {
        UiLog(L"UI_DIRECTORY_PICK_FAILED", L"创建目录选择器失败 hresult=" + std::to_wstring((LONG)hr), 2);
        Notify(L"无法打开目录选择器：" + WinError((DWORD)hr), true);
        return {};
    }
    DWORD options = 0;
    if (FAILED(hr = dialog->GetOptions(&options)) ||
        FAILED(hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                       FOS_DONTADDTORECENT)) ||
        FAILED(hr = dialog->SetTitle(title.c_str()))) {
        UiLog(L"UI_DIRECTORY_PICK_FAILED", L"初始化目录选择器失败 hresult=" + std::to_wstring((LONG)hr), 2);
        Notify(L"目录选择器初始化失败：" + WinError((DWORD)hr), true);
        return {};
    }
    if (!initial.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(folder.Put()))))
            dialog->SetFolder(folder.p);
    }
    UiLog(L"UI_DIRECTORY_PICK_SHOWN", title + L" 初始目录=" + UiLocalDirectory(initial));
    hr = dialog->Show(hwnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        UiLog(L"UI_DIRECTORY_PICK_CANCELLED", L"用户取消目录选择");
        Notify(L"已取消选择。", false, false);
        return {};
    }
    if (FAILED(hr)) {
        UiLog(L"UI_DIRECTORY_PICK_FAILED", L"显示目录选择器失败 hresult=" + std::to_wstring((LONG)hr), 2);
        Notify(L"目录选择失败：" + WinError((DWORD)hr), true);
        return {};
    }
    ComPtr<IShellItem> result;
    if (FAILED(hr = dialog->GetResult(result.Put()))) {
        UiLog(L"UI_DIRECTORY_PICK_FAILED", L"读取目录选择结果失败 hresult=" + std::to_wstring((LONG)hr), 2);
        Notify(L"无法读取所选目录。", true);
        return {};
    }
    PWSTR path = nullptr;
    hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
    std::wstring selected = SUCCEEDED(hr) && path ? path : L"";
    CoTaskMemFree(path);
    if (selected.empty()) {
        UiLog(L"UI_DIRECTORY_REJECTED", L"所选项目没有文件系统目录 hresult=" + std::to_wstring((LONG)hr), 1);
        Notify(L"所选项目不是本地文件夹。", true);
    } else
        UiLog(L"UI_DIRECTORY_SELECTED", L"已选择目录=" + UiLocalDirectory(selected));
    return selected;
}
void App::Pick(int id) {
    if (!serviceStarted && (bootstrapBusy || !bootstrapFailed || !bootstrapConfigValid || stopping)) {
        Notify(L"启动尚在处理，或配置未能安全读取；请先检查配置后重试。", true, false);
        return;
    }
    if (Busy()) {
        UiLog(L"UI_DIRECTORY_REJECTED", L"目录切换或导入被拒绝：已有任务运行", 1);
        Notify(L"请等待当前任务结束后再切换目录。", true);
        return;
    }
    bool importing = id == Import;
    if (importing && status.cacheRoot.empty()) {
        SetPage(3);
        Notify(L"请先选择用于存放备份的缓存文件夹，再导入旧版缓存。", true);
        return;
    }
    if (importing)
        UiLog(L"UI_IMPORT_INTENT", L"导入旧版缓存意图");
    const std::wstring initial = importing        ? legacyCache
                                 : id == PickGame ? status.gameRoot
                                                  : status.cacheRoot;
    const std::wstring title = importing        ? L"选择需要导入的旧版缓存文件夹"
                               : id == PickGame ? L"选择外服英雄联盟 Game 文件夹"
                                                : L"选择缓存存储位置";
    std::wstring picked = PickDirectory(title, initial);
    if (picked.empty())
        return;
    if (!importing) {
        std::wstring normalized, error;
        if (!pathpolicy::NormalizeRoot(picked, normalized, error) ||
            !pathpolicy::ValidateRoot(normalized, error)) {
            Notify(L"不是有效路径：" + error +
                       (id == PickGame ? L" 请选择外服安装目录中的 League of Legends\\Game 文件夹。" : L""),
                   true);
            return;
        }
        if (id == PickGame && StartupOptions().validateGameRoot) {
            normalized = core::GameDetect::Find(normalized, false);
            if (normalized.empty()) {
                Notify(L"不是有效路径：请选择外服安装目录中的 League of Legends\\Game 文件夹。", true);
                return;
            }
        }
        const auto management = (smoke ? root + L"\\settings" : root) + L"\\management-v1";
        const auto& other = id == PickGame ? status.cacheRoot : status.gameRoot;
        if (!pathpolicy::AreDisjoint(management, normalized, error) ||
            (!other.empty() && !pathpolicy::AreDisjoint(normalized, other, error))) {
            Notify(L"不是有效路径：" + error +
                       (id == PickGame ? L" 请选择与缓存及管理目录隔离的外服 Game 文件夹。" : L""),
                   true);
            return;
        }
        picked = std::move(normalized);
    }
    std::wstring question;
    if (importing)
        question = L"从以下目录导入支持的游戏缓存：\r\n\r\n" + picked +
                   L"\r\n\r\n仅导入 saves、shards 和受支持的 BOT "
                   L"文件，不执行文件，也不删除目标中的其他文件。\r\n是否继续？";
    else if (id == PickGame)
        question = L"将游戏目录设置为：\r\n\r\n" + picked +
                   L"\r\n\r\n服务仅按文件布局作启发式检查，不代表 Riot 签名或来源认证。"
                   L"此操作不会自动恢复或移动文件。\r\n是否继续？";
    else if (id == PickCache)
        question = L"将缓存位置切换到：\r\n\r\n" + picked +
                   L"\r\n\r\n直接使用所选文件夹，不再套一层缓存目录；仅处理 saves、shards 和六个根业务文件，"
                   L"无关内容保留；同名业务文件会按规则覆盖/删除；旧文件不会自动搬移。\r\n是否继续？";
    else
        question = L"将游戏目录设置为：\r\n\r\n" + picked;
    if (!Confirm(importing ? L"确认导入缓存" : L"确认更改目录", question) || stopping)
        return;
    if (!serviceStarted) {
        QueueRootRecovery(id, picked, config);
        return;
    }
    Request(importing        ? L"import_cache"
            : id == PickGame ? L"set_game_root"
                             : L"set_cache_root",
            {{"path", Utf8(picked)}},
            importing ? L"旧版缓存已安全导入。" : L"目录已保存，显示的是实际生效路径。");
}
void App::Send(int id) {
    if (id >= NavFirst && id < NavFirst + PageCount) {
        SetPage(id - NavFirst);
        return;
    }
    if (id == WinMin) {
        ShowWindow(hwnd, SW_MINIMIZE);
        return;
    }
    if (id == WinMax) {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return;
    }
    if (id == WinClose) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (stopping)
        return;
    if (id == QuickRose || id == QuickAkari || id == QuickAbc) {
        // Use the identical Tool Center launch path, including the native smoke guard.
        SendTool(ToolFirst + (id == QuickAbc ? 2 : id == QuickAkari ? 1 : 0) * ToolStride);
        return;
    }
    if (id >= ToolFirst && id < ToolFirst + VisibleToolCount * ToolStride &&
        ((id - ToolFirst) % ToolStride == 0 || (id - ToolFirst) % ToolStride == 3)) {
        SendTool(id);
        return;
    }
    if (id == ThemeQuick && smoke) {
        UiLog(L"UI_THEME_INTENT", L"隔离预览：快捷切换主题");
        smokeTheme = !smokeTheme;
        light = smokeTheme;
        ApplyTheme();
        UpdateControls(true);
        Notify(L"测试预览主题已切换。", false, false);
        return;
    }
    if (id == ThemeQuick) {
        UiLog(L"UI_THEME_INTENT", std::wstring(L"快捷切换主题意图：") + (light ? L"深色" : L"浅色"));
        if (SettingsEdited()) {
            UiLog(L"UI_THEME_REJECTED", L"主题切换拒绝：设置页仍有未保存修改", 1);
            Notify(L"请先保存设置页中的修改，再快捷切换主题。", true);
            return;
        }
        Request(L"save_settings", {{"theme", light ? 0 : 1}}, L"界面主题已更新。");
    } else if (id == Backup) {
        UiLog(L"UI_BACKUP_INTENT", L"手动备份意图");
        if (!status.gameOk || status.gameRoot.empty() || status.cacheRoot.empty()) {
            UiLog(L"UI_BACKUP_REJECTED", L"手动备份拒绝：游戏或缓存目录尚未配置/无效", 1);
            SetPage(3);
            Notify(L"请先在「设置」选择有效的游戏 Game 目录和缓存文件夹。", true);
            return;
        }
        if (Busy()) {
            UiLog(L"UI_BACKUP_REJECTED", L"手动备份拒绝：已有任务运行", 1);
            return;
        }
        if (Confirm(L"确认立即备份", L"立即执行 Game → 当前缓存目录的备份？同名文件会被覆盖。\r\n\r\n"
                                     L"仅复制受管配置文件，不会向 Game 还原旧缓存。\r\n\r\n"
                                     L"文件在后台处理，可在「备份任务」查看进度或取消。")) {
            SetPage(1);
            Request(L"backup_now", json::object(), L"备份已完成。");
        }
    } else if (id == Restore) {
        UiLog(L"UI_RESTORE_INTENT", L"手动恢复意图");
        if (!status.gameOk || status.gameRoot.empty() || status.cacheRoot.empty()) {
            SetPage(3);
            Notify(L"请先在「设置」选择有效的游戏 Game 目录和缓存文件夹。", true);
            return;
        }
        if (Busy()) {
            UiLog(L"UI_RESTORE_REJECTED", L"手动恢复拒绝：目录无效或已有任务运行", 1);
            return;
        }
        if (Confirm(L"确认恢复到游戏",
                    L"恢复会将当前缓存中的受管配置直接复制到游戏目录，同名文件会被覆盖。"
                    L"\r\n\r\n不会等待游戏退出，也不执行内容校验；失败时可能留下部分文件。确定恢复？"))
            Request(L"restore_now", json::object(), L"恢复已完成。");
    } else if (id == Cancel) {
        UiLog(L"UI_CANCEL_INTENT", L"用户请求取消任务");
        Request(L"cancel_op", json::object(), L"取消请求已送达，任务将在安全边界停止。");
    } else if (id == Auto) {
        if (!status.autoOn && (!status.gameOk || status.gameRoot.empty() || status.cacheRoot.empty())) {
            SetPage(3);
            Notify(L"请先在「设置」选择有效的游戏 Game 目录和缓存文件夹，再开启大厅联动。", true);
            return;
        }
        UiLog(L"UI_AUTO_INTENT", std::wstring(L"大厅联动切换意图：") + (status.autoOn ? L"关闭" : L"开启"));
        Request(L"set_auto", {{"enabled", !status.autoOn}},
                status.autoOn ? L"大厅联动已暂停。" : L"大厅联动已开启。");
    } else if (id == PickGame || id == PickCache || id == Import)
        Pick(id);
    else if (id == OpenGame)
        Open(status.gameRoot);
    else if (id == OpenCache)
        Open(status.cacheRoot);
    else if (id == OpenLogs)
        Open((smoke ? root + L"\\settings" : root) + L"\\logs");
    else if (id == OldCache)
        Open(legacyCache);
    else if (id == GoSettings)
        SetPage(3);
    else if (id == OpenDataRoot) {
        if (!smoke) Open(root);
        else Notify(L"隔离预览不打开外部窗口。", false, false);
    }
    else if (id == OpenManual)
        ShowManual();
    else if (id == About)
        ShowAbout();
    else if (id == RefreshLogs) {
        UiLog(L"UI_LOG_REFRESH_INTENT", L"用户点击刷新日志");
        RequestLogs();
    } else if (id == Save)
        SaveSettings();
}
void App::Queue(std::function<void()> action) {
    {
        std::lock_guard<std::mutex> lock(workMutex);
        if (workerFinish)
            return;
        work.push_back(std::move(action));
    }
    workCv.notify_one();
}
void App::PostResult(std::unique_ptr<UiResult> result) {
    if (PostMessageW(hwnd, UiResultMessage, 0, reinterpret_cast<LPARAM>(result.get())))
        result.release();
}
void App::Open(const std::wstring& path) {
    UiLog(L"UI_FOLDER_OPEN_REQUEST", L"打开文件夹请求 目录=" + UiLocalDirectory(path));
    if (path.empty()) {
        UiLog(L"UI_FOLDER_OPEN_FAILED", L"打开文件夹拒绝：目录尚未配置", 1);
        Notify(L"尚未配置此目录。", true);
        return;
    }
    Notify(L"正在打开文件夹…", false, false);
    Queue([this, path] {
        auto result = std::make_unique<UiResult>();
        result->kind = UiResult::Opened;
        HINSTANCE opened = ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        INT_PTR code = reinterpret_cast<INT_PTR>(opened);
        result->ok = code > 32;
        UiLog(result->ok ? L"UI_FOLDER_OPEN_SUCCEEDED" : L"UI_FOLDER_OPEN_FAILED",
              std::wstring(result->ok ? L"文件夹打开请求已交给系统" : L"Shell打开文件夹失败") +
                  L" shell_code=" + std::to_wstring(code) + L" 目录=" + UiLocalDirectory(path),
              result->ok ? 0 : 2);
        result->message = result->ok
                              ? L"文件夹已打开。"
                              : L"无法打开文件夹，请检查路径和权限（Shell " + std::to_wstring(code) + L"）。";
        PostResult(std::move(result));
    });
}
void App::QueueWelcome() {
    if (welcomeShown || welcomePending || (smoke && !previewWelcome) || stopping || !serviceStarted ||
        (status.gameOk && !status.gameRoot.empty() && !status.cacheRoot.empty()) || !windowShown ||
        !uiReady || !hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || modalWindow || trayPopup)
        return;
    welcomePending = PostMessageW(hwnd, WelcomeCheck, 0, 0) != FALSE;
}
void App::MaybeShowWelcome() {
    welcomePending = false;
    if (welcomeShown || (smoke && !previewWelcome) || stopping || !serviceStarted ||
        (status.gameOk && !status.gameRoot.empty() && !status.cacheRoot.empty()) ||
        !windowShown || !uiReady || !hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || modalWindow ||
        trayPopup)
        return;
    // This runs after RevealWindow completed a synchronous first paint. Refresh never opens a modal.
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    welcomeShown = true;
    const bool goPick = Confirm(L"外服 LOL 专用工具",
                                L"本程序只支持外服（Riot）客户端，不会识别或操作国服/腾讯客户端。\r\n\r\n"
                                L"首次使用请到「设置」选好以下两个目录：\r\n"
                                L"1. 游戏目录：外服安装目录中的 League of Legends\\Game 文件夹。\r\n"
                                L"2. 缓存文件夹：用来存放备份；已有缓存请选择原来的缓存文件夹。\r\n\r\n"
                                L"提示：切换缓存文件夹不会自动搬迁旧文件，请先确认里面的内容。\r\n\r\n"
                                L"现在前往「设置」选择游戏目录和缓存文件夹吗？");
    if (goPick && !stopping && hwnd && IsWindow(hwnd)) {
        SetPage(3);
        SetFocus(Item(GameEdit));
    }
}
void App::RevealWindow() {
    if (windowShown || stopping || !hwnd || !IsWindow(hwnd))
        return;
    // Arrange all child HWNDs while the parent is still hidden. Mask only the FIRST compositor pass;
    // no fade/animation and no waiting for a timer with alpha zero that could strand an invisible UI.
    uiReady = true;
    Layout();
    const LONG_PTR originalStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    bool masked = false;
    if (!(originalStyle & WS_EX_LAYERED)) {
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, originalStyle | WS_EX_LAYERED);
        const bool changed = previous != 0 || GetLastError() == ERROR_SUCCESS;
        masked = changed && SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        if (changed && !masked)
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, originalStyle);
    }
    ShowWindow(hwnd, initialShow);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (masked) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, originalStyle);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    windowShown = true;
    SetPropW(hwnd, L"BBS.FirstFrameReady", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    QueueWelcome();
}
void App::StartWorker() {
    worker = std::thread([this] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock<std::mutex> lock(workMutex);
                workCv.wait(lock, [this] { return workerFinish || !work.empty(); });
                if (work.empty() && workerFinish)
                    break;
                action = std::move(work.front());
                work.pop_front();
            }
            try {
                action();
            } catch (const std::exception& ex) {
                auto r = std::make_unique<UiResult>();
                UiLog(L"UI_WORKER_FAILED", L"UI后台操作异常；不记录异常原文", 2);
                r->message = L"后台操作失败：" + Wide(ex.what());
                PostResult(std::move(r));
            } catch (...) {
                auto r = std::make_unique<UiResult>();
                UiLog(L"UI_WORKER_FAILED", L"UI后台操作发生未知异常", 2);
                r->message = L"后台操作遇到异常，详情请查看日志。";
                PostResult(std::move(r));
            }
        }
        if (SUCCEEDED(com))
            CoUninitialize();
        PostMessageW(hwnd, WorkerStopped, 0, 0);
    });
    const auto fontDiagnostic = iconRenderDiagnostic;
    Queue([this, fontDiagnostic] { BootService(fontDiagnostic); });
}
core::Service::StartOptions App::StartupOptions() const {
    core::Service::StartOptions options;
    options.enableAutodetect = false;
    if (smoke) {
        options.allowAutostart = false;
        options.enableMonitoring = false;
        options.enableScheduling = false;
        options.validateGameRoot = false;
    }
    return options;
}
void App::EffectiveStartupRoots(core::Config&) const {
    // Paths are explicit user choices; never synthesize missing startup roots.
}
void App::ShowFailedConfig() {
    status = core::Service::Status{}; // Never claim the failed backend has validated this Game.
    if (bootstrapConfigValid) {
        auto display = config;
        EffectiveStartupRoots(display);
        status.gameRoot = display.gameRoot;
        status.cacheRoot = display.cacheRoot;
        status.theme = config.theme;
        status.closeAction = config.closeAction;
        status.syncMinutes = config.syncMinutes;
        status.autoOn = config.autoOn;
        status.autostart = config.autostart;
    }
}
bool App::RetryStartup() {
    if (stopping || bootstrapBusy || !bootstrapFailed || serviceStarted)
        return false;
    bootstrapBusy = true; // Set on UI thread BEFORE enqueue: F5/button repeats cannot duplicate it.
    UiLog(L"UI_START_RETRY", L"用户请求重新读取配置并重试启动；不覆盖配置");
    Notify(L"正在重新验证配置与目录并重试启动…", false, false);
    EnableControls();
    UpdateControls();
    const auto diagnostic = iconRenderDiagnostic;
    Queue([this, diagnostic] { BootService(diagnostic); });
    return true;
}
void App::QueueRootRecovery(int id, const std::wstring& path, const core::Config& approved) {
    if (stopping || bootstrapBusy || serviceStarted || !bootstrapFailed || !bootstrapConfigValid ||
        (id != PickGame && id != PickCache))
        return;
    bootstrapBusy = true;
    UiLog(L"UI_BOOTSTRAP_ROOT_BEGIN",
          L"服务未启动：验证所选目录 role=" + std::to_wstring(id) + L" 目录=" + UiLocalDirectory(path));
    Notify(L"正在验证目录并保存所选路径，随后重试启动…", false, false);
    UpdateControls();
    const auto diagnostic = iconRenderDiagnostic;
    Queue([this, id, path, approved, diagnostic] { RecoverRoot(id, path, approved, diagnostic); });
}
void App::RecoverRoot(int id, const std::wstring& path, const core::Config& approved,
                      const std::wstring& diagnostic) {
    auto result = std::make_unique<UiResult>();
    result->kind = UiResult::Recovery;
    auto fail = [&](const std::wstring& message) {
        result->message = message;
        UiLog(L"UI_BOOTSTRAP_ROOT_FAILED", L"启动前目录恢复未提交或未完成；请关联目录验证结果", 1);
        PostResult(std::move(result));
    };
    auto cancelled = [this] {
        return startupRepairCancelled.load() || core::Service::Instance().IsRunning();
    };
    try {
        if (cancelled()) {
            fail(L"正在退出或服务状态已变化，已取消目录恢复。");
            return;
        }
        const auto settings = smoke ? root + L"\\settings" : root;
        if (!LoadStartupConfig(settings, result->config)) {
            fail(L"配置未能安全读取，原文件保持不变；请修复后按 F5 重试。");
            return;
        }
        result->configValid = true;
        const auto expected = result->config;
        if (expected.gameRoot != approved.gameRoot || expected.cacheRoot != approved.cacheRoot) {
            fail(L"目录配置已被外部修改，本次未保存；请核对显示的最新目录后重新选择。");
            return;
        }
        core::Config beforeEffective = expected;
        EffectiveStartupRoots(beforeEffective);
        core::Config next = beforeEffective;
        std::wstring selected, error;
        if (!pathpolicy::NormalizeRoot(path, selected, error)) {
            fail(L"不是有效路径：所选目录不安全，未修改配置。 " + error);
            return;
        }
        if (id == PickGame) {
            if (StartupOptions().validateGameRoot) {
                selected = core::GameDetect::Find(selected, false); // Explicit path only, never scan drives.
                if (selected.empty()) {
                    fail(L"不是有效路径：请选择外服安装目录中的 League of Legends\\Game 文件夹，未修改配置。");
                    return;
                }
            }
            next.gameRoot = selected;
        } else if (id == PickCache)
            next.cacheRoot = selected;
        else {
            fail(L"此恢复入口只允许明确选择游戏或缓存目录。");
            return;
        }
        const auto management = settings + L"\\management-v1";
        auto validRoots = [&] {
            return pathpolicy::ValidateRoot(management, error) &&
                   (next.cacheRoot.empty() || (pathpolicy::ValidateRoot(next.cacheRoot, error) &&
                                              pathpolicy::AreDisjoint(management, next.cacheRoot, error))) &&
                   (next.gameRoot.empty() || (pathpolicy::ValidateRoot(next.gameRoot, error) &&
                                             pathpolicy::AreDisjoint(management, next.gameRoot, error))) &&
                   (next.gameRoot.empty() || next.cacheRoot.empty() ||
                    pathpolicy::AreDisjoint(next.gameRoot, next.cacheRoot, error));
        };
        if (!validRoots() || cancelled()) {
            fail(L"不是有效路径：目录隔离检查未通过或操作已取消；原配置未修改。 " + error);
            return;
        }
        {
            if (id == PickCache && !fileops::EnsureDir(selected)) {
                const DWORD code = GetLastError();
                fail(L"无法创建所选缓存目录；原配置未修改。 " + WinError(code) + L" Win32=" +
                     std::to_wstring(code));
                return;
            }
            if (!validRoots() || cancelled()) {
                fail(L"不是有效路径：保存前目录检查未通过或操作已取消；原配置未修改。 " + error);
                return;
            }
            BootstrapRootEdit edit{expected, id, selected, &startupRepairCancelled};
            if (!core::Config::UpdateAtomic(settings + L"\\config.json", ApplyBootstrapRoot, &edit)) {
                fail(edit.conflict
                         ? L"配置目录已被其他写入者修改；本次未覆盖，请核对后重新选择。"
                         : L"目录配置保存失败或已取消；原文件未被默认值覆盖。 " + core::Config::LastError());
                return;
            }
        }
        UiLog(L"UI_BOOTSTRAP_ROOT_SAVED", L"启动前目录验证与单字段原子保存完成；开始串行启动重试");
        if (startupRepairCancelled.load()) {
            fail(L"目录已保存，但已请求安全退出，不会重新启动服务。");
            return;
        }
        BootService(diagnostic);
    } catch (...) {
        fail(L"目录恢复遇到异常，已停止处理；请检查日志并按 F5 重试。");
    }
}
void App::BootService(const std::wstring& fontDiagnostic) {
    // Startup and retries stay serialized on the existing UI worker.
    auto result = std::make_unique<UiResult>();
    result->kind = UiResult::Boot;
    try {
        const std::wstring settings = smoke ? root + L"\\settings" : root;
        const auto options = StartupOptions();
        if (startupRepairCancelled.load()) {
            result->message = L"已请求安全退出，已取消启动。";
            PostResult(std::move(result));
            return;
        }
        const bool configLoaded = LoadStartupConfig(settings, result->config);
        result->configValid = configLoaded;
        if (!configLoaded) {
            result->message = L"配置未能安全读取，已拒绝覆盖；请检查文件、权限或从备份恢复后按 F5 重试。";
            UiLog(L"UI_CONFIG_LOAD_REJECTED", L"配置读取失败；未由UI覆盖原配置", 2);
            PostResult(std::move(result));
            return;
        }
        bool legacyProbeFailed = false;
        if (!smoke) {
            const std::wstring desktop = KnownFolder(FOLDERID_Desktop);
            if (!desktop.empty()) {
                const std::wstring old = desktop + L"\\缓存";
                const DWORD attr = GetFileAttributesW(old.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                    result->legacy = old;
                else if (attr == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND &&
                         GetLastError() != ERROR_PATH_NOT_FOUND)
                    legacyProbeFailed = true;
            }
        }
        // Service owns logger initialization and all normal startup validation/scheduling options.
        if (startupRepairCancelled.load()) {
            result->message = L"已请求安全退出，已取消服务启动重试。";
            UiLog(L"UI_START_CANCELLED", L"服务重试开始前收到退出请求；不再重启", 1);
            PostResult(std::move(result));
            return;
        }
        result->ok = core::Service::Instance().StartWithOptions(hwnd, settings, L"", options);
        UiLog(L"UI_START", std::wstring(smoke ? L"原生界面隔离测试启动摘要" : L"原生界面启动摘要") +
                               L" 应用目录=" + UiLocalDirectory(settings));
        core::Log().Info(L"ui-assets", assets.Diagnostics());
        core::Log().Info(L"ui-glyphs", fontDiagnostic);
        if (legacyProbeFailed)
            UiLog(L"UI_LEGACY_CACHE_PROBE_FAILED", L"旧桌面缓存检测失败，仍可手动选择导入", 1);
        if (!result->ok) {
            const auto reason = core::Service::Instance().GetStatus().lastError;
            if (result->message.empty())
                result->message =
                    reason.empty() ? L"本地服务启动失败，请检查运行日志。" : L"本地服务启动失败：" + reason;
            UiLog(L"UI_START_FAILED", L"本地服务启动失败；请关联SERVICE日志排查", 2);
        } else if (result->message.empty())
            result->message = L"本地服务已就绪。";
    } catch (...) {
        result->ok = false;
        result->message = L"本地服务启动遇到异常，已停止处理；请检查日志。";
        UiLog(L"UI_START_FAILED", L"启动后台流程异常；不记录配置或异常原文", 2);
    }
    PostResult(std::move(result));
}
WinRect App::Placement() const {
    WINDOWPLACEMENT p{};
    p.length = sizeof(p);
    if (GetWindowPlacement(hwnd, &p))
        return {p.rcNormalPosition.left, p.rcNormalPosition.top,
                p.rcNormalPosition.right - p.rcNormalPosition.left,
                p.rcNormalPosition.bottom - p.rcNormalPosition.top};
    RECT r{};
    GetWindowRect(hwnd, &r);
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}
void App::SavePlacement() {
    if (stopping || !serviceStarted)
        return;
    WinRect r = Placement();
    std::wstring path = (smoke ? root + L"\\settings" : root) + L"\\config.json";
    Queue([this, r, path] {
        WinRect value = r;
        if (!core::Config::UpdateAtomic(path, ApplyWinRect, &value)) {
            auto result = std::make_unique<UiResult>();
            result->message = L"窗口位置未能保存，请检查配置文件权限。";
            PostResult(std::move(result));
        }
    });
}
void App::RestorePlacement(const core::Config& c) {
    if (c.winW < D(940) || c.winH < D(640) || c.winW > 12000 || c.winH > 10000)
        return;
    RECT proposed{};
    if (!window_geometry::TryRect(c.winX, c.winY, c.winW, c.winH, proposed))
        return;
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi))
        return;
    RECT fitted{};
    if (!window_geometry::FitToWorkArea(proposed, mi.rcWork, fitted))
        return;
    SetWindowPos(hwnd, nullptr, fitted.left, fitted.top, fitted.right - fitted.left,
                 fitted.bottom - fitted.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
void App::Shutdown(const wchar_t* reason) {
    if (stopping)
        return;
    UiLog(L"UI_EXIT_REQUESTED", L"请求安全退出 原因=" + std::wstring(reason));
    stopping = true;
    startupRepairCancelled = true;
    if (licenseWindow)
        DestroyWindow(licenseWindow);
    stopSince = GetTickCount64();
    KillTimer(hwnd, SmokeTimer);
    KillTimer(hwnd, CaptureTimer);
    ShowWindow(hwnd, SW_RESTORE);
    EnableControls();
    InvalidateRect(hwnd, nullptr, FALSE);
    // PostCommand only signals/queues: never wait for cancellation on the window thread.
    if (serviceStarted) {
        const int id = requestId++;
        {
            core::ScopedLogContext trace(L"request:" + std::to_wstring(id));
            UiLog(L"UI_REQUEST_SENT", L"退出前请求取消任务 " + UiRequestLabel(L"cancel_op", id));
            core::Service::Instance().PostCommand(id, L"cancel_op", L"{}");
        }
        core::Service::Instance().RequestStop();
    }
    WinRect r = Placement();
    std::wstring cfg = (smoke ? root + L"\\settings" : root) + L"\\config.json";
    {
        std::lock_guard<std::mutex> lock(workMutex);
        work.clear();
        work.push_back([this, r, cfg] {
            // A close during startup is serialized after StartWithOptions, so no Start/Stop race.
            core::Service::Instance().PostCommand(0, L"cancel_op", L"{}");
            core::Service::Instance().RequestStop();
            // The manager cancels and joins its payload worker here, never on the window thread.
            toolManager.Shutdown();
            WinRect value = r;
            if (!core::Config::UpdateAtomic(cfg, ApplyWinRect, &value)) {
                UiLog(L"UI_PLACEMENT_SAVE_FAILED", L"退出时窗口位置保存失败", 1);
                auto result = std::make_unique<UiResult>();
                result->message = L"退出时窗口位置未能保存，请检查配置文件。";
                PostResult(std::move(result));
            }
            UiLog(L"UI_EXIT_WAITING", L"安全退出：等待后台文件任务结束");
            core::Service::Instance().Stop();
            serviceStopComplete = true;
            UiLog(L"UI_EXIT_SAFE", L"后台服务已安全结束");
        });
        workerFinish = true;
    }
    workCv.notify_one();
}
void App::ClosePolicy() {
    if (stopping)
        return;
    // Ask about edited-but-unsaved settings before deciding how to close. Choosing "save and
    // continue" defers the close until the service acknowledges the save.
    if (!GuardUnsaved(-1, true))
        return;
    int action = serviceStarted ? status.closeAction : 0;
    if (action == 0) {
        const int answer = AskCloseDialog(); // 0 cancel, 1 tray, 2 exit
        if (answer == 0)
            return;
        action = answer;
    }
    if (action == 1) {
        if (!trayAdded) {
            UiLog(L"UI_TRAY_HIDE_REJECTED", L"托盘隐藏拒绝：图标不可用", 1);
            Notify(L"托盘图标不可用，为避免窗口丢失，请在设置中选择退出程序。", true);
            return;
        }
        UiLog(L"UI_TRAY_HIDDEN", L"关闭策略：隐藏到托盘");
        ShowWindow(hwnd, SW_HIDE);
        TrayBalloon(L"已隐藏到托盘，右键图标可打开窗口或安全退出。", false);
    } else
        Shutdown(L"关闭窗口策略：安全退出");
}
bool App::InitTray() {
    if (!trayMenu) {
        trayMenu = CreatePopupMenu();
        if (!trayMenu) {
            Notify(L"无法初始化托盘菜单。", true, false);
            return false;
        }
        bool ok = AppendMenuW(trayMenu, MF_STRING, TrayShow, L"打开 BBS自动工具") &&
                  AppendMenuW(trayMenu, MF_SEPARATOR, 0, nullptr) &&
                  AppendMenuW(trayMenu, MF_STRING, Backup, L"立即备份…") &&
                  AppendMenuW(trayMenu, MF_STRING, Restore, L"恢复到游戏…") &&
                  AppendMenuW(trayMenu, MF_STRING, TrayAuto, L"大厅联动") &&
                  AppendMenuW(trayMenu, MF_SEPARATOR, 0, nullptr) &&
                  AppendMenuW(trayMenu, MF_STRING, OpenCache, L"打开缓存文件夹") &&
                  AppendMenuW(trayMenu, MF_STRING, OpenLogs, L"打开日志文件夹") &&
                  AppendMenuW(trayMenu, MF_STRING, GoSettings, L"设置") &&
                  AppendMenuW(trayMenu, MF_SEPARATOR, 0, nullptr) &&
                  AppendMenuW(trayMenu, MF_STRING, TrayExit, L"安全退出");
        if (!ok) {
            DestroyMenu(trayMenu);
            trayMenu = nullptr;
            Notify(L"托盘菜单创建失败。", true, false);
            return false;
        }
    }
    tray = {};
    tray.cbSize = sizeof(tray);
    tray.hWnd = hwnd;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    tray.uCallbackMessage = TrayEvent;
    tray.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!tray.hIcon)
        tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(tray.szTip, L"BBS自动工具 · QQ1121003655",
              static_cast<int>(sizeof(tray.szTip) / sizeof(wchar_t)));
    trayAdded = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
    if (!trayAdded) {
        Notify(L"托盘图标创建失败，关闭窗口时不会隐藏。", true, false);
        return false;
    }
    tray.uVersion = NOTIFYICON_VERSION_4;
    trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &tray) != FALSE;
    if (!trayVersion4)
        Notify(L"托盘使用系统兼容模式。", false, false);
    return true;
}
void App::TrayBalloon(const std::wstring& message, bool error) {
    if (!trayAdded)
        return;
    NOTIFYICONDATAW info = tray;
    info.uFlags = NIF_INFO;
    lstrcpynW(info.szInfoTitle, error ? L"BBS自动工具 · 需要留意" : L"BBS自动工具",
              static_cast<int>(sizeof(info.szInfoTitle) / sizeof(wchar_t)));
    lstrcpynW(info.szInfo, message.c_str(), static_cast<int>(sizeof(info.szInfo) / sizeof(wchar_t)));
    info.dwInfoFlags = error ? NIIF_WARNING : NIIF_INFO;
    if (!Shell_NotifyIconW(NIM_MODIFY, &info)) {
        notice = L"系统通知发送失败，请打开窗口查看状态。";
        noticeUntil = GetTickCount64() + 15000;
        errorNotice = true;
    }
}
struct TrayMenuItem {
    UINT id;
    const wchar_t* text;
    bool separator = false;
    bool enabled = true;
    bool checked = false;
};
struct TrayMenuState {
    App* app;
    const std::vector<TrayMenuItem>* items;
    HWND window = nullptr;
    int width = 0, height = 0, hot = -1, pressed = -1;
    bool activated = false;
    UINT chosen = 0;
    bool closing = false;
    bool trackingMouse = false;
    bool syntheticInput = false; // Explicitly enabled only for an isolated smoke popup.
    std::unique_ptr<Buffer> frame;
    int frameWidth = 0, frameHeight = 0;
};
void App::ShowTrayMenu() {
    if (stopping)
        return;
    if (modalWindow && IsWindow(modalWindow)) {
        SetForegroundWindow(modalWindow);
        return;
    }
    if (trayPopup && IsWindow(trayPopup)) {
        // A genuine second click on the icon toggles the popup; repeated notifications of the
        // SAME click must not tear it down again.
        if (GetTickCount64() - trayMenuShownAt > 250)
            DestroyWindow(trayPopup);
        return;
    }
    if (!trayAdded && !InitTray())
        return;
    Refresh();
    const bool ready = serviceStarted && !stopping && !Busy();
    const bool rootsReady = status.gameOk && !status.gameRoot.empty() && !status.cacheRoot.empty();
    std::vector<TrayMenuItem> items = {
        {TrayShow, L"打开 BBS自动工具", false, !stopping},
        {0, L"", true},
        {Backup, L"立即备份…", false, ready && rootsReady},
        {Restore, L"恢复到游戏…", false, ready && rootsReady},
        {TrayAuto, L"大厅联动", false,
         serviceStarted && !stopping && !HasPending(L"set_auto") && (status.autoOn || rootsReady), status.autoOn},
        {0, L"", true},
        {OpenCache, L"打开缓存文件夹", false, !stopping && !status.cacheRoot.empty()},
        {OpenLogs, L"打开日志文件夹", false, !stopping},
        {GoSettings, L"设置", false, !stopping},
        {0, L"", true},
        {TrayExit, L"安全退出", false, true}};
    TrayMenuState state{};
    state.app = this;
    state.items = &items;
    const int itemHeight = D(23), separatorHeight = D(7), padding = D(4);
    state.width = D(184);
    for (const auto& item : items)
        state.height += item.separator ? separatorHeight : itemHeight;
    state.height += padding * 2;
    static const wchar_t* klass = L"BBS.TrayMenu";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayMenuProc;
    wc.hInstance = instance;
    wc.lpszClassName = klass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;
    POINT point{};
    GetCursorPos(&point);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    const RECT work = monitor.rcWork;
    const int x = std::max((int)work.left, std::min((int)point.x, (int)(work.right - state.width)));
    const int y = std::max((int)work.top, std::min((int)point.y, (int)(work.bottom - state.height)));
    SetForegroundWindow(hwnd);
    HWND menu = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, klass, L"", WS_POPUP, x, y, state.width,
                                state.height, hwnd, nullptr, instance, &state);
    if (!menu)
        return;
    trayPopup = menu;
    trayMenuShownAt = GetTickCount64();
    ShowWindow(menu, SW_SHOW);
    SetForegroundWindow(menu);
    // Unlike a system popup, this custom window has no built-in menu-mode dismissal. Capture makes
    // outside mouse clicks reach us even if the shell never activated this popup.
    if (state.window == menu) {
        SetCapture(menu);
        UpdateWindow(menu);
    }
    bool quit = false, pumpFailed = false;
    int quitCode = 0;
    while (state.window && IsWindow(state.window)) {
        MSG message{};
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            quit = received == 0;
            pumpFailed = received == -1;
            quitCode = quit ? static_cast<int>(message.wParam) : 1;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state.closing = true;
    if (GetCapture() == menu)
        ReleaseCapture();
    if (state.window)
        DestroyWindow(state.window);
    if (trayPopup == menu)
        trayPopup = nullptr;
    if (quit) {
        PostQuitMessage(quitCode);
        return;
    }
    if (pumpFailed) {
        UiLog(L"UI_TRAY_MENU_FAILED", L"托盘菜单消息循环失败", 2);
        Shutdown(L"托盘菜单消息循环失败");
        return;
    }
    if (stopping || !hwnd || !IsWindow(hwnd))
        return;
    const UINT chosen = state.chosen;
    if (chosen == TrayShow) {
        UiLog(L"UI_TRAY_WAKE", L"托盘菜单唤醒窗口");
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    } else if (chosen == TrayExit) {
        UiLog(L"UI_TRAY_EXIT", L"托盘菜单请求安全退出");
        Shutdown(L"托盘菜单退出");
    } else if (chosen == TrayAuto)
        Send(Auto);
    else if (chosen) {
        if (chosen == Backup || chosen == Restore || chosen == GoSettings) {
            UiLog(L"UI_TRAY_WAKE", L"托盘操作唤醒窗口");
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        Send(chosen);
    }
}
LRESULT CALLBACK App::TrayMenuProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<TrayMenuState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<TrayMenuState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state || !state->app)
        return DefWindowProcW(window, msg, wp, lp);
    App* app = state->app;
    const int itemHeight = app->D(23), separatorHeight = app->D(7), padding = app->D(4);
    auto indexAt = [&](POINT point) {
        if (point.x < padding || point.x >= state->width - padding)
            return -1;
        int y = padding;
        for (int i = 0; i < (int)state->items->size(); ++i) {
            const auto& item = (*state->items)[i];
            const int h = item.separator ? separatorHeight : itemHeight;
            if (point.y >= y && point.y < y + h)
                return i;
            y += h;
        }
        return -1;
    };
    auto rowRect = [&](int index) {
        int y = padding;
        for (int i = 0; i < index; ++i)
            y += (*state->items)[i].separator ? separatorHeight : itemHeight;
        return RECT{app->D(3), y, state->width - app->D(3), y + itemHeight};
    };
    auto setHot = [&](int hot) {
        if (hot < 0 || hot >= static_cast<int>(state->items->size()) || (*state->items)[hot].separator ||
            !(*state->items)[hot].enabled)
            hot = -1;
        if (hot == state->hot)
            return;
        const int previous = state->hot;
        state->hot = hot;
        if (previous >= 0) {
            const RECT row = rowRect(previous);
            InvalidateRect(window, &row, FALSE);
        }
        if (hot >= 0) {
            const RECT row = rowRect(hot);
            InvalidateRect(window, &row, FALSE);
        }
    };
    auto dismiss = [&] {
        if (state->closing)
            return;
        state->closing = true;
        if (GetCapture() == window)
            ReleaseCapture();
        if (state->window == window)
            DestroyWindow(window);
    };
    switch (msg) {
    case WM_APP + 0x155: {
        if (!app->smoke)
            return 0;
        // SendMessage-based probes do not move the physical cursor. Keep queued real cursor
        // moves/leaves separate; explicit synthetic leave still uses the normal handler below.
        state->syntheticInput = true;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_CANCEL | TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        state->trackingMouse = false;
        return 1;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        if (app->trayPopup == window)
            app->trayPopup = nullptr;
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, msg, wp, lp);
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            // The shell briefly hands activation back to the taskbar right after the click that
            // opened us. A popup that was never activated must not treat that as "user clicked away".
            if (state->activated)
                dismiss();
        } else {
            state->activated = true;
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp && state->activated)
            dismiss();
        return 0;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
    case WM_CLOSE:
        dismiss();
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != window)
            dismiss();
        return 0;
    case WM_MOUSEMOVE: {
        if (state->syntheticInput && InSendMessageEx(nullptr) == ISMEX_NOSEND)
            return 0;
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!state->trackingMouse && !state->syntheticInput) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            state->trackingMouse = TrackMouseEvent(&tracking) != FALSE;
        }
        setHot(indexAt(point));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state->syntheticInput && InSendMessageEx(nullptr) == ISMEX_NOSEND)
            return 0;
        state->trackingMouse = false;
        setHot(-1);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT client{};
        GetClientRect(window, &client);
        if (!PtInRect(&client, point)) {
            dismiss();
            return 0;
        }
        state->pressed = msg == WM_LBUTTONDOWN ? indexAt(point) : -1;
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int index = indexAt(point);
        if (index >= 0 && index == state->pressed) {
            const auto& item = (*state->items)[index];
            if (!item.separator && item.enabled) {
                state->chosen = item.id;
                dismiss();
            }
        }
        state->pressed = -1;
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            dismiss();
        } else if (wp == VK_UP || wp == VK_DOWN || wp == VK_HOME || wp == VK_END) {
            const int count = static_cast<int>(state->items->size());
            const int direction = (wp == VK_UP || wp == VK_END) ? -1 : 1;
            int index = (wp == VK_HOME) ? -1 : (wp == VK_END ? count : state->hot);
            if (index < 0 && direction < 0)
                index = 0;
            for (int n = 0; n < count; ++n) {
                index = (index + direction + count) % count;
                const auto& item = (*state->items)[index];
                if (!item.separator && item.enabled) {
                    setHot(index);
                    break;
                }
            }
        } else if ((wp == VK_RETURN || wp == VK_SPACE) && state->hot >= 0) {
            const auto& item = (*state->items)[state->hot];
            if (!item.separator && item.enabled) {
                state->chosen = item.id;
                dismiss();
            }
        }
        return 0;
    case WM_PAINT: {
        PaintSession paint(window);
        if (!paint.dc)
            return 0;
        RECT rc{};
        GetClientRect(window, &rc);
        if (!state->frame || state->frameWidth != rc.right || state->frameHeight != rc.bottom) {
            state->frame.reset();
            try {
                auto frame = std::make_unique<Buffer>(paint.dc, rc.right, rc.bottom);
                if (frame->dc) {
                    state->frame = std::move(frame);
                    state->frameWidth = rc.right;
                    state->frameHeight = rc.bottom;
                }
            } catch (const std::bad_alloc&) {
                // Keep the menu usable if even the Buffer wrapper cannot be allocated.
            }
        }
        // Normal paints compose every layer offscreen. Allocation failure explicitly falls back
        // to clipped direct drawing; that exceptional path cannot promise atomic presentation.
        HDC target = state->frame ? state->frame->dc : paint.dc;
        HBRUSH background = CreateSolidBrush(app->palette.card);
        if (background) {
            FillRect(target, &rc, background);
            DeleteObject(background);
        }
        app->Round(target, rc, app->palette.card, app->palette.border, 10);
        int y = padding;
        for (int i = 0; i < (int)state->items->size(); ++i) {
            const auto& item = (*state->items)[i];
            if (item.separator) {
                // Menu coordinates are already physical pixels: do not scale them a second time.
                const int lineY = y + separatorHeight / 2;
                RECT line{padding + app->D(4), lineY, state->width - padding - app->D(4), lineY + 1};
                HBRUSH separator = CreateSolidBrush(app->palette.border);
                if (separator) {
                    FillRect(target, &line, separator);
                    DeleteObject(separator);
                }
                y += separatorHeight;
                continue;
            }
            RECT row{app->D(3), y, state->width - app->D(3), y + itemHeight};
            if (i == state->hot && item.enabled)
                app->Round(target, row, app->palette.accentSoft, app->palette.accentSoft, 6);
            app->Text(target, item.text,
                      RECT{row.left + app->D(22), row.top, row.right - app->D(6), row.bottom},
                      (HFONT)app->smallFont.handle, item.enabled ? app->palette.text : app->palette.muted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (item.checked)
                app->Text(target, L"✓",
                          RECT{row.left + app->D(4), row.top, row.left + app->D(19), row.bottom},
                          (HFONT)app->smallFont.handle, app->palette.accent,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            y += itemHeight;
        }
        if (state->frame && !IsRectEmpty(&paint.ps.rcPaint)) {
            const RECT& dirty = paint.ps.rcPaint;
            // BeginPaint retains the exact update-region clip, including disjoint old/new rows.
            BitBlt(paint.dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                   target, dirty.left, dirty.top, SRCCOPY);
        }
        return 0;
    }
    }
    return DefWindowProcW(window, msg, wp, lp);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<App*>(cs->lpCreateParams);
        self->hwnd = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        try {
            return self->Message(msg, wp, lp);
        } catch (const std::exception&) {
            if (!self->uiExceptionReported) {
                self->uiExceptionReported = true;
                UiLog(msg == WM_CREATE ? L"UI_START_FAILED" : L"UI_CALLBACK_FAILED",
                      L"界面消息处理异常 message=" + std::to_wstring(msg), 2);
            }
            if (msg == WM_CREATE) {
                MessageBoxW(window, L"原生界面初始化失败，请检查系统资源。", AppTitle, MB_ICONERROR);
                return -1;
            }
            self->Notify(L"界面操作失败，请查看日志或重试。", true, false);
            return 0;
        } catch (...) {
            if (!self->uiExceptionReported) {
                self->uiExceptionReported = true;
                UiLog(L"UI_CALLBACK_FAILED", L"界面消息处理发生未知异常 message=" + std::to_wstring(msg), 2);
            }
            return DefWindowProcW(window, msg, wp, lp);
        }
    }
    return DefWindowProcW(window, msg, wp, lp);
}
LRESULT App::Message(UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreated && msg == taskbarCreated) {
        trayAdded = false;
        InitTray();
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        using DpiFn = UINT(WINAPI*)(HWND);
        auto getDpi =
            reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (getDpi)
            dpi = (int)getDpi(hwnd);
        else {
            HDC dc = GetDC(hwnd);
            if (dc) {
                dpi = GetDeviceCaps(dc, LOGPIXELSX);
                ReleaseDC(hwnd, dc);
            }
        }
    }
        if (smoke && previewDpi)
            dpi = previewDpi;
        assets.Load(instance);
        MakeControls();
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        InitTray();
        if (!SetTimer(hwnd, RefreshTimer, 500, nullptr))
            Notify(L"无法启动界面状态计时器。", true, false);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINT:
        // Own client/child composition; DefWindowProc would otherwise print children a second time
        // without the fixed page-heading clip. Native WM_PRINT need not honor SetWindowRgn.
    case WM_PRINTCLIENT:
        PrintClient(reinterpret_cast<HDC>(wp));
        return 0;
    case WM_PAINT: {
        PaintSession paint(hwnd);
        if (paint.dc) {
            RECT r{};
            GetClientRect(hwnd, &r);
            if (!paintBuffer || paintBufferWidth != r.right || paintBufferHeight != r.bottom) {
                auto candidate = std::make_unique<Buffer>(paint.dc, r.right, r.bottom);
                if (candidate->dc) {
                    paintBuffer = std::move(candidate);
                    paintBufferWidth = r.right;
                    paintBufferHeight = r.bottom;
                } else {
                    paintBuffer.reset();
                }
            }
            RECT dirty{};
            if (GetClipBox(paint.dc, &dirty) == ERROR || IsRectEmpty(&dirty))
                dirty = r;
            if (smoke) {
                ++uiPerf.paints;
                uiPerf.dirtyPixels += static_cast<uint64_t>(std::max<LONG>(0, dirty.right - dirty.left)) *
                                      static_cast<uint64_t>(std::max<LONG>(0, dirty.bottom - dirty.top));
            }
            HDC target = paintBuffer ? paintBuffer->dc : paint.dc;
            const int saved = SaveDC(target);
            IntersectClipRect(target, dirty.left, dirty.top, dirty.right, dirty.bottom);
            Paint(target);
            if (saved)
                RestoreDC(target, saved);
            if (paintBuffer)
                BitBlt(paint.dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                       target, dirty.left, dirty.top, SRCCOPY);
        }
        return 0;
    }
    case WM_APP + 12:
        EnsureVisible(reinterpret_cast<HWND>(wp));
        return 0;
    case WM_MOUSEWHEEL:
        if (page == 2) {
            // Route to the edit exactly once; it owns fractional wheel accumulation.
            SendMessageW(Item(LogEdit), WM_MOUSEWHEEL, wp, lp);
        } else {
            const int steps = AccumulateWheelTicks(wheelRemainder, GET_WHEEL_DELTA_WPARAM(wp));
            ScrollTo(scrollY - steps * 48);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE: {
        int mx = MulDiv(GET_X_LPARAM(lp), 96, dpi), my = MulDiv(GET_Y_LPARAM(lp), 96, dpi);
        if (msg == WM_LBUTTONDOWN) {
            if (mx >= width - 20 && my >= BodyTop && my < BodyBottom() && MaxScroll() > 0)
                scrollDrag = 1;
            else if (page == 2 && mx >= Left() + ContentWidth() - 36 && mx <= Left() + ContentWidth() - 16 &&
                     my >= 244 && my < height - 80)
                scrollDrag = 2;
            if (scrollDrag)
                SetCapture(hwnd);
        }
        if (scrollDrag == 1) {
            int track = std::max(32, height - 224), visible = height - 208;
            int thumb = std::min(track, std::max(32, track * visible / std::max(1, PageBottom() - 160)));
            ScrollTo((my - 168 - thumb / 2) * MaxScroll() / std::max(1, track - thumb));
        } else if (scrollDrag == 2) {
            int total = (int)SendMessageW(Item(LogEdit), EM_GETLINECOUNT, 0, 0);
            int first = (int)SendMessageW(Item(LogEdit), EM_GETFIRSTVISIBLELINE, 0, 0);
            int visible = std::max(1, (height - 324) / 20), track = std::max(48, height - 344);
            int thumb = std::min(track, std::max(32, track * visible / std::max(visible, total)));
            int target = std::max(0, std::min(std::max(0, total - visible), (my - 248 - thumb / 2) *
                                                                                std::max(0, total - visible) /
                                                                                std::max(1, track - thumb)));
            SendMessageW(Item(LogEdit), EM_LINESCROLL, 0, target - first);
            InvalidateLogScrollBar();
        }
        if (scrollDrag)
            return 0;
        break;
    }
    case WM_LBUTTONUP:
        if (scrollDrag) {
            scrollDrag = 0;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        scrollDrag = 0;
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            Layout();
        return 0;
    case WM_NCCALCSIZE:
        // The app paints its own caption strip inside the client area so it always matches the theme.
        return wp ? 0 : DefWindowProcW(hwnd, msg, wp, lp);
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCPAINT:
        return 0;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT frame{};
        if (GetWindowRect(hwnd, &frame) && !IsZoomed(hwnd)) {
            const int grip = D(6);
            const bool left = point.x < frame.left + grip, right = point.x >= frame.right - grip;
            const bool top = point.y < frame.top + grip, bottom = point.y >= frame.bottom - grip;
            if (top && left)
                return HTTOPLEFT;
            if (top && right)
                return HTTOPRIGHT;
            if (bottom && left)
                return HTBOTTOMLEFT;
            if (bottom && right)
                return HTBOTTOMRIGHT;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
            if (top)
                return HTTOP;
            if (bottom)
                return HTBOTTOM;
            if (point.y < frame.top + D(56))
                return HTCAPTION;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {D(940), D(640)};
        // Only isolated render previews may exceed the host monitor tracking size.
        if (smoke && previewWidth && previewHeight) {
            info->ptMaxTrackSize.x = std::max(info->ptMaxTrackSize.x, static_cast<LONG>(D(previewWidth)));
            info->ptMaxTrackSize.y = std::max(info->ptMaxTrackSize.y, static_cast<LONG>(D(previewHeight)));
        }
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        paintBuffer.reset();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        auto r = reinterpret_cast<RECT*>(lp);
        Fonts();
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_EXITSIZEMOVE:
        SavePlacement();
        return 0;
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_MEASUREITEM: {
        auto measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (measure->CtlType == ODT_COMBOBOX) {
            measure->itemHeight = D(28);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, palette.text);
        SetBkColor(dc, palette.field);
        return reinterpret_cast<LRESULT>(fieldBrush.handle ? fieldBrush.handle : GetStockObject(DC_BRUSH));
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (initialized && !syncingControls && (id == SyncEdit && code == EN_CHANGE))
            settingsDirty = true;
        if (initialized && !syncingControls && id >= ThemeCombo && id <= StartupCombo &&
            code == CBN_SELCHANGE) {
            settingsDirty = true;
            const int selected = static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0));
            const wchar_t* preference = id == ThemeCombo   ? L"主题"
                                        : id == CloseCombo ? L"关闭行为"
                                                           : L"自启动";
            UiLog(L"UI_PREFERENCE_CHANGED", std::wstring(L"分段偏好选择：") + preference + L" 选项=" +
                                                std::to_wstring(selected) + L"（尚未保存）");
        }
        if (code == BN_CLICKED)
            Send(id);
        return 0;
    }
    case WM_TIMER:
        if (wp == SmokeTimer) {
            Shutdown(L"隔离测试计时结束");
            return 0;
        }
        if (wp == CaptureTimer) {
            if (serviceStarted && !captured) {
                KillTimer(hwnd, CaptureTimer);
                Capture();
            }
            return 0;
        }
        if (wp == RefreshTimer) {
            if (!windowShown && GetTickCount64() - startedAt > 1200)
                RevealWindow();
            if (stopping) {
                if (!shutdownWarned && GetTickCount64() - stopSince >= ShutdownWarnMs) {
                    shutdownWarned = true;
                    UiLog(L"UI_EXIT_WAIT_TIMEOUT", L"安全退出等待已超过5秒；继续等待安全边界，不强制结束", 1);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            Drain();
            if (RefreshToolDirectories()) {
                EnableControls();
                if (page == 4)
                    InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (page == 2 && GetTickCount64() - lastLogsRequest > 4000)
                RequestLogs();
            for (auto& pair : pending)
                if (!pair.second.warned && GetTickCount64() - pair.second.sent > 30000) {
                    pair.second.warned = true;
                    {
                        core::ScopedLogContext trace(L"request:" + std::to_wstring(pair.first));
                        if (LoggedUiMethod(pair.second.method))
                            UiLog(L"UI_REQUEST_TIMEOUT",
                                  L"请求等待超过30秒；仍待服务结果 " +
                                      UiRequestLabel(pair.second.method, pair.first),
                                  1);
                        else if ((pair.second.method == L"get_status" || pair.second.method == L"get_logs") &&
                                 !pollFailed[pair.second.method]) {
                            pollFailed[pair.second.method] = true;
                            UiLog(L"UI_POLL_TIMEOUT",
                                  L"自动读取首次等待超过30秒 " +
                                      UiRequestLabel(pair.second.method, pair.first),
                                  1);
                        }
                    }
                    Notify(L"任务仍在后台处理，可继续浏览或请求取消。", false, false);
                }
            if (noticeUntil && GetTickCount64() > noticeUntil) {
                noticeUntil = 0;
                errorNotice = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case ToolEvents:
        if (!stopping) {
            RefreshTools();
            EnableControls();
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        return 0;
    case ServiceEvents:
        Drain();
        return 0;
    case WelcomeCheck:
        MaybeShowWelcome();
        return 0;
    case SingleInstanceWake:
        if (serviceStarted)
            UiLog(L"UI_SINGLE_INSTANCE_WAKE", L"收到另一次启动的单实例唤醒通知");
        else
            ++bootstrapWakeCount; // Flush once after Service opens the logger.
        return 0;
    case WM_APP + 60:
        // Only isolated acceptance instances recognize these test messages.
        if (smoke)
            Shutdown(L"隔离测试退出探针");
        return 0;
    case WM_APP + 67:
        if (!smoke || lp != 0x55495046)
            return 0;
        if (wp == 99)
            return 1;
        switch (wp) {
        case 0:
            uiPerf = {};
            return 1;
        case 1:
            return static_cast<LRESULT>(uiPerf.paints);
        case 2:
            return static_cast<LRESULT>(uiPerf.textDraws);
        case 3:
            return static_cast<LRESULT>(uiPerf.textSkips);
        case 4:
            return static_cast<LRESULT>(uiPerf.logoDraws);
        case 5:
            return static_cast<LRESULT>(uiPerf.directoryQueries);
        case 6:
            return static_cast<LRESULT>(uiPerf.dirtyPixels);
        case 7:
            return static_cast<LRESULT>(uiPerf.layouts);
        }
        return 0;
    case WM_APP + 65:
        // Read-only smoke identity probe: zero means no additional card.
        if (smoke && wp < VisibleToolCount)
            return static_cast<int>(ToolIds[wp]) + 1;
        return 0;
    case WM_APP + 64:
        if (smoke) {
            if (wp == 0) {
                KillTimer(hwnd, 3); // The isolated scroll probe captures explicitly after exercising input.
                RemovePropW(hwnd, L"BBS.ProbeHides");
                RemovePropW(hwnd, L"BBS.ProbeShows");
                RemovePropW(hwnd, L"BBS.ProbeErases");
                SetPropW(hwnd, L"BBS.ScrollProbe", reinterpret_cast<HANDLE>(1));
            } else if (wp == 1)
                return reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"BBS.ProbeHides"));
            else if (wp == 2)
                return reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"BBS.ProbeShows"));
            else if (wp == 3)
                return GetDlgCtrlID(GetFocus());
            else if (wp == 4) {
                SetFocus(Item(PickGame));
                return GetDlgCtrlID(GetFocus());
            } else if (wp == 5)
                return scrollY;
            else if (wp == 6)
                return MaxScroll();
            else if (wp == 7)
                return reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"BBS.ProbeErases"));
        }
        return 0;
    case WM_APP + 63:
        if (smoke && wp <= 6) {
            smokeAutoPreview = static_cast<int>(wp);
            Refresh();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_APP + 62:
        if (smoke && (wp == 0 || wp == 1 || wp == 2)) {
            smokeLobbyPreview = wp == 2 ? -1 : static_cast<int>(wp);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_APP + 61:
        if (smoke && windowShown && !stopping) {
            if (wp == 1)
                AskCloseDialog();
            else if (wp == 2)
                ShowAbout();
            else
                Confirm(L"提示窗口预览",
                        L"这是隔离测试提示，不会备份、恢复或删除游戏文件。\r\n\r\n确定后返回主界面。");
        }
        return 0;
    case UiResultMessage: {
        std::unique_ptr<UiResult> result(reinterpret_cast<UiResult*>(lp));
        if (!result)
            return 0;
        if (result->kind == UiResult::Recovery) {
            bootstrapBusy = false;
            bootstrapConfigValid = result->configValid;
            config = result->config;
            ShowFailedConfig();
            if (!stopping) {
                UpdateControls(true);
                Layout();
            }
        }
        if (result->kind == UiResult::Boot) {
            serviceStarted = result->ok;
            bootstrapBusy = false;
            bootstrapFailed = !result->ok;
            bootstrapConfigValid = result->configValid;
            UiLog(result->ok ? L"UI_READY" : L"UI_START_FAILED",
                  result->ok ? L"UI已收到本地服务就绪结果" : L"UI收到本地服务启动失败结果",
                  result->ok ? 0 : 2);
            if (result->ok) {
                UiLog(L"UI_BOOTSTRAP", L"引导摘要：初始页=" + std::to_wstring(page) + L" 托盘=" +
                                           (trayAdded ? L"可用" : L"不可用") + L" 启动阶段单实例唤醒数=" +
                                           std::to_wstring(bootstrapWakeCount));
                bootstrapWakeCount = 0;
            }
            config = result->config;
            legacyCache = result->legacy;
            if (!serviceStarted)
                ShowFailedConfig();
            lastError = result->ok ? L"" : result->message;
            if (!stopping) {
                if (!smoke || !previewWidth)
                    RestorePlacement(config);
                Refresh();
                UpdateControls(true);
                Layout();
                RevealWindow();
                if (serviceStarted) {
                    Request(L"get_status", json::object(), L"", true);
                    if (page == 2)
                        RequestLogs();
                }
            }
        }
        if (result->kind == UiResult::Notice)
            UiLog(result->ok ? L"UI_ASYNC_SUCCEEDED" : L"UI_ASYNC_FAILED",
                  result->ok ? L"UI后台操作完成" : L"UI后台操作失败；请关联此前事件码", result->ok ? 0 : 2);
        if (!stopping)
            Notify(result->message, !result->ok);
        else if (!result->ok && !result->message.empty()) {
            // Do not open another blocking modal while the owner is being destroyed.
            UiLog(L"UI_EXIT_OPERATION_FAILED", L"安全退出期间有后台操作失败；不记录返回原文", 2);
            notice = result->message;
            errorNotice = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WorkerStopped:
        UiLog(L"UI_EXIT_COMPLETED", L"UI工作线程已结束，准备销毁窗口");
        // The worker has left every service/IO task. Join only after the message loop exits.
        if (trayAdded) {
            if (!Shell_NotifyIconW(NIM_DELETE, &tray))
                OutputDebugStringW(L"LOL Backup: tray cleanup failed\n");
            trayAdded = false;
        }
        if (trayMenu) {
            DestroyMenu(trayMenu);
            trayMenu = nullptr;
        }
        ShutdownBlockReasonDestroy(hwnd);
        DestroyWindow(hwnd);
        return 0;
    case TrayEvent: {
        const UINT event = LOWORD(lp);
        // NOTIFYICON_VERSION_4 delivers one click as several messages (516/517/123). Opening the
        // popup on each of them made the next one immediately close it again, so the user saw no
        // menu at all. In version 4 WM_CONTEXTMENU is the definitive gesture.
        if (event == WM_CONTEXTMENU || (!trayVersion4 && event == WM_RBUTTONUP))
            ShowTrayMenu();
        else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP ||
                 event == WM_LBUTTONDBLCLK) {
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
                UiLog(L"UI_TRAY_WAKE", L"托盘图标唤醒窗口");
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        ClosePolicy();
        return 0;
    case WM_QUERYENDSESSION:
        ShutdownBlockReasonCreate(hwnd, L"BBS自动工具 正在安全结束文件任务，请稍后重试关机。");
        Shutdown(L"Windows请求结束会话或关机");
        return FALSE; // no silent forced termination in the middle of a write
    case WM_DESTROY:
        KillTimer(hwnd, RefreshTimer);
        KillTimer(hwnd, SmokeTimer);
        KillTimer(hwnd, CaptureTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int App::Run(int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = ClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        const DWORD error = GetLastError();
        UiLog(L"UI_START_FAILED", L"注册主窗口失败 error=" + std::to_wstring(error), 2);
        MessageBoxW(nullptr, (L"无法注册窗口：" + WinError(error)).c_str(), AppTitle, MB_ICONERROR);
        return 1;
    }
    using SystemDpiFn = UINT(WINAPI*)();
    auto systemDpi =
        reinterpret_cast<SystemDpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (systemDpi)
        dpi = (int)systemDpi();
    else {
        HDC dc = GetDC(nullptr);
        if (dc) {
            dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
        }
    }
    if (smoke && previewDpi)
        dpi = previewDpi;
    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    int w = std::min(D(1240), (int)(workArea.right - workArea.left) - D(40)),
        h = std::min(D(852), (int)(workArea.bottom - workArea.top) - D(40));
    if (smoke && previewWidth && previewHeight) {
        // Custom caption: client area equals the window rect, so the logical size is exact.
        w = D(previewWidth);
        h = D(previewHeight);
    }
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, ClassName, AppTitle,
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
        workArea.left + std::max(0, ((int)(workArea.right - workArea.left) - w) / 2),
        workArea.top + std::max(0, ((int)(workArea.bottom - workArea.top) - h) / 2), w, h, nullptr, nullptr,
        instance, this);
    if (!window) {
        const DWORD error = GetLastError();
        UiLog(L"UI_START_FAILED", L"创建主窗口失败 error=" + std::to_wstring(error), 2);
        MessageBoxW(nullptr, (L"无法创建主窗口：" + WinError(error)).c_str(), AppTitle, MB_ICONERROR);
        return 1;
    }
    if (smoke && previewWidth && previewHeight)
        SetWindowPos(window, nullptr, 0, 0, D(previewWidth), D(previewHeight),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Stay hidden until the first service status (theme + window placement) is known, then reveal
    // once, so startup never flashes an empty or wrongly themed frame.
    initialShow = show == SW_HIDE ? SW_SHOWNORMAL : show;
    startedAt = GetTickCount64();
    // Initialize metadata only. Prepare owns its thread and never delays service bootstrap.
    toolsInitialized =
        toolManager.Initialize(instance, hwnd, ToolEvents, smoke ? root + L"\\settings" : root, smoke);
    if (!toolsInitialized)
        toolsStartupError = L"工具管理器不可用；备份、缓存与设置功能不受影响。";
    RefreshTools();
    StartWorker();
    if (smoke && !SetTimer(hwnd, SmokeTimer, static_cast<UINT>(smokeDurationMs), nullptr)) {
        Notify(L"无法启动测试退出计时器，正在安全退出。", true, false);
        Shutdown(L"隔离测试退出计时器启动失败");
    }
    if (smoke && !capturePath.empty() && !SetTimer(hwnd, CaptureTimer, 2000, nullptr))
        Notify(L"无法启动界面截图计时器。", true, false);
    MSG msg{};
    int exitCode = 0;
    for (;;) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (!got) {
            exitCode = static_cast<int>(msg.wParam);
            break;
        }
        if (got == -1) {
            UiLog(L"UI_MESSAGE_LOOP_FAILED", L"主消息循环失败 error=" + std::to_wstring(GetLastError()), 2);
            Notify(L"Windows 消息循环失败，正在安全退出。", true, false);
            Shutdown(L"Windows主消息循环失败");
            exitCode = 1;
            break;
        }
        if (licenseWindow && GetAncestor(msg.hwnd, GA_ROOT) == licenseWindow) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                DestroyWindow(licenseWindow);
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                HWND focus = GetFocus();
                if (focus == GetDlgItem(licenseWindow, 2) || focus == GetDlgItem(licenseWindow, 3) ||
                    focus == GetDlgItem(licenseWindow, 4)) {
                    SendMessageW(licenseWindow, WM_COMMAND, GetDlgCtrlID(focus),
                                 reinterpret_cast<LPARAM>(focus));
                    continue;
                }
            }
            if (IsDialogMessageW(licenseWindow, &msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam >= '1' && msg.wParam <= '5') {
                SetPage(NavPageOrder[(int)msg.wParam - '1']);
                SetFocus(Item(NavFirst + page));
                continue;
            }
            if (msg.wParam == 'B') {
                Send(Backup);
                continue;
            }
            if (msg.wParam == 'R') {
                SetPage(1);
                Send(Restore);
                continue;
            }
            if (msg.wParam == 'S' && page == 3) {
                SaveSettings();
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5) {
            if (!(msg.lParam & (1LL << 30)))
                UiLog(L"UI_MANUAL_REFRESH", page == 2 ? L"用户按F5刷新日志" : L"用户按F5刷新状态");
            if (!serviceStarted)
                RetryStartup();
            else if (page == 2)
                RequestLogs();
            else {
                Request(L"get_status", json::object(), L"", true);
                Refresh();
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND focus = GetFocus();
            wchar_t klass[32]{};
            GetClassNameW(focus, klass, 32);
            if (GetParent(focus) == hwnd && wcscmp(klass, L"Button") == 0 && IsWindowEnabled(focus)) {
                int id = GetDlgCtrlID(focus);
                if (id >= ThemeCombo && id <= StartupCombo)
                    SendMessageW(focus, WM_KEYDOWN, VK_RETURN, 0);
                else
                    Send(id);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT ||
                                          msg.wParam == VK_HOME || msg.wParam == VK_END)) {
            wchar_t klass[32]{};
            GetClassNameW(GetFocus(), klass, 32);
            if (wcscmp(klass, L"Edit") != 0 && page != 2) {
                ScrollTo(msg.wParam == VK_HOME ? 0
                         : msg.wParam == VK_END
                             ? MaxScroll()
                             : scrollY + (msg.wParam == VK_NEXT ? 1 : -1) * std::max(48, height - 256));
                continue;
            }
        }
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UiLog(L"UI_MESSAGE_LOOP_ENDED", L"主消息循环结束 code=" + std::to_wstring(exitCode));
    // No heap-owned UI results survive the window, including completion posted just before destruction.
    while (PeekMessageW(&msg, window, UiResultMessage, UiResultMessage, PM_REMOVE))
        delete reinterpret_cast<UiResult*>(msg.lParam);
    return exitCode;
}
} // namespace nativeui

#include "tool_verification.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace nativeui;
    try {
        platform::WindowsVersion os;
        if (!platform::QueryWindowsVersion(os)) {
            MessageBoxW(nullptr, L"无法确认 Windows 版本，已安全停止启动。请检查系统组件后重试。", AppTitle,
                        MB_ICONWARNING);
            return 1;
        }
        if (!platform::IsSupportedWindowsVersion(os.major, os.build)) {
            MessageBoxW(nullptr, L"本程序需要 Windows 10（build 10240）或更高版本 / Windows 11。", AppTitle,
                        MB_ICONWARNING);
            return 1;
        }
        using AwarenessFn = BOOL(WINAPI*)(HANDLE);
        auto awareness = reinterpret_cast<AwarenessFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
        if (!awareness || !awareness(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
            SetProcessDPIAware();
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com)) {
            MessageBoxW(nullptr, L"无法初始化 Windows 桌面组件。", AppTitle, MB_ICONERROR);
            return 1;
        }
        struct ComScope {
            ~ComScope() {
                CoUninitialize();
            }
        } comScope;
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&common)) {
            MessageBoxW(nullptr, L"无法初始化系统控件。", AppTitle, MB_ICONERROR);
            return 1;
        }
        bool smoke = false, previewLight = false, hasPreviewPage = false;
        int previewPage = 0, previewWidth = 0, previewHeight = 0;
        int previewDpi = 0, previewDurationMs = SmokePreviewDefaultMs;
        bool previewWelcome = false, verifyToolsOnly = false;
        std::wstring root, captureFile;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) {
            MessageBoxW(nullptr, L"无法读取启动参数。", AppTitle, MB_ICONERROR);
            return 1;
        }
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--verify-tools") == 0 && i + 1 < argc) {
                verifyToolsOnly = true;
                root = argv[++i];
            } else if (wcscmp(argv[i], L"--smoke") == 0 && i + 1 < argc) {
                smoke = true;
                root = argv[++i];
            } else if (wcscmp(argv[i], L"--capture") == 0 && i + 1 < argc) {
                captureFile = argv[++i];
            } else if (wcscmp(argv[i], L"--smoke-light") == 0) {
                previewLight = true;
            } else if (wcscmp(argv[i], L"--smoke-welcome") == 0) {
                previewWelcome = true;
            } else if (wcscmp(argv[i], L"--smoke-dpi") == 0 && i + 1 < argc) {
                wchar_t* end = nullptr;
                long value = wcstol(argv[++i], &end, 10);
                if (!end || *end || value < 96 || value > 384) {
                    LocalFree(argv);
                    return 2;
                }
                previewDpi = static_cast<int>(value);
            } else if (wcscmp(argv[i], L"--smoke-duration") == 0 && i + 1 < argc) {
                wchar_t* end = nullptr;
                long value = wcstol(argv[++i], &end, 10);
                if (!end || *end || value < SmokePreviewMinMs || value > SmokePreviewMaxMs) {
                    LocalFree(argv);
                    return 2;
                }
                previewDurationMs = static_cast<int>(value);
            } else if (wcscmp(argv[i], L"--smoke-size") == 0 && i + 2 < argc) {
                wchar_t* endW = nullptr;
                wchar_t* endH = nullptr;
                long w = wcstol(argv[++i], &endW, 10), h = wcstol(argv[++i], &endH, 10);
                if (!endW || *endW || !endH || *endH || w < 940 || w > 2400 || h < 640 || h > 1800) {
                    LocalFree(argv);
                    MessageBoxW(nullptr, L"--smoke-size 需要逻辑客户区尺寸 W H（940–2400 × 640–1800）。",
                                AppTitle, MB_ICONERROR);
                    return 2;
                }
                previewWidth = (int)w;
                previewHeight = (int)h;
            } else if (wcscmp(argv[i], L"--smoke-page") == 0 && i + 1 < argc) {
                hasPreviewPage = true;
                wchar_t* end = nullptr;
                long value = wcstol(argv[++i], &end, 10);
                if (!end || *end || value < 0 || value >= PageCount) {
                    LocalFree(argv);
                    MessageBoxW(nullptr, L"--smoke-page 必须为 0–4。", AppTitle, MB_ICONERROR);
                    return 2;
                }
                previewPage = (int)value;
            } else {
                LocalFree(argv);
                MessageBoxW(nullptr,
                            L"启动参数无效。测试用法：--smoke <绝对临时根目录> [--capture <根内 PNG>] "
                            L"[--smoke-page 0..4] [--smoke-light] [--smoke-size W H]",
                            AppTitle, MB_ICONERROR);
                return 2;
            }
        }
        LocalFree(argv);
        if (verifyToolsOnly) {
            if (!AbsolutePath(root) || smoke || !captureFile.empty() || hasPreviewPage || previewLight ||
                previewWidth || previewHeight || previewDpi || previewWelcome ||
                previewDurationMs != SmokePreviewDefaultMs)
                return 2;
            return integration_verification::Run(instance, FullPath(root));
        }
        if (smoke && !AbsolutePath(root)) {
            MessageBoxW(nullptr, L"--smoke 必须指定绝对临时根目录；未写入任何应用配置。", AppTitle,
                        MB_ICONERROR);
            return 2;
        }
        if (!smoke &&
            (!captureFile.empty() || previewLight || hasPreviewPage || previewWidth || previewHeight ||
             previewDpi || previewWelcome || previewDurationMs != SmokePreviewDefaultMs)) {
            MessageBoxW(nullptr, L"截图和预览参数仅可与 --smoke 一同使用。", AppTitle, MB_ICONERROR);
            return 2;
        }
        if (!smoke) {
            std::wstring local = KnownFolder(FOLDERID_LocalAppData);
            if (local.empty()) {
                MessageBoxW(nullptr, L"无法读取 LocalAppData，已停止启动以免写入错误位置。", AppTitle,
                            MB_ICONERROR);
                return 1;
            }
            root = local + L"\\LOLBackupV3";
        }
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/'))
            root.pop_back();
        root = FullPath(root);
        if (root.empty()) {
            MessageBoxW(nullptr, L"应用数据路径无效。", AppTitle, MB_ICONERROR);
            return 2;
        }
        if (!captureFile.empty()) {
            std::wstring full = AbsolutePath(captureFile) ? FullPath(captureFile) : L"",
                         prefix = root + L"\\";
            bool inside = full.size() > prefix.size() &&
                          CompareStringOrdinal(full.data(), (int)prefix.size(), prefix.data(),
                                               (int)prefix.size(), TRUE) == CSTR_EQUAL;
            bool png = full.size() > 4 && _wcsicmp(full.c_str() + full.size() - 4, L".png") == 0;
            if (!inside || !png || full.find(L':', 2) != std::wstring::npos) {
                MessageBoxW(nullptr, L"--capture 必须是 --smoke 临时根目录内的绝对 PNG 路径。", AppTitle,
                            MB_ICONERROR);
                return 2;
            }
            captureFile = full;
        }
        std::wstring dump = (smoke ? root + L"\\settings" : root) + L"\\logs\\crash.dmp";
        lstrcpynW(DumpPath, dump.c_str(), static_cast<int>(sizeof(DumpPath) / sizeof(wchar_t)));
        ULONG stack = static_cast<ULONG>(common::kCrashStackGuaranteeBytes);
        SetThreadStackGuarantee(&stack);
        SetUnhandledExceptionFilter(CrashFilter);
        HANDLE mutex = nullptr;
        if (!smoke) {
            std::wstring mutexName = InstanceMutexName();
            if (mutexName.empty()) {
                MessageBoxW(nullptr, L"无法读取当前账户标识，已停止启动以免跨会话重复运行。", AppTitle,
                            MB_ICONERROR);
                return 1;
            }
            mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
            DWORD error = GetLastError();
            if (!mutex) {
                MessageBoxW(nullptr, (L"无法创建单实例锁：" + WinError(error)).c_str(), AppTitle,
                            MB_ICONERROR);
                return 1;
            }
            if (error == ERROR_ALREADY_EXISTS) {
                // The other instance may still be creating its window: retry briefly instead of
                // immediately telling the user to close something that is about to appear.
                InstanceWindowSearch search{ProcessUserSid(GetCurrentProcess())};
                for (int attempt = 0; attempt < 60; ++attempt) {
                    search.window = nullptr;
                    search.processId = 0;
                    EnumWindows(FindInstanceWindow, reinterpret_cast<LPARAM>(&search));
                    DWORD processId = 0;
                    if (search.window && GetWindowThreadProcessId(search.window, &processId) &&
                        processId == search.processId &&
                        PostMessageW(search.window, SingleInstanceWake, 0, 0)) {
                        // Only a verified same-account window with a delivered wake may be shown.
                        if (IsIconic(search.window))
                            ShowWindow(search.window, SW_RESTORE);
                        ShowWindow(search.window, SW_SHOW);
                        SetForegroundWindow(search.window);
                        CloseHandle(mutex);
                        return 0;
                    }
                    Sleep(80);
                }
                // A running instance always owns the future window; if it is mid-startup or on another
                // desktop, silently step aside instead of popping a confusing system dialog.
                OutputDebugStringW(L"BBS自动工具: another instance is running; new launch exits quietly.\n");
                CloseHandle(mutex);
                return 0;
            }
        }
        struct HandleScope {
            HANDLE h;
            ~HandleScope() {
                if (h)
                    CloseHandle(h);
            }
        } mutexScope{mutex};
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            MessageBoxW(nullptr, L"无法初始化 Windows 图形组件。", AppTitle, MB_ICONERROR);
            return 1;
        }
        struct GraphicsScope {
            ULONG_PTR token;
            ~GraphicsScope() {
                Gdiplus::GdiplusShutdown(token);
            }
        } graphicsScope{token};
        App app(instance, root, smoke, previewPage, previewLight, captureFile, previewWidth, previewHeight);
        app.ConfigureSmokePreview(previewDpi, previewWelcome, previewDurationMs);
        return app.Run(show);
    } catch (const std::exception&) {
        UiLog(L"UI_START_FAILED", L"程序启动或运行异常；不记录异常原文", 2);
        MessageBoxW(nullptr, L"程序启动失败，请检查本地应用数据目录中的日志。", AppTitle, MB_ICONERROR);
        return 1;
    } catch (...) {
        UiLog(L"UI_START_FAILED", L"程序启动或运行发生未知异常", 2);
        MessageBoxW(nullptr, L"程序遇到未预期的错误。", AppTitle, MB_ICONERROR);
        return 1;
    }
}
