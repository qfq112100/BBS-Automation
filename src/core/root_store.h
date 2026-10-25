// External, identity-bound management records. Business roots never receive new metadata.
#pragma once
#include <memory>
#include <string>

namespace core::rootstore {
struct Record {
    std::wstring root, directory, identity;
    bool identityVariant = false; // Set only by Resolve after validating an explicitly authorized variant.
};
// Local fixed/removable drive folders only; UNC, mapped remote and unknown volumes fail closed.
bool ValidateLocalRoot(const std::wstring& path, std::wstring& error);
// managementRoot must be an application-owned dedicated directory, disjoint from dataRoot.
// New records publish only after complete initialization in a private .pending-root-record-* sibling.
// create=false performs no directory creation or ownership claim.
// Explicit confirmation may register a NEW physical-identity variant, never rebind or
// modify the old record. Existing valid variants remain usable without confirmation.
bool Resolve(const std::wstring& managementRoot, const std::wstring& dataRoot, Record& record,
             std::wstring& error, bool create = true, bool allowIdentityRebind = false);

class Lease {
  public:
    Lease();
    ~Lease();
    Lease(Lease&&) noexcept;
    Lease& operator=(Lease&&) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    // Lifetime physical-identity exclusion works across threads, sessions and metadata roots.
    // Legacy files are opened without creation and validated without changing their attributes.
    // Stop older application versions before migration; future mixed-version writes are unsupported.
    bool Acquire(const Record& record, std::wstring& error, bool businessCache = true);
    // Call ONLY after durable migration of the legacy sync state. Never touches that JSON.
    // Disposes only the exact validated marker/lock objects retained by Acquire.
    bool FinishLegacyCleanup(std::wstring& error);
    bool Matches(const Record& record) const noexcept;
    void Reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Lowercase SHA-256 hex. Empty result indicates failure; error is cleared on success.
std::string Digest(const std::string& bytes, std::wstring& error);
} // namespace core::rootstore
