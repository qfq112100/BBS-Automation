#include "tool_manager.h"
#include "../../third_party/json/json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fdi.h>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <shellapi.h>
#include <stdexcept>
#include <thread>
#include <tlhelp32.h>
#include <vector>
#include <wincrypt.h>

namespace core {
namespace {
using Json = nlohmann::json;
struct Failure : std::runtime_error {
    DWORD code;
    Failure(const char* text, DWORD value) : std::runtime_error(text), code(value) {}
};
[[noreturn]] void Fail(const char* text, DWORD code = ERROR_INVALID_DATA) {
    throw Failure(text, code);
}
std::wstring Wide(const std::string& text) {
    if (text.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                   nullptr, 0);
    if (!size)
        Fail("Invalid UTF-8", ERROR_NO_UNICODE_TRANSLATION);
    std::wstring out(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &out[0],
                        size);
    if (out.find(L'\0') != std::wstring::npos)
        Fail("Embedded NUL");
    return out;
}
std::wstring Lower(std::wstring value) {
    if (!value.empty())
        CharLowerBuffW(&value[0], static_cast<DWORD>(value.size()));
    return value;
}
std::wstring Native(const std::wstring& path) {
    if (path.compare(0, 4, L"\\\\?\\") == 0)
        return path;
    if (path.compare(0, 2, L"\\\\") == 0)
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
std::wstring Absolute(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos || path.compare(0, 4, L"\\\\.\\") == 0 ||
        path.compare(0, 4, L"\\\\?\\") == 0)
        Fail("Invalid data root", ERROR_BAD_PATHNAME);
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!n || n > 32000)
        Fail("Cannot resolve data root", GetLastError());
    std::wstring result(n, 0);
    DWORD used = GetFullPathNameW(path.c_str(), n, &result[0], nullptr);
    if (!used || used >= n)
        Fail("Cannot resolve data root", GetLastError());
    result.resize(used);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\')
        result.pop_back();
    return result;
}
std::wstring Relative(const std::string& text) {
    std::wstring path = Wide(text);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.empty() || path.size() > 30000 || path.front() == L'\\')
        Fail("Unsafe relative path");
    size_t at = 0;
    while (at < path.size()) {
        size_t end = path.find(L'\\', at);
        if (end == std::wstring::npos)
            end = path.size();
        std::wstring part = path.substr(at, end - at);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
            Fail("Unsafe relative path");
        for (wchar_t c : part)
            if (c < 32 || std::wstring(L":*?\"<>|").find(c) != std::wstring::npos)
                Fail("Unsafe relative path");
        std::wstring base = Lower(part.substr(0, part.find(L'.')));
        if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" ||
            (base.size() == 4 && (base.substr(0, 3) == L"com" || base.substr(0, 3) == L"lpt") &&
             base[3] >= L'0' && base[3] <= L'9'))
            Fail("Reserved device path");
        at = end + 1;
    }
    if (path.back() == L'\\')
        Fail("Empty path component");
    return path;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
// Holding non-delete-shared directory handles prevents junction swaps while writing descendants.
struct Directories {
    std::vector<std::unique_ptr<Handle>> locks;
    std::set<std::wstring> locked;
    void Ensure(const std::wstring& path, bool create) {
        size_t start = path.compare(0, 2, L"\\\\") == 0 ? path.find(L'\\', 2) : 2;
        if (start == std::wstring::npos)
            Fail("Invalid root");
        if (path.compare(0, 2, L"\\\\") == 0)
            start = path.find(L'\\', start + 1);
        if (start == std::wstring::npos)
            start = path.size();
        for (size_t end = start; end <= path.size();) {
            if (end > 2) {
                std::wstring part = path.substr(0, end);
                if (part.size() == 2 && part[1] == L':')
                    part += L'\\';
                if (locked.count(Lower(part))) {
                    if (end == path.size())
                        break;
                    end = path.find(L'\\', end + 1);
                    if (end == std::wstring::npos)
                        end = path.size();
                    continue;
                }
                if (create && !CreateDirectoryW(Native(part).c_str(), nullptr)) {
                    DWORD e = GetLastError();
                    if (e != ERROR_ALREADY_EXISTS)
                        Fail("Cannot create component directory", e);
                }
                auto h = std::make_unique<Handle>(CreateFileW(
                    Native(part).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (h->value == INVALID_HANDLE_VALUE)
                    Fail("Cannot lock component directory", GetLastError());
                BY_HANDLE_FILE_INFORMATION info{};
                if (!GetFileInformationByHandle(h->value, &info))
                    Fail("Cannot inspect directory", GetLastError());
                if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    Fail("Reparse directory rejected");
                locks.push_back(std::move(h));
                locked.insert(Lower(part));
            }
            if (end == path.size())
                break;
            end = path.find(L'\\', end + 1);
            if (end == std::wstring::npos)
                end = path.size();
        }
    }
};
std::wstring Parent(const std::wstring& path) {
    return path.substr(0, path.find_last_of(L'\\'));
}
void Regular(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info))
        Fail("Cannot inspect component file", GetLastError());
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1)
        Fail("Non-regular component file rejected");
}
std::string Digest(HANDLE file, const std::string* bytes = nullptr, const std::atomic<bool>* stop = nullptr) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        Fail("SHA256 provider unavailable", GetLastError());
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        DWORD e = GetLastError();
        CryptReleaseContext(provider, 0);
        Fail("SHA256 unavailable", e);
    }
    DWORD error = ERROR_SUCCESS;
    if (bytes) {
        if (!CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes->data()),
                           static_cast<DWORD>(bytes->size()), 0))
            error = GetLastError();
    } else {
        BYTE buffer[65536];
        for (;;) {
            if (stop && stop->load()) {
                error = ERROR_CANCELLED;
                break;
            }
            DWORD read = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
                error = GetLastError();
                break;
            }
            if (!read)
                break;
            if (!CryptHashData(hash, buffer, read, 0)) {
                error = GetLastError();
                break;
            }
        }
    }
    BYTE digest[32];
    DWORD size = sizeof(digest);
    if (!error && !CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0))
        error = GetLastError();
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (error)
        Fail("SHA256 failed", error);
    std::string out;
    for (BYTE b : digest) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
