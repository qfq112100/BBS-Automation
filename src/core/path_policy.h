// Shared fail-closed Windows path policy. UTF-8 source.
#pragma once
#include <string>
#include <windows.h>

namespace pathpolicy {
// Absolute drive/UNC paths only. Rejects roots, NUL/control characters, device
// namespaces, ambiguous names and every existing reparse-point component.
// Nonexistent suffixes are allowed; existing components must be directories.
// All failures set GetLastError and error; exceptions never escape.
bool NormalizeRoot(const std::wstring& path, std::wstring& normalized, std::wstring& error);
bool ValidateRoot(const std::wstring& path, std::wstring& error);
// Resolves existing ancestors by handle, including DOS/short-name aliases.
// Rejects equality and either ancestor relationship, case-insensitively.
bool AreDisjoint(const std::wstring& a, const std::wstring& b, std::wstring& error);
bool EqualPath(const std::wstring& a, const std::wstring& b);
struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const;
};
// Helpers shared with the file library. NormalizePath also permits files in
// the final component; it validates spelling only, not filesystem existence.
bool NormalizePath(const std::wstring& path, std::wstring& normalized, std::wstring& error);
std::wstring LongPath(const std::wstring& normalized);
size_t RootLength(const std::wstring& normalized);
} // namespace pathpolicy
