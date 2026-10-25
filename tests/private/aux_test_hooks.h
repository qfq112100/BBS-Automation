// Private deterministic seams, compiled only by explicitly opted-in auxiliary tests.
#pragma once
#ifdef LOL_AUX_TEST_HOOKS
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
namespace core::aux_test {
struct ScanFixture {
    std::vector<std::pair<std::wstring, UINT>> drives;
    std::vector<std::wstring> runningCandidates;
    size_t opened = 0, closed = 0, entries = 0, candidates = 0;
    size_t entryLimit = 400, candidateLimit = 2048;
    bool throwAfterOpen = false;
    std::function<ULONGLONG()> clock;
};
void SetScanFixture(ScanFixture* fixture);
std::wstring ParseDisplayIcon(const std::wstring& value);
// Runs outside allLogsMutex after the coherent handle/size snapshot; caller owns lifetime.
void SetTailSnapshotHook(std::function<void()> hook);
} // namespace core::aux_test
#endif
