// limits_shared.h — named constants for values that previously appeared as bare literals.
#pragma once
#include <cstddef>

namespace common {
// Maximum Windows long-path buffer size: 32767 characters plus the terminating NUL.
inline constexpr std::size_t kMaxPathChars = 32768;
// Milliseconds in one minute; used to convert configured minute intervals.
inline constexpr unsigned long long kMillisecondsPerMinute = 60000;
// Deadline for the startup configuration read before it is reported as a failure.
inline constexpr unsigned long long kConfigReadTimeoutMs = 5000;
// Delay before retrying a failed service start, in milliseconds.
inline constexpr int kStartRetryDelayMs = 5000;
// Stack reserve requested from the OS for the crash-handler path (32 KiB).
inline constexpr unsigned long kCrashStackGuaranteeBytes = 32768;
} // namespace common
