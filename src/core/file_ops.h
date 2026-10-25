// Native Win32 file operations. Checked APIs are preferred for new callers.
#pragma once
#include "../common/winutil.h"
#include "path_policy.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace fileops {
struct FileError {
    std::wstring operation;
    std::wstring path;
    DWORD code = ERROR_SUCCESS;
    std::wstring message;
};
using CancelFn = std::function<bool()>; // true means cancel
using ProgressFn = std::function<bool(int done, int total, const std::wstring& current)>;
struct ListResult {
    std::vector<std::wstring> files;
    std::vector<std::wstring> directories;
    bool complete = false;
    bool cancelled = false;
    std::vector<FileError> errors;
};
struct MirrorResult {
    int copied = 0;
    std::vector<std::wstring> copiedPaths; // committed relative paths, including partial transfers
    int deleted = 0;
    int busy = 0;         // sharing/lock violations ONLY, not all failures
    bool skipped = false; // completely enumerated empty source: destination untouched
    bool cancelled = false;
    std::vector<FileError> errors;
    bool complete = false; // true only if the entire requested operation succeeded
    std::wstring summary;
};

// Internal ownership metadata/temp names never enter business file lists.
// Reparse points, enumeration errors, duplicate directory identities and
// case-colliding names make complete=false. Partial lists MUST NOT drive deletion.
ListResult ListFilesChecked(const std::wstring& dir, const CancelFn& cancel = nullptr);
// Only saves/shards subtrees and the six managed root files; unrelated entries are not traversed.
// Keeps the same complete/cancel/error contract and strict checks inside the managed scope.
ListResult ListBusinessFilesChecked(const std::wstring& dir, const CancelFn& cancel = nullptr);
std::vector<std::wstring> ListFiles(const std::wstring& dir); // legacy: empty on error
int CountFiles(const std::wstring& dir); // legacy: 0 on error, GetLastError distinguishes
bool EnsureDir(const std::wstring& dir); // creates directories, NOT a writability claim

// Keep this alive across a complete multi-file job to prevent substitution
// between individual checked calls. Acquire failure preserves the old lease.
class DirectoryLease {
  public:
    DirectoryLease();
    ~DirectoryLease();
    DirectoryLease(DirectoryLease&&) noexcept;
    DirectoryLease& operator=(DirectoryLease&&) noexcept;
    DirectoryLease(const DirectoryLease&) = delete;
    DirectoryLease& operator=(const DirectoryLease&) = delete;
    bool Acquire(const std::wstring& dir, std::wstring& error, bool requireDedicated = false);
    void Reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Claims only a newly created or truly empty directory. Unknown nonempty
// directories are refused. Existing ownership markers must validate exactly.
bool PrepareDedicatedRoot(const std::wstring& dir, std::wstring& error);
// Explicit user-selected data root: permits unrelated existing contents in place.
// Only creates/validates our hidden marker and probes writability; never scans, moves or deletes
// user entries. Callers MUST restrict all later mutations to the managed business/metadata scope.
// Path, reparse/hardlink, conflicting-marker and handle-pinning protections remain unchanged.
bool PrepareSelectedRoot(const std::wstring& dir, std::wstring& error);
// Claims an EXISTING directory whose contents the caller verified are exactly the managed business
// set (plus an empty leftover data folder). Only the hidden ownership marker is created; user bytes
// are never moved, renamed or deleted. A conflicting marker is refused.
bool ClaimBusinessCacheRoot(const std::wstring& dir, std::wstring& error);
bool IsDedicatedRoot(const std::wstring& dir, std::wstring& error);
// Removes a dedicated root that holds nothing but our own marker/lock/temp files. Any business or
// unknown entry refuses the whole operation, so this can never delete user data.
bool RemoveIfEmptyDedicatedRoot(const std::wstring& dir, std::wstring& error);
bool ProbeWritable(const std::wstring& dir, std::wstring& error);
inline bool IsWritable(const std::wstring& dir, std::wstring& error) {
    return ProbeWritable(dir, error);
}

// MirrorDir requires a valid ownership marker IN dst itself. It never claims
// arbitrary directories. Empty source protection is retained. No deletion after
// incomplete enumeration, copy errors or cancellation. Checks cancellation
// during enumeration, each I/O chunk, immediately before commit and each delete.
// Callback may also run during enumeration (total=0), deletion and I/O polling;
// callers must not assume it is called exactly once per file.
MirrorResult MirrorDir(const std::wstring& src, const std::wstring& dst,
                       const ProgressFn& onProgress = nullptr);
// Restore/copy-only: no ownership marker required on dst, never deletes extras.
MirrorResult CopyTreeChecked(const std::wstring& src, const std::wstring& dst,
                             const ProgressFn& onProgress = nullptr);
int CopyTree(const std::wstring& src, const std::wstring& dst, std::vector<std::wstring>* busyOut = nullptr,
             const ProgressFn& onProgress = nullptr);

// Stages a same-directory CREATE_NEW temp; complete read/write + FlushFileBuffers
// precede atomic same-volume rename. Failure leaves the old target unchanged.
// Existing directory/reparse targets are refused, never removed to make room.
bool CopyFileChecked(const std::wstring& src, const std::wstring& dst, FileError& error,
                     const CancelFn& cancel = nullptr);
bool CopyFileForce(const std::wstring& src, const std::wstring& dst);
// Metadata helpers use fixed-parent relative opens/commits. Caller owns out.
// OpenFileChecked accepts OPEN_EXISTING, CREATE_NEW and OPEN_ALWAYS only; it
// never truncates an existing file before validation. Parent must already exist.
bool OpenFileChecked(const std::wstring& path, HANDLE& out, DWORD access, DWORD share, DWORD creation,
                     FileError& error);
bool RenameHandleChecked(HANDLE source, const std::wstring& destination, FileError& error,
                         bool replace = true);
bool OpenDirectoryChecked(const std::wstring& path, HANDLE& opened, DWORD access, DWORD share,
                          FileError& error);
bool MoveDirectoryChecked(const std::wstring& src, const std::wstring& dst, FileError& error,
                          const BY_HANDLE_FILE_INFORMATION* expected = nullptr);
bool RemoveEmptyDirectoryChecked(const std::wstring& path, FileError& error);
struct PinnedTreeFile {
    std::wstring relative;
    HANDLE handle = INVALID_HANDLE_VALUE; // borrowed only during validator callback
    BY_HANDLE_FILE_INFORMATION identity{};
    bool directory = false;
};
using ValidatePinnedTree =
    std::function<bool(const BY_HANDLE_FILE_INFORMATION&, const std::vector<PinnedTreeFile>&, FileError&)>;
// Preflight and hold exact managed objects through file and empty-directory deletion.
MirrorResult ClearTreeEntriesChecked(const std::wstring& root, const std::vector<std::wstring>& files,
                                     const std::vector<std::wstring>& directories,
                                     const CancelFn& cancel = nullptr,
                                     const std::function<void(const std::wstring&)>& beforeDelete = nullptr,
                                     bool removeRoot = false, const ValidatePinnedTree& validator = nullptr);
bool DeleteFileChecked(const std::wstring& path, FileError& error);
bool DeleteFileForce(const std::wstring& path);
bool Exists(const std::wstring& path);
bool IsDir(const std::wstring& path);
long long FileSize(const std::wstring& path);
bool FileReadable(const std::wstring& path);
} // namespace fileops
