#include "utils/system_info.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <processthreadsapi.h>
#  include <sysinfoapi.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  elif defined(__APPLE__)
#    include <pthread.h>
#  endif
#endif

namespace logger::utils {

std::uint64_t GetPid() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t GetTid() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    std::uint64_t tid = 0;
    ::pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(::pthread_self()));
#endif
}

std::size_t GetPageSize() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    return static_cast<std::size_t>(si.dwPageSize);
#else
    return static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#endif
}

}  // namespace logger::utils
