#include "mmap/mmap_region.h"

#include <system_error>
#include <utility>

#include "utils/system_info.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace logger::mmap {

namespace {

std::size_t RoundUpToPage(std::size_t n) {
    const std::size_t page = utils::GetPageSize();
    if (page == 0) return n;
    return (n + page - 1) / page * page;
}

#if defined(_WIN32)
[[noreturn]] void ThrowLastError(const char* what) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), what);
}
#else
[[noreturn]] void ThrowErrno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}
#endif

}  // namespace

MmapRegion MmapRegion::Create(const std::filesystem::path& path, std::size_t size_bytes) {
    if (size_bytes == 0) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "MmapRegion::Create: size_bytes == 0");
    }
    const std::size_t mapped = RoundUpToPage(size_bytes);

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        // ignore ec — Create() below will fail with the real error if the dir is unusable
    }

    MmapRegion r;
    r.path_ = path;
    r.size_ = mapped;

#if defined(_WIN32)
    HANDLE file = ::CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ThrowLastError("CreateFileW");
    }

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(mapped);
    if (!::SetFilePointerEx(file, li, nullptr, FILE_BEGIN) ||
        !::SetEndOfFile(file)) {
        const auto err = ::GetLastError();
        ::CloseHandle(file);
        throw std::system_error(static_cast<int>(err), std::system_category(),
                                "SetEndOfFile");
    }

    const DWORD size_hi = static_cast<DWORD>(static_cast<std::uint64_t>(mapped) >> 32);
    const DWORD size_lo = static_cast<DWORD>(mapped & 0xFFFFFFFFu);

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE,
                                          size_hi, size_lo, nullptr);
    if (mapping == nullptr) {
        const auto err = ::GetLastError();
        ::CloseHandle(file);
        throw std::system_error(static_cast<int>(err), std::system_category(),
                                "CreateFileMappingW");
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapped);
    if (view == nullptr) {
        const auto err = ::GetLastError();
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        throw std::system_error(static_cast<int>(err), std::system_category(),
                                "MapViewOfFile");
    }

    r.file_handle_    = file;
    r.mapping_handle_ = mapping;
    r.data_           = static_cast<std::byte*>(view);
#else
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) ThrowErrno("open");

    if (::ftruncate(fd, static_cast<off_t>(mapped)) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category(), "ftruncate");
    }

    void* addr = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category(), "mmap");
    }

    r.fd_   = fd;
    r.data_ = static_cast<std::byte*>(addr);
#endif

    return r;
}

MmapRegion::~MmapRegion() {
    Close();
}

MmapRegion::MmapRegion(MmapRegion&& other) noexcept
    : path_(std::move(other.path_)),
      data_(other.data_),
      size_(other.size_)
#if defined(_WIN32)
    , file_handle_(other.file_handle_),
      mapping_handle_(other.mapping_handle_)
#else
    , fd_(other.fd_)
#endif
{
    other.data_ = nullptr;
    other.size_ = 0;
#if defined(_WIN32)
    other.file_handle_    = nullptr;
    other.mapping_handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
}

MmapRegion& MmapRegion::operator=(MmapRegion&& other) noexcept {
    if (this != &other) {
        Close();
        path_  = std::move(other.path_);
        data_  = other.data_;
        size_  = other.size_;
#if defined(_WIN32)
        file_handle_    = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        other.file_handle_    = nullptr;
        other.mapping_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MmapRegion::Sync(std::size_t offset, std::size_t length) {
    if (data_ == nullptr || length == 0) return;
    if (offset >= size_) return;
    if (offset + length > size_) length = size_ - offset;

#if defined(_WIN32)
    if (!::FlushViewOfFile(data_ + offset, length)) {
        ThrowLastError("FlushViewOfFile");
    }
#else
    // msync requires a page-aligned starting address.
    const std::size_t page = utils::GetPageSize();
    const std::size_t aligned_off = page ? (offset / page * page) : offset;
    const std::size_t pad = offset - aligned_off;
    if (::msync(data_ + aligned_off, length + pad, MS_SYNC) != 0) {
        ThrowErrno("msync");
    }
#endif
}

void MmapRegion::Close() noexcept {
#if defined(_WIN32)
    if (data_)            { ::UnmapViewOfFile(data_);     data_ = nullptr; }
    if (mapping_handle_)  { ::CloseHandle(mapping_handle_); mapping_handle_ = nullptr; }
    if (file_handle_)     { ::CloseHandle(file_handle_);    file_handle_    = nullptr; }
#else
    if (data_) { ::munmap(data_, size_); data_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    size_ = 0;
}

}  // namespace logger::mmap