struct Resource {
    const BYTE* data = nullptr;
    size_t size = 0;
};
struct Process {
    DWORD pid;
    std::wstring image;
};
#ifdef TOOL_MANAGER_TEST
struct TestHooks {
    std::function<Resource(int)> resource;
    std::function<std::vector<Process>()> processes;
    std::function<DWORD(const std::wstring&, bool, bool)> launch;
    std::function<DWORD(DWORD, bool)> close;
    std::function<bool(DWORD)> reopen;
};
TestHooks testHooks;
#endif
Resource Load(HINSTANCE module, int id) {
#ifdef TOOL_MANAGER_TEST
    if (testHooks.resource)
        return testHooks.resource(id);
#endif
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource)
        Fail("Bundled resource missing", GetLastError());
    HGLOBAL loaded = LoadResource(module, resource);
    DWORD size = SizeofResource(module, resource);
    const BYTE* bytes = static_cast<const BYTE*>(LockResource(loaded));
    if (!bytes || !size)
        Fail("Bundled resource unreadable", ERROR_RESOURCE_DATA_NOT_FOUND);
    return {bytes, size};
}
std::vector<Process> Processes() {
#ifdef TOOL_MANAGER_TEST
    if (testHooks.processes)
        return testHooks.processes();
#endif
    std::vector<Process> out;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE)
        Fail("Process snapshot failed", GetLastError());
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) {
        DWORD e = GetLastError();
        if (e != ERROR_NO_MORE_FILES)
            Fail("Process enumeration failed", e);
        return out;
    }
    do {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (process.value == INVALID_HANDLE_VALUE)
            continue;
        std::wstring path(32768, 0);
        DWORD n = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process.value, 0, &path[0], &n)) {
            path.resize(n);
            out.push_back({entry.th32ProcessID, Lower(path)});
        }
    } while (Process32NextW(snapshot.value, &entry));
    return out;
}
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
// Copy entries verbatim, including drive-current-directory pseudoentries (=C:=...).
// Only the exact Electron mode key is removed; the parent environment is never changed.
std::vector<wchar_t> AkariEnvironment(const wchar_t* source) {
    std::vector<wchar_t> result;
    for (const wchar_t* at = source; at && *at;) {
        const std::wstring entry(at);
        const size_t keyEnd = entry.find(L'=');
        if (keyEnd != 20 ||
            CompareStringOrdinal(entry.c_str(), static_cast<int>(keyEnd),
                                 L"ELECTRON_RUN_AS_NODE", 20, TRUE) != CSTR_EQUAL) {
            result.insert(result.end(), entry.begin(), entry.end());
            result.push_back(L'\0');
        }
        at += entry.size() + 1;
    }
    result.push_back(L'\0');
    if (result.size() == 1)
        result.push_back(L'\0');
    return result;
}
DWORD Execute(const std::wstring& path, bool elevated, HWND owner, bool cleanAkari = false) {
#ifdef TOOL_MANAGER_TEST
    if (testHooks.launch)
        return testHooks.launch(path, elevated, cleanAkari);
#endif
    if (cleanAkari && !elevated) {
        struct Environment {
            LPWCH value = GetEnvironmentStringsW();
            ~Environment() { if (value) FreeEnvironmentStringsW(value); }
        } inherited;
        if (!inherited.value)
            return GetLastError();
        auto environment = AkariEnvironment(inherited.value);
        std::wstring commandLine = L"\"" + path + L"\"";
        const std::wstring directory = Parent(path);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), &commandLine[0], nullptr, nullptr, FALSE,
                            CREATE_UNICODE_ENVIRONMENT, environment.data(), directory.c_str(),
                            &startup, &process))
            return GetLastError();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return ERROR_SUCCESS;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.hwnd = owner;
    info.lpVerb = elevated ? L"runas" : L"open";
    info.lpFile = path.c_str();
    std::wstring directory = Parent(path);
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        return GetLastError();
    if (info.hProcess)
        CloseHandle(info.hProcess);
    return ERROR_SUCCESS;
}
struct ReopenWindowContext {
    DWORD pid;
    HWND window = nullptr;
    bool allowUntitled = false;
};
bool ReopenWindowEligible(bool hasOwner, LONG_PTR extendedStyle, int titleLength,
                          bool allowUntitled, bool visible, bool rectangleValid, const RECT& rectangle) {
    if (hasOwner || (extendedStyle & WS_EX_TOOLWINDOW))
        return false;
    // Preserve Akari's existing titled-window policy. Only ABC opts into custom-drawn titles.
    if (titleLength > 0)
        return true;
    return allowUntitled && visible && rectangleValid &&
           !(extendedStyle & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) &&
           rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}
