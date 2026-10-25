// winutil.h — Win32 路径/资源安全工具 (v2 全新实现)
// 解决三类反复出现的问题:
//   1. 长路径 (>MAX_PATH): 统一加 \\?\ 前缀, 让 Win32 API 接受 260+ 字符路径
//   2. 未检查返回值: 失败时返回空串而不是让调用方读到未初始化内存
//   3. 资源释放: RAII 包装 HANDLE/HBITMAP/HBRUSH/HRGN/HGLOBAL, 异常路径也不漏
#pragma once
#include <shlobj.h>
#include <string>
#include <vector>
#include <windows.h>

namespace winutil {

// ---------- 长路径支持 ----------
// 转换为 \\?\ 前缀形式, 突破 MAX_PATH (260) 限制。
// 规则 (与 Windows 文档一致):
//   - 已是 \\?\ 开头 → 原样返回 (幂等)
//   - UNC \\server\share → \\?\UNC\server\share
//   - 盘符绝对路径 C:\... → \\?\C:\...
//   - 相对路径 → 原样返回 (\\?\ 不支持相对路径; 依赖 CWD 的路径本就不可靠)
//
// BUG-FIX: \\?\ 前缀路径**只认反斜杠**。原实现直接拼接原串, 于是
//   "C:/Users/..." 这种正斜杠路径会变成 "\\?\C:/Users/..." ——
//   该路径不被 Win32 识别, GetFileAttributesW 返回 INVALID_FILE_ATTRIBUTES,
//   表现为"目录明明存在却找不到" (实测: raw 查询成功, \\?\ 查询失败)。
//   修正: 加前缀前把 '/' 全部换成 '\\'。
inline std::wstring LongPath(const std::wstring& p) {
    if (p.empty())
        return p;
    if (p.compare(0, 4, L"\\\\?\\") == 0)
        return p; // 已转换 (幂等)
    // 统一分隔符: \\?\ 路径只接受反斜杠
    std::wstring norm = p;
    for (auto& c : norm) {
        if (c == L'/')
            c = L'\\';
    }
    if (norm.size() >= 2 && norm[0] == L'\\' && norm[1] == L'\\') {
        return L"\\\\?\\UNC\\" + norm.substr(2);
    }
    if (norm.size() >= 2 && norm[1] == L':') {
        return L"\\\\?\\" + norm;
    }
    return norm; // 相对路径: 不加前缀 (但分隔符已归一化)
}

// 反向: 去掉 \\?\ 前缀 (用于展示/日志/传给不支持长路径的 API)
inline std::wstring StripLongPrefix(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) {
        if (p.compare(0, 8, L"\\\\?\\UNC\\") == 0)
            return L"\\\\" + p.substr(8);
        return p.substr(4);
    }
    return p;
}

// ---------- 安全获取已知文件夹 ----------
// 失败返回空串 (调用方必须处理); 绝不让调用方读到未初始化的栈缓冲
inline std::wstring SafeFolderPath(int csidl) {
    wchar_t buf[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, 0, buf)) || !buf[0])
        return L"";
    return std::wstring(buf);
}

// ---------- exe 路径 (长路径安全) ----------
// GetModuleFileNameW 在路径超过 MAX_PATH 时会截断且不保证 null 终止 (Win10 1607+
// 返回 n == 缓冲区大小 表示被截断)。这里循环扩容直到拿到完整路径。
inline std::wstring GetExePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (int attempt = 0; attempt < 9; attempt++) {
        DWORD n = GetModuleFileNameW(NULL, buf.data(), (DWORD)buf.size());
        if (n == 0)
            return L"";
        if (n < buf.size()) { // 完整路径 (含终止符空间)
            return std::wstring(buf.data(), n);
        }
        buf.resize(buf.size() * 2); // 被截断 → 扩容重试
        if (buf.size() > 65536)
            return L"";
    }
    return L"";
}

// exe 所在目录 (尾部分隔符已去掉); 失败返回空串
inline std::wstring GetExeDir() {
    std::wstring p = GetExePath();
    if (p.empty())
        return L"";
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"";
    return p.substr(0, slash);
}

// ---------- RAII 资源包装 ----------
// 用途: 消除"中途 return / 抛异常 → HANDLE/HBITMAP 泄漏"的整类问题。
// 显式 Release() 可提前释放; 析构兜底。

class ScopedHandle {
    HANDLE m_h = INVALID_HANDLE_VALUE;

  public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : m_h(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (m_h != INVALID_HANDLE_VALUE && m_h != NULL)
            CloseHandle(m_h);
    }
    HANDLE get() const {
        return m_h;
    }
    bool valid() const {
        return m_h != INVALID_HANDLE_VALUE && m_h != NULL;
    }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (m_h == h)
            return;
        if (m_h != INVALID_HANDLE_VALUE && m_h != NULL)
            CloseHandle(m_h);
        m_h = h;
    }
    HANDLE release() {
        HANDLE h = m_h;
        m_h = INVALID_HANDLE_VALUE;
        return h;
    }
};

// GDI 对象 (HBITMAP / HBRUSH / HPEN / HFONT / HRGN) — 用 DeleteObject 释放
class ScopedGdi {
    HGDIOBJ m_o = NULL;

  public:
    ScopedGdi() = default;
    explicit ScopedGdi(HGDIOBJ o) : m_o(o) {}
    ScopedGdi(const ScopedGdi&) = delete;
    ScopedGdi& operator=(const ScopedGdi&) = delete;
    ~ScopedGdi() {
        if (m_o)
            DeleteObject(m_o);
    }
    HGDIOBJ get() const {
        return m_o;
    }
    bool valid() const {
        return m_o != NULL;
    }
    void reset(HGDIOBJ o = NULL) {
        if (m_o == o)
            return;
        if (m_o)
            DeleteObject(m_o);
        m_o = o;
    }
    HGDIOBJ release() {
        HGDIOBJ o = m_o;
        m_o = NULL;
        return o;
    }
};

// ---------- 缓冲区安全拷贝 ----------
// 复制到定长 wchar_t 缓冲: 保证 null 终止, 永不溢出, 永不触发 invalid parameter handler。
// MinGW 下 wcsncpy_s/_countof 的边界行为与 MSVC 不同 (超长会触发 ERANGE → 目标被清空),
// 因此本项目一律不用 *_s 系列做字符串拷贝, 改用这个自实现。
inline void SafeCopyW(wchar_t* dst, size_t dstCount, const wchar_t* src) {
    if (!dst || dstCount == 0)
        return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t i = 0;
    for (; i + 1 < dstCount && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

// 安全追加: 把 src 追加到 dst (dst 为 std::wstring), 自动扩容
inline void SafeAppendW(std::wstring& dst, const wchar_t* src, size_t maxLen = 0) {
    if (!src || !*src)
        return;
    size_t n = 0;
    while ((maxLen == 0 || n < maxLen) && src[n])
        n++;
    dst.append(src, n);
}

} // namespace winutil