BOOL CALLBACK FindReopenWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<ReopenWindowContext*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != context.pid)
        return TRUE;
    const int titleLength = GetWindowTextLengthW(window);
    RECT rectangle{};
    const bool rectangleValid = context.allowUntitled && titleLength <= 0 && GetWindowRect(window, &rectangle);
    if (ReopenWindowEligible(GetWindow(window, GW_OWNER) != nullptr,
                             GetWindowLongPtrW(window, GWL_EXSTYLE), titleLength, context.allowUntitled,
                             IsWindowVisible(window) != FALSE, rectangleValid, rectangle)) {
        context.window = window;
        return FALSE;
    }
    return TRUE;
}
bool ReopenOwned(DWORD pid, const std::wstring& target, bool allowUntitled = false) {
#ifdef TOOL_MANAGER_TEST
    if (testHooks.reopen)
        return testHooks.reopen(pid);
#endif
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (process.value == INVALID_HANDLE_VALUE)
        Fail("Cannot verify running component", GetLastError());
    std::wstring image(32768, 0);
    DWORD n = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.value, 0, &image[0], &n))
        Fail("Cannot verify running component image", GetLastError());
    image.resize(n);
    if (!SamePath(image, target) || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
        Fail("Running component changed; retry launch", ERROR_NOT_FOUND);
    ReopenWindowContext context{pid, nullptr, allowUntitled};
    EnumWindows(FindReopenWindow, reinterpret_cast<LPARAM>(&context));
    if (!context.window)
        return false;
    ShowWindowAsync(context.window, SW_RESTORE);
    if (!SetForegroundWindow(context.window)) {
        if (allowUntitled) {
            // ABC is restored already. Respect Windows foreground protection: request
            // attention instead of reporting a running app as failed or spawning it again.
            FLASHWINFO attention{sizeof(attention), context.window, FLASHW_TRAY, 3, 0};
            FlashWindowEx(&attention);
            return true;
        }
        Fail("Component window restored; Windows denied foreground activation", ERROR_ACCESS_DENIED);
    }
    return true;
}
struct CloseWindows {
    DWORD pid;
    bool any = false;
    DWORD error = ERROR_SUCCESS;
};
BOOL CALLBACK CloseWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<CloseWindows*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == context.pid) {
        context.any = true;
        if (!PostMessageW(window, WM_CLOSE, 0, 0))
            context.error = GetLastError();
    }
    return TRUE;
}
struct File {
    std::string cab, sha;
    std::wstring path;
    ULONGLONG size;
    bool mutableData = false;
};
struct Binding {
    size_t tool;
    std::wstring from, to;
};
struct Definition {
    std::string id, stamp, itemStamp;
    bool legacy336 = false;
    std::wstring name, version, storageVersion, entry, directory;
    int resource = 0;
    bool elevated = false, legacyAkari = false;
    std::vector<File> files;
    std::vector<Binding> bindings;
};
size_t Index(ToolId id) {
    switch (id) {
    case ToolId::Rose:
        return 0;
    case ToolId::Akari:
        return 1;
    case ToolId::Ltk:
        return 2;
    case ToolId::Abc:
        return 3;
    }
    return 4;
}
size_t Index(const std::string& id) {
    if (id == "rose")
        return 0;
    if (id == "akari")
        return 1;
    if (id == "ltk")
        return 2;
    if (id == "abc")
        return 3;
    Fail("Unknown tool id");
}
std::vector<Definition> Parse(const Resource& resource, const std::wstring& root) {
    if (!resource.data || !resource.size || resource.size > 16 * 1024 * 1024)
        Fail("Invalid manifest resource");
    Json manifest = Json::parse(resource.data, resource.data + resource.size);
    if (manifest.at("format").get<int>() != 1 || !manifest.at("tools").is_array())
        Fail("Unsupported manifest format");
    std::vector<Definition> definitions(manifest.at("tools").size());
    if (definitions.size() != 3 && definitions.size() != 4)
        Fail("Manifest must contain three or four components");
    std::set<size_t> seen;
    for (const auto& item : manifest.at("tools")) {
        size_t i = Index(item.at("id").get<std::string>());
        if (!seen.insert(i).second)
            Fail("Duplicate tool id");
        if (i >= definitions.size())
            Fail("Missing component slot");
        auto& d = definitions[i];
        d.id = item.at("id").get<std::string>();
        d.name = Wide(item.at("name").get<std::string>());
        d.version = Relative(item.at("version").get<std::string>());
        if (d.version.find(L'\\') != std::wstring::npos)
            Fail("Unsafe component version");
        d.storageVersion = Relative(item.value("storage_version", item.at("version").get<std::string>()));
        if (d.storageVersion.find(L'\\') != std::wstring::npos)
            Fail("Unsafe component storage version");
        std::string canonicalItem = item.dump();
        d.itemStamp = Digest(INVALID_HANDLE_VALUE, &canonicalItem);
        d.legacyAkari = d.id == "akari" && d.version == L"1.5.1" &&
            Digest(INVALID_HANDLE_VALUE, &canonicalItem) ==
                "e84c52576629d3f77d066b72cdbc0c2e20b2db01b6db01bff901a2c3ab57c22b";
        std::string entry = item.at("entry").get<std::string>();
        d.entry = entry.empty() ? L"" : Relative(entry);
        if (i == 2 && !d.entry.empty())
            Fail("LTK must be a prepare-only component");
        if (i == 3 && d.entry != L"ABC.exe")
            Fail("ABC must use its pinned executable entry");
        d.resource = item.at("cab_resource").get<int>();
        if (d.resource != 3101 + static_cast<int>(i))
            Fail("Unexpected cabinet resource");
        d.elevated = item.at("elevated").get<bool>();
        d.directory = root + L"\\components\\" + Wide(d.id) + L"\\" + d.storageVersion;
        std::set<std::string> tokens;
        std::set<std::wstring> paths;
        if (!item.at("files").is_array() || item.at("files").empty())
            Fail("Empty component files");
        for (const auto& f : item.at("files")) {
            File file;
            file.cab = f.at("cab").get<std::string>();
            if (file.cab.size() != 7 || file.cab[0] != 'f' ||
                file.cab.find_first_not_of("0123456789", 1) != std::string::npos)
                Fail("Unsafe cabinet token");
            file.path = Relative(f.at("path").get<std::string>());
            if (Lower(file.path) == L".bbs-component.json")
                Fail("Reserved marker path");
            file.mutableData = f.value("mutable", false);
            if (file.mutableData) {
                const std::set<std::wstring> mutableSeeds = {L"data\\config.json", L"data\\user.db", L"data\\abc.ini", L"data\\updates.ini", L"lib\\config.toml"};
                if (d.id != "abc" || !mutableSeeds.count(Lower(file.path)))
                    Fail("Only approved ABC data/config seeds may be mutable");
            }
            file.sha = f.at("sha256").get<std::string>();
            if (file.sha.size() != 64 || file.sha.find_first_not_of("0123456789abcdef") != std::string::npos)
                Fail("Invalid SHA256");
            if (!f.at("size").is_number_unsigned() && !f.at("size").is_number_integer())
                Fail("Invalid file size");
            if (f.at("size").get<int64_t>() < 0)
                Fail("Invalid file size");
            file.size = f.at("size").get<ULONGLONG>();
            if (!tokens.insert(file.cab).second || !paths.insert(Lower(file.path)).second)
                Fail("Duplicate manifest file");
            d.files.push_back(std::move(file));
        }
        std::set<std::wstring> bindingPaths;
        for (const auto& b : item.value("bindings", Json::array())) {
            Binding binding{Index(b.at("tool").get<std::string>()), Relative(b.at("from").get<std::string>()),
                            Relative(b.at("to").get<std::string>())};
            if (!bindingPaths.insert(Lower(binding.to)).second || Lower(binding.to) == L".bbs-component.json")
                Fail("Duplicate binding destination");
            paths.insert(Lower(binding.to));
            d.bindings.push_back(std::move(binding));
        }
        if (!d.entry.empty() && !paths.count(Lower(d.entry)))
            Fail("Entry not present in manifest");
    }
    if (seen.size() != definitions.size() || !seen.count(0) || !seen.count(1) || !seen.count(2))
        Fail("Manifest must contain rose, akari and ltk");
    for (const auto& d : definitions)
        for (const auto& b : d.bindings)
            if (b.tool >= definitions.size()) Fail("Missing binding component");
    // One stamp includes dependency hashes and mappings, not only this component's version.
    std::string canonical = manifest.dump();
    std::string stamp = Digest(INVALID_HANDLE_VALUE, &canonical);
    // Only the exact shipped 3.3.6 specifications may adopt its old global receipt.
    // Rose also pins its LTK dependency; ABC can never inherit an unrelated receipt.
    const char* oldItems[] = {
        "42d657e7301b2b7f84ba946300a84ed2e75516cad2099945c1d5ea640299b751",
        "e84c52576629d3f77d066b72cdbc0c2e20b2db01b6db01bff901a2c3ab57c22b",
        "b2eee1cce040ab645d298bc205ef4e4e372e1cbe0fc4828bf36515abbde13e40"};
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto& d = definitions[i];
        d.stamp = stamp;
        d.legacy336 = i < 3 && d.itemStamp == oldItems[i];
        if (i == 0) d.legacy336 = d.legacy336 && definitions[2].itemStamp == oldItems[2];
    }
    return definitions;
}
size_t FileCount(const Definition& definition) {
    std::set<std::wstring> paths;
    for (const auto& f : definition.files)
        paths.insert(Lower(f.path));
    for (const auto& b : definition.bindings)
        paths.insert(Lower(b.to));
    return paths.size();
}
bool MarkerAccepted(const Definition& d, const Json& marker) {
    if (marker.value("format", 0) != 1 || marker.value("files", size_t(0)) != FileCount(d))
        return false;
    const std::string stamp = marker.value("stamp", "");
    return stamp == d.stamp || (d.legacy336 &&
        stamp == "5fe7b6700c9d074453a88994f0e30182057414e960f999918867ab4d7aef0a49") || (d.legacyAkari && FileCount(d) == 71 &&
        stamp == "84ac9f2ff62ea41de1c1452790e32ee80ed52b56692a229dac5ff3c39e79a83d");
}

void EnsureAbcLogDirectories(const Definition& definition, const std::wstring& directory) {
    if (definition.id != "abc") return;
    // Empty writable log directories are local app state, never payload/sentinel files.
    // Reuse the locked ancestor walk so neither junctions nor non-directories are followed.
    Directories directories;
    directories.Ensure(directory, false);
    directories.Ensure(directory + L"\\Log", true);
    directories.Ensure(directory + L"\\lib\\logs", true);
}
struct CabFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool input = false, closed = false;
    size_t offset = 0;
    ULONGLONG expected = 0, written = 0;
    ~CabFile() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
struct Cabinet {
    Resource bytes;
    const Definition& definition;
    std::wstring root;
    std::atomic<bool>& stop;
    std::function<void(int)> progress;
    std::vector<std::unique_ptr<CabFile>> handles;
    std::set<std::string> seen, complete;
    DWORD error = ERROR_SUCCESS;
    Directories directories;
    Cabinet(Resource r, const Definition& d, std::wstring p, std::atomic<bool>& s,
            std::function<void(int)> fn)
        : bytes(r), definition(d), root(std::move(p)), stop(s), progress(std::move(fn)) {}
};
thread_local Cabinet* activeCabinet = nullptr;
void* DIAMONDAPI CabAlloc(ULONG size) {
    return std::malloc(size);
}
void DIAMONDAPI CabFree(void* value) {
    std::free(value);
}
INT_PTR DIAMONDAPI CabOpen(char* name, int, int) {
    try {
        auto& c = *activeCabinet;
        if (c.stop.load())
            Fail("Preparation cancelled", ERROR_CANCELLED);
        if (std::strcmp(name, "bbs.cab") != 0)
            Fail("External cabinet rejected");
        auto file = std::make_unique<CabFile>();
        file->input = true;
        auto* result = file.get();
        c.handles.push_back(std::move(file));
        return reinterpret_cast<INT_PTR>(result);
    } catch (const Failure& e) {
        activeCabinet->error = e.code;
    } catch (...) {
        activeCabinet->error = ERROR_NOT_ENOUGH_MEMORY;
    }
    return -1;
}
UINT DIAMONDAPI CabRead(INT_PTR value, void* buffer, UINT count) {
    auto& c = *activeCabinet;
    auto& f = *reinterpret_cast<CabFile*>(value);
    if (c.stop.load()) {
        c.error = ERROR_CANCELLED;
        return static_cast<UINT>(-1);
    }
    if (!f.input || f.closed || f.offset > c.bytes.size)
        return static_cast<UINT>(-1);
    size_t n = std::min<size_t>(count, c.bytes.size - f.offset);
    std::memcpy(buffer, c.bytes.data + f.offset, n);
    f.offset += n;
    return static_cast<UINT>(n);
}
UINT DIAMONDAPI CabWrite(INT_PTR value, void* buffer, UINT count) {
    auto& c = *activeCabinet;
    auto& f = *reinterpret_cast<CabFile*>(value);
    if (c.stop.load()) {
        c.error = ERROR_CANCELLED;
        return static_cast<UINT>(-1);
    }
    if (f.input || f.closed || f.written + count > f.expected) {
        c.error = ERROR_INVALID_DATA;
        return static_cast<UINT>(-1);
    }
    DWORD done = 0;
    if (!WriteFile(f.handle, buffer, count, &done, nullptr) || done != count) {
        c.error = GetLastError();
        return static_cast<UINT>(-1);
    }
    f.written += done;
    return done;
}
int DIAMONDAPI CabClose(INT_PTR value) {
    auto& f = *reinterpret_cast<CabFile*>(value);
    if (f.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(f.handle);
        f.handle = INVALID_HANDLE_VALUE;
    }
    f.closed = true;
    return 0;
}
long DIAMONDAPI CabSeek(INT_PTR value, long distance, int origin) {
    auto& c = *activeCabinet;
    auto& f = *reinterpret_cast<CabFile*>(value);
    int64_t at = origin == SEEK_SET   ? 0
                 : origin == SEEK_CUR ? static_cast<int64_t>(f.offset)
                 : origin == SEEK_END ? static_cast<int64_t>(c.bytes.size)
                                      : -1;
    at += distance;
    if (!f.input || at < 0 || static_cast<uint64_t>(at) > c.bytes.size || at > LONG_MAX)
        return -1;
    f.offset = static_cast<size_t>(at);
    return static_cast<long>(at);
}
INT_PTR DIAMONDAPI CabNotify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION info) {
    auto& c = *activeCabinet;
    try {
        if (c.stop.load())
            Fail("Preparation cancelled", ERROR_CANCELLED);
        if (type == fdintNEXT_CABINET || type == fdintPARTIAL_FILE)
            Fail("Split cabinet rejected");
        if (type == fdintCOPY_FILE) {
            auto found = std::find_if(c.definition.files.begin(), c.definition.files.end(),
                                      [&](const File& f) { return f.cab == info->psz1; });
            if (found == c.definition.files.end() || !c.seen.insert(found->cab).second || info->cb < 0 ||
                static_cast<ULONGLONG>(info->cb) != found->size)
                Fail("Cabinet differs from manifest");
            std::wstring path = c.root + L"\\" + found->path;
            c.directories.Ensure(Parent(path), true);
            auto file = std::make_unique<CabFile>();
            file->expected = found->size;
            file->handle = CreateFileW(Native(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file->handle == INVALID_HANDLE_VALUE)
                Fail("Cannot create cabinet output", GetLastError());
            Regular(file->handle);
            auto* result = file.get();
            c.handles.push_back(std::move(file));
            return reinterpret_cast<INT_PTR>(result);
        }
        if (type == fdintCLOSE_FILE_INFO) {
            auto& f = *reinterpret_cast<CabFile*>(info->hf);
            if (f.written != f.expected)
                Fail("Incomplete cabinet file");
            if (!FlushFileBuffers(f.handle))
                Fail("Cannot flush extracted file", GetLastError());
            CabClose(info->hf);
            c.complete.insert(info->psz1);
            c.progress(static_cast<int>(c.complete.size() * 85 / c.definition.files.size()));
            return TRUE;
        }
        return 0;
    } catch (const Failure& e) {
        c.error = e.code;
    } catch (...) {
        c.error = ERROR_INVALID_DATA;
    }
    return -1;
}
void Extract(Cabinet& context) {
    ERF error{};
    activeCabinet = &context;
    HFDI fdi =
        FDICreate(CabAlloc, CabFree, CabOpen, CabRead, CabWrite, CabClose, CabSeek, cpuUNKNOWN, &error);
    if (!fdi) {
        activeCabinet = nullptr;
        Fail("FDICreate failed", ERROR_INVALID_DATA);
    }
    char name[] = "bbs.cab", directory[] = "";
    BOOL ok = FDICopy(fdi, name, directory, 0, CabNotify, nullptr, &context);
    FDIDestroy(fdi);
    activeCabinet = nullptr;
    if (!ok || context.error || context.complete.size() != context.definition.files.size())
        Fail("Cabinet extraction failed", context.error ? context.error : ERROR_INVALID_DATA);
}
void CheckFile(const std::wstring& root, const File& file, bool hash, std::atomic<bool>& stop,
               bool existingMutable = false) {
    Directories parents;
    parents.Ensure(Parent(root + L"\\" + file.path), false);
    const bool mutableCheckRegular = existingMutable && file.mutableData;
    // Vendor databases may be open with share mode zero. Inspect metadata, never their data.
    Handle h(CreateFileW(Native(root + L"\\" + file.path).c_str(),
                         mutableCheckRegular ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                         mutableCheckRegular ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.value == INVALID_HANDLE_VALUE)
        Fail("Component file missing", GetLastError());
    Regular(h.value);
    // Only explicit ABC data seeds may change after first publication, never executable/DLL payloads.
    if (mutableCheckRegular) return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.value, &size))
        Fail("Cannot read component file size", GetLastError());
    if (size.QuadPart < 0 || static_cast<ULONGLONG>(size.QuadPart) != file.size)
        Fail("Component file size mismatch");
    if (hash && Digest(h.value, nullptr, &stop) != file.sha)
        Fail("Component SHA256 mismatch", ERROR_CRC);
}
std::string ReadSmall(const std::wstring& path) {
    Handle h(CreateFileW(Native(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.value == INVALID_HANDLE_VALUE)
        Fail("Cannot read tool-center metadata", GetLastError());
    Regular(h.value);
    LARGE_INTEGER n{};
    if (!GetFileSizeEx(h.value, &n))
        Fail("Cannot size metadata", GetLastError());
    if (n.QuadPart < 0 || n.QuadPart > 1024 * 1024)
        Fail("Metadata too large");
    std::string bytes(static_cast<size_t>(n.QuadPart), 0);
    DWORD used = 0;
    if (!bytes.empty() && (!ReadFile(h.value, &bytes[0], static_cast<DWORD>(bytes.size()), &used, nullptr) ||
                           used != bytes.size()))
        Fail("Cannot read metadata", GetLastError());
    return bytes;
}
void WriteNew(const std::wstring& path, const std::string& bytes) {
    Handle h(CreateFileW(Native(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.value == INVALID_HANDLE_VALUE)
        Fail("Cannot create metadata", GetLastError());
    Regular(h.value);
    DWORD used = 0;
    if (!WriteFile(h.value, bytes.data(), static_cast<DWORD>(bytes.size()), &used, nullptr) ||
        used != bytes.size())
        Fail("Cannot write metadata", GetLastError());
    if (!FlushFileBuffers(h.value))
        Fail("Cannot flush metadata", GetLastError());
}
} // namespace

struct ToolManager::Impl {
    enum class Action { Prepare, Launch, Close, Force, Persist };
    struct Command {
        Action action;
        size_t index;
        bool automatic = false;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::deque<Command> queue;
    std::vector<ToolState> states;
    std::vector<Definition> definitions;
    std::vector<Process> processes;
    HINSTANCE module = nullptr;
    HWND window = nullptr;
    UINT message = 0;
    std::wstring root;
    bool initialized = false, dryRun = false, dirty = false;
    int lobby = -1, observedLobby = -1;
    bool followPending[2] = {false, false};
    ULONGLONG lastNotify = 0;

    Impl() {
        for (size_t i = 0; i < 4; ++i) {
            ToolState s;
            s.id = static_cast<ToolId>(i);
            s.name = i == 0 ? L"Rose" : i == 1 ? L"Akari" : i == 2 ? L"LTK patcher" : L"ABC";
            s.state = L"Not initialized";
            states.push_back(std::move(s));
        }
    }
    void Changed() {
        dirty = true;
    }
    void Notify() {
        HWND target = nullptr;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ULONGLONG now = GetTickCount64();
            if (!dirty || now - lastNotify < 100)
                return;
            dirty = false;
            lastNotify = now;
            target = window;
            msg = message;
        }
        if (target && msg)
            PostMessageW(target, msg, 0, 0);
    }
    void Progress(size_t i, int percent) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            states[i].percent = percent;
            Changed();
        }
        Notify();
    }
    void Error(size_t i, const std::exception& e, DWORD code) {
        std::lock_guard<std::mutex> lock(mutex);
        states[i].error = Wide(e.what()) + L" (Windows error " + std::to_wstring(code) + L")";
        states[i].state = code == ERROR_CANCELLED ? L"Cancelled" : L"Failed";
        Changed();
    }
    // mutex must be held. Edges survive a busy preparation/launch without becoming retry loops.
    void ReconcileFollow() {
        if (!initialized || dryRun || stop.load() || observedLobby < 0)
            return;
        for (size_t i = 0; i < 2; ++i) {
            auto& s = states[i];
            if (!s.followLobby || !followPending[i] || s.busy)
                continue;
            followPending[i] = false;
            s.busy = true;
            s.error.clear();
            s.state = L"Queued";
            queue.push_back({observedLobby == 1 ? Action::Launch : Action::Close, i, true});
            Changed();
        }
        wake.notify_one();
    }
    bool AutomaticAllowed(const Command& command) {
        if (!command.automatic)
            return true;
        std::lock_guard<std::mutex> lock(mutex);
        size_t i = command.index;
        if (!states[i].followLobby || dryRun || stop.load())
            return false;
        if (observedLobby < 0) {
            followPending[i] = true; // Unknown defers work, but never creates a lifecycle edge.
            return false;
        }
        int wanted = command.action == Action::Launch ? 1 : 0;
        if (observedLobby != wanted)
            return false;
        followPending[i] = false; // This command now satisfies the latest matching edge.
        return true;
    }
    bool Enqueue(Action action, size_t i) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!initialized || stop.load() || i >= definitions.size() ||
            (dryRun && action != Action::Prepare && action != Action::Persist))
            return false;
        auto& s = states[i];
        if (!s.bundled || s.busy)
            return false;
        if (action == Action::Launch && definitions[i].entry.empty()) {
            s.error = L"This is a prepare-only dependency; no launch entry is defined.";
            s.state = L"Prepare only";
            Changed();
            wake.notify_one();
            return false;
        }
        s.busy = true;
        s.percent = -1;
        s.error.clear();
        s.state = L"Queued";
        queue.push_back({action, i});
        Changed();
        wake.notify_one();
        return true;
    }
    File Expected(size_t i, const std::wstring& path, std::set<size_t> stack = {}) {
        if (!stack.insert(i).second)
            Fail("Dependency cycle");
        for (const auto& f : definitions[i].files)
            if (SamePath(f.path, path))
                return f;
        for (const auto& b : definitions[i].bindings)
            if (SamePath(b.to, path)) {
                File f = Expected(b.tool, b.from, stack);
                f.path = b.to;
                return f;
            }
        Fail("Dependency source not listed in manifest");
    }
    // Runtime targets are a small, strict manifest-only check; never an alternate signed profile.
    void CheckRuntimeCoherence(size_t i, const std::wstring& directory) {
        const auto& d = definitions[i];
        std::wstring prefix;
        if (d.id == "rose" && d.version == L"1.4.9")
            prefix = L"_internal\\injection\\tools\\";
        else if (d.id != "ltk" || (d.version != L"1.29.0" && d.version != L"1.29.1" && d.version != L"1.30.4"))
            return;
        try {
            for (const wchar_t* name : {L"ltk_patcher_host.exe", L"ltk_patcher_dll.dll"}) {
                const auto path = prefix + name;
                bool present = false;
                for (const auto& file : d.files) present = present || SamePath(file.path, path);
                for (const auto& binding : d.bindings) present = present || SamePath(binding.to, path);
                if (present)
                    CheckFile(directory, Expected(i, path), true, stop);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            states[i].ready = false;
            Changed();
            throw;
        }
    }
    bool ComponentRunning(size_t i) {
        const auto& d = definitions[i];
        std::wstring prefix = Lower(d.directory + L"\\");
        for (const auto& p : processes)
            if (Lower(p.image).compare(0, prefix.size(), prefix) == 0)
                return true;
        return false;
    }
    // Called only for a newly-created unpublished stage, never an existing component/user directory.
    void CopyBinding(size_t destinationTool, const Binding& binding, const std::wstring& destination) {
        const auto& source = definitions[binding.tool];
        File expected = Expected(binding.tool, binding.from);
        CheckFile(source.directory, expected, true, stop);
        Directories dirs;
        dirs.Ensure(Parent(source.directory + L"\\" + binding.from), false);
        dirs.Ensure(Parent(destination + L"\\" + binding.to), true);
        std::wstring target = destination + L"\\" + binding.to;
        DWORD attributes = GetFileAttributesW(Native(target).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            expected.path = binding.to;
            CheckFile(destination, expected, true, stop);
            return;
        } else {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                Fail("Cannot inspect embedded dependency", error);
        }
        Handle input(CreateFileW(Native(source.directory + L"\\" + binding.from).c_str(), GENERIC_READ,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr));
        if (input.value == INVALID_HANDLE_VALUE)
            Fail("Cannot open dependency", GetLastError());
        Regular(input.value);
        // Validate the same locked handle that will be copied, not a previously-closed observation.
        LARGE_INTEGER size{}, start{};
        if (!GetFileSizeEx(input.value, &size))
            Fail("Cannot size dependency", GetLastError());
        if (size.QuadPart < 0 || static_cast<ULONGLONG>(size.QuadPart) != expected.size ||
            Digest(input.value, nullptr, &stop) != expected.sha)
            Fail("Dependency changed before binding", ERROR_CRC);
        if (!SetFilePointerEx(input.value, start, nullptr, FILE_BEGIN))
            Fail("Cannot rewind dependency", GetLastError());
        std::wstring temporary = target + L".bbs-bind-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount64());
        Handle output(CreateFileW(Native(temporary).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (output.value == INVALID_HANDLE_VALUE)
            Fail("Cannot create staged dependency binding", GetLastError());
        Regular(output.value);
        BYTE buffer[65536];
        for (;;) {
            if (stop.load())
                Fail("Preparation cancelled", ERROR_CANCELLED);
            DWORD read = 0, written = 0;
            if (!ReadFile(input.value, buffer, sizeof(buffer), &read, nullptr))
                Fail("Cannot read dependency", GetLastError());
            if (!read)
                break;
            if (!WriteFile(output.value, buffer, read, &written, nullptr) || read != written)
                Fail("Cannot write dependency", GetLastError());
        }
        if (!FlushFileBuffers(output.value))
            Fail("Cannot flush dependency", GetLastError());
        CloseHandle(output.value);
        output.value = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(Native(temporary).c_str(), Native(target).c_str(),
                         MOVEFILE_WRITE_THROUGH))
            Fail("Cannot publish staged dependency binding; existing files were preserved", GetLastError());
    }
    void PrepareComponent(size_t i, std::set<size_t>& stack) {
        if (stop.load())
            Fail("Preparation cancelled", ERROR_CANCELLED);
        if (!stack.insert(i).second)
            Fail("Dependency cycle");
        const auto& d = definitions[i];
        try {
            for (const auto& b : d.bindings) {
                Expected(b.tool, b.from);
                PrepareComponent(b.tool, stack);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            states[i].ready = false;
            Changed();
            throw;
        }
        stack.erase(i);
        bool alreadyReady;
        {
            std::lock_guard<std::mutex> lock(mutex);
            alreadyReady = states[i].ready;
            if (!alreadyReady) {
                states[i].state = L"Preparing";
                Changed();
            }
        }
        if (alreadyReady) {
            EnsureAbcLogDirectories(d, d.directory);
            CheckRuntimeCoherence(i, d.directory);
            return;
        }
        struct BusyScope {
            Impl& owner;
            size_t index;
            bool previous;
            ~BusyScope() {
                std::lock_guard<std::mutex> lock(owner.mutex);
                owner.states[index].busy = previous;
                owner.Changed();
            }
        };
        bool wasBusy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wasBusy = states[i].busy;
            states[i].busy = true;
            Changed();
        }
        BusyScope busy{*this, i, wasBusy};
        Directories parents;
        parents.Ensure(Parent(d.directory), true);
        DWORD attributes = GetFileAttributesW(Native(d.directory).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                Fail("Existing component path is unsafe");
            Directories current;
            current.Ensure(d.directory, false);
            Json marker = Json::parse(ReadSmall(d.directory + L"\\.bbs-component.json"));
            if (!MarkerAccepted(d, marker))
                Fail("Existing component is incomplete or belongs to another manifest; preserve it and "
                     "repair manually");
            for (const auto& f : d.files)
                CheckFile(d.directory, f, true, stop, true);
            for (const auto& b : d.bindings) {
                File f = Expected(b.tool, b.from);
                f.path = b.to;
                CheckFile(d.directory, f, true, stop);
            }
            EnsureAbcLogDirectories(d, d.directory);
        } else {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                Fail("Cannot inspect component directory", error);
            if (ComponentRunning(i))
                Fail("Component is running; preparation refused", ERROR_BUSY);
            // Never remove or overwrite an old component/user directory, even on cancellation.
            std::wstring stage;
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                stage = Parent(d.directory) + L"\\.stage-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
                if (CreateDirectoryW(Native(stage).c_str(), nullptr))
                    break;
                if (GetLastError() != ERROR_ALREADY_EXISTS || attempt == 99)
                    Fail("Cannot create staging directory", GetLastError());
            }
            {
                Cabinet cabinet(Load(module, d.resource), d, stage, stop,
                                [this, i](int percent) { Progress(i, percent); });
                Extract(cabinet);
                // CAB contents are checked only against the embedded manifest.
                for (const auto& f : d.files)
                    CheckFile(stage, f, true, stop);
                for (const auto& b : d.bindings)
                    CopyBinding(i, b, stage);
                // Validate staged bindings without modifying any published directory.
                for (const auto& f : d.files)
                    CheckFile(stage, f, false, stop);
                for (const auto& b : d.bindings) {
                    File f = Expected(b.tool, b.from);
                    f.path = b.to;
                    CheckFile(stage, f, true, stop);
                }
                if (stop.load())
                    Fail("Preparation cancelled", ERROR_CANCELLED);
                EnsureAbcLogDirectories(d, stage);
                Json marker{{"format", 1}, {"stamp", d.stamp}, {"files", FileCount(d)}};
                WriteNew(stage + L"\\.bbs-component.json", marker.dump());
            }
            if (!MoveFileExW(Native(stage).c_str(), Native(d.directory).c_str(), MOVEFILE_WRITE_THROUGH))
                Fail("Cannot publish staged component; existing data was preserved", GetLastError());
        }
        std::lock_guard<std::mutex> lock(mutex);
        states[i].ready = true;
        states[i].percent = 100;
        states[i].error.clear();
        states[i].state = L"Ready";
        Changed();
    }
    void Poll() {
        processes = Processes();
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < definitions.size(); ++i) {
            DWORD pid = 0;
            if (!definitions[i].entry.empty())
                for (const auto& p : processes)
                    if (SamePath(p.image, definitions[i].directory + L"\\" + definitions[i].entry)) {
                        pid = p.pid;
                        break;
                    }
            auto& s = states[i];
            if (s.pid != pid || s.running != (pid != 0)) {
                s.pid = pid;
                s.running = pid != 0;
                if (!s.busy)
                    s.state = pid ? L"Running" : s.ready ? L"Ready" : L"Not prepared";
                Changed();
            }
        }
    }
    void Close(size_t i, bool force) {
        const auto& d = definitions[i];
        if (d.entry.empty())
            Fail("Prepare-only dependency cannot be closed", ERROR_NOT_SUPPORTED);
        const std::wstring target = d.directory + L"\\" + d.entry;
        for (const auto& p : processes) {
            if (!SamePath(p.image, target))
                continue;
#ifdef TOOL_MANAGER_TEST
            if (testHooks.close) {
                DWORD error = testHooks.close(p.pid, force);
                if (error)
                    Fail("Test close failed", error);
                continue;
            }
#endif
            // Re-open and re-query immediately: never act on a recycled PID or a generic name.
            Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | (force ? PROCESS_TERMINATE : 0), FALSE,
                                 p.pid));
            if (h.value == INVALID_HANDLE_VALUE)
                Fail("Cannot open component process", GetLastError());
            std::wstring image(32768, 0);
            DWORD n = static_cast<DWORD>(image.size());
            if (!QueryFullProcessImageNameW(h.value, 0, &image[0], &n))
                Fail("Cannot verify component process", GetLastError());
            image.resize(n);
            if (!SamePath(image, target))
                continue;
            if (force) {
                if (!TerminateProcess(h.value, 1))
                    Fail("Cannot stop component process", GetLastError());
            } else {
                CloseWindows context{p.pid};
                EnumWindows(CloseWindow, reinterpret_cast<LPARAM>(&context));
                if (context.error)
                    Fail("Cannot request component close", context.error);
                if (!context.any)
                    Fail("No component window; process is still running", ERROR_NOT_FOUND);
            }
        }
    }
    void Persist() {
        Json config{{"format", 1}, {"followLobby", Json::object()}};
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < 3; ++i)
                config["followLobby"][definitions[i].id] = states[i].followLobby;
        }
        Directories directory;
        directory.Ensure(root, true);
        std::wstring target = root + L"\\tool-center.json";
        DWORD attributes = GetFileAttributesW(Native(target).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            Handle h(CreateFileW(Native(target).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (h.value == INVALID_HANDLE_VALUE)
                Fail("Cannot inspect tool-center config", GetLastError());
            Regular(h.value);
        }
        std::wstring temp = target + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64());
        WriteNew(temp, config.dump(2));
        if (!MoveFileExW(Native(temp).c_str(), Native(target).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            Fail("Cannot save tool-center configuration", GetLastError());
    }
    void LoadPreferences() {
        try {
            Directories directory;
            directory.Ensure(root, false);
            Json config = Json::parse(ReadSmall(root + L"\\tool-center.json"));
            if (config.value("format", 0) != 1)
                return;
            bool values[2] = {false, false};
            for (size_t i = 0; i < 2; ++i)
                values[i] = !dryRun && config.at("followLobby").value(definitions[i].id, false);
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < 2; ++i)
                states[i].followLobby = values[i];
            Changed();
        } catch (...) { /* Missing or invalid preferences never authorize automatic actions. */
        }
    }
    void Run() {
        LoadPreferences();
        auto nextPoll = std::chrono::steady_clock::now();
        while (!stop.load()) {
            Command command{};
            bool have = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100),
                              [&] { return stop.load() || !queue.empty(); });
                if (stop.load())
                    break;
                if (!queue.empty()) {
                    command = queue.front();
                    queue.pop_front();
                    have = true;
                }
            }
            try {
                if (have) {
                    size_t i = command.index;
                    if (command.action == Action::Persist)
                        Persist();
                    else if (command.action == Action::Prepare || command.action == Action::Launch) {
                        Poll();
                        std::set<size_t> stack;
                        PrepareComponent(i, stack);
                        if (command.action == Action::Launch && !stop.load() && AutomaticAllowed(command)) {
                            SYSTEM_INFO system{};
                            GetNativeSystemInfo(&system);
                            if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
                                Fail("Bundled tools require x64 Windows", ERROR_BAD_EXE_FORMAT);
                            const auto& d = definitions[i];
                            bool reopenFallback = false;
                            if (ComponentRunning(i) && !command.automatic) {
                                if (d.id != "akari" && d.id != "abc")
                                    Fail("Rose is already running; use its existing window or tray icon", ERROR_ALREADY_EXISTS);
                                bool matched = false, restored = false;
                                for (const auto& process : processes)
                                    if (SamePath(process.image, d.directory + L"\\" + d.entry)) {
                                        matched = true;
                                        if (ReopenOwned(process.pid, d.directory + L"\\" + d.entry, d.id == "abc")) {
                                            restored = true;
                                            break;
                                        }
                                    }
                                if (!matched)
                                    Fail("Component is running; no verified main process to reopen", ERROR_BUSY);
                                if (!restored) {
                                    if (!d.legacyAkari)
                                        Fail("Component is running without a restorable window; use its tray icon", ERROR_NOT_FOUND);
                                    // Only the pinned unchanged Akari release has verified single-instance reopening.
                                    reopenFallback = true;
                                }
                            }
                            if (!ComponentRunning(i) || reopenFallback) {
                                File entry = Expected(i, d.entry);
                                Directories launchDirectories;
                                launchDirectories.Ensure(Parent(d.directory + L"\\" + d.entry), false);
                                CheckFile(d.directory, entry, reopenFallback, stop);
                                Handle executable(CreateFileW(Native(d.directory + L"\\" + d.entry).c_str(),
                                                              GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                                              nullptr));
                                if (executable.value == INVALID_HANDLE_VALUE)
                                    Fail("Cannot lock launch entry", GetLastError());
                                Regular(executable.value);
                                if (reopenFallback && Digest(executable.value, nullptr, &stop) != entry.sha)
                                    Fail("Akari reopen entry changed; existing process preserved", ERROR_CRC);
                                DWORD error = ERROR_SUCCESS;
                                if (AutomaticAllowed(command)) {
                                    CheckRuntimeCoherence(i, d.directory);
                                    error = Execute(d.directory + L"\\" + d.entry, d.elevated, window, d.id == "akari");
                                }
                                if (error)
                                    Fail("Tool launch failed or UAC was cancelled", error);
                            }
                        }
                    } else {
                        Poll();
                        if (AutomaticAllowed(command))
                            Close(i, command.action == Action::Force);
                    }
                }
                if (have || std::chrono::steady_clock::now() >= nextPoll) {
                    Poll();
                    nextPoll = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                }
            } catch (const Failure& e) {
                if (have)
                    Error(command.index, e, e.code);
            } catch (const std::exception& e) {
                if (have)
                    Error(command.index, e, ERROR_INVALID_DATA);
            } catch (...) {
                if (have) {
                    Failure e("Unexpected tool worker failure", ERROR_GEN_FAILURE);
                    Error(command.index, e, e.code);
                }
            }
            if (have && command.action != Action::Persist) {
                std::lock_guard<std::mutex> lock(mutex);
                auto& s = states[command.index];
                s.busy = false;
                if (s.error.empty()) {
                    if (command.action == Action::Close || command.action == Action::Force)
                        s.state = s.running ? L"Close requested; still running" : L"Stopped";
                    else
                        s.state = s.running                          ? L"Running"
                                  : command.action == Action::Launch ? L"Launch requested"
                                                                     : L"Ready";
                }
                Changed();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ReconcileFollow();
            }
            Notify();
        }
        std::lock_guard<std::mutex> lock(mutex);
        queue.clear();
        for (auto& s : states) {
            s.busy = false;
            if (s.state == L"Queued")
                s.state = L"Cancelled";
        }
        Changed();
    }
};
ToolManager::ToolManager() : impl(std::make_unique<Impl>()) {}
ToolManager::~ToolManager() {
    Shutdown();
}
bool ToolManager::Initialize(HINSTANCE module, HWND notifyWindow, UINT notifyMessage,
                             const std::wstring& dataRoot, bool dryRun) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->initialized)
        return false;
    try {
        impl->root = Absolute(dataRoot);
        impl->definitions = Parse(Load(module, 3100), impl->root);
        impl->module = module;
        impl->window = notifyWindow;
        impl->message = notifyMessage;
        impl->dryRun = dryRun;
        impl->stop.store(false);
        impl->lobby = -1;
        impl->observedLobby = -1;
        impl->followPending[0] = impl->followPending[1] = false;
        for (size_t i = 0; i < impl->definitions.size(); ++i) {
            auto& s = impl->states[i];
            const auto& d = impl->definitions[i];
            s = ToolState{};
            s.id = static_cast<ToolId>(i);
            s.name = d.name;
            s.version = d.version;
            s.directory = d.directory;
            s.bundled = true;
            s.elevated = d.elevated;
            s.state = L"Not prepared";
            s.error.clear();
            s.followLobby = false;
        }
        impl->initialized = true;
        impl->Changed();
        impl->worker = std::thread([this] { impl->Run(); });
        return true;
    } catch (const Failure& e) {
        impl->initialized = false;
        for (auto& s : impl->states) {
            s.bundled = false;
            s.state = L"Unavailable";
            s.error = Wide(e.what()) + L" (Windows error " + std::to_wstring(e.code) + L")";
        }
    } catch (const std::exception& e) {
        impl->initialized = false;
        for (auto& s : impl->states) {
            s.bundled = false;
            s.state = L"Unavailable";
            s.error = Wide(e.what());
        }
    }
    return false;
}
std::vector<ToolState> ToolManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->states;
}
bool ToolManager::Prepare(ToolId id) {
    return impl->Enqueue(Impl::Action::Prepare, Index(id));
}
bool ToolManager::Launch(ToolId id) {
    return impl->Enqueue(Impl::Action::Launch, Index(id));
}
bool ToolManager::RequestClose(ToolId id) {
    return impl->Enqueue(Impl::Action::Close, Index(id));
}
bool ToolManager::ForceStop(ToolId id) {
    return impl->Enqueue(Impl::Action::Force, Index(id));
}
bool ToolManager::SetFollowLobby(ToolId id, bool enabled) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    size_t i = Index(id);
    if (!impl->initialized || impl->stop.load() || impl->dryRun || i >= 2)
        return false;
    impl->states[i].followLobby = enabled;
    if (!enabled)
        impl->followPending[i] = false;
    impl->queue.push_back({Impl::Action::Persist, i});
    impl->Changed();
    impl->wake.notify_one();
    return true;
}
void ToolManager::ObserveLobby(int rawState) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->initialized || impl->dryRun || impl->stop.load())
        return;
    impl->observedLobby = rawState == 0 || rawState == 1 ? rawState : -1;
    if (impl->observedLobby < 0)
        return;
    if (impl->lobby != rawState) {
        impl->lobby = rawState;
        for (size_t i = 0; i < 2; ++i)
            if (impl->states[i].followLobby)
                impl->followPending[i] = true;
    }
    impl->ReconcileFollow();
    impl->Changed();
}
void ToolManager::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stop.store(true);
        impl->initialized = false;
        impl->wake.notify_all();
    }
    if (impl->worker.joinable()) {
        // Cancel this worker's synchronous file I/O; never stop any third-party process here.
        CancelSynchronousIo(impl->worker.native_handle());
        impl->worker.join();
    }
}
} // namespace core
