#pragma once

#include <cstddef>
#include <filesystem>

namespace logger::mmap {

// 跨平台单段 mmap 的 RAII 封装。
// 构造（通过 Create）成功后保证：底层文件已扩容至 size_bytes 向上对齐到页，
// 内存映射 [data(), data()+size()) 可读可写；析构自动 unmap + 关闭句柄。
class MmapRegion {
public:
    // size_bytes 会向上对齐到 utils::GetPageSize() 的整数倍。
    // 失败抛 std::system_error。
    static MmapRegion Create(const std::filesystem::path& path, std::size_t size_bytes);

    MmapRegion() noexcept = default;
    ~MmapRegion();

    MmapRegion(MmapRegion&& other) noexcept;
    MmapRegion& operator=(MmapRegion&& other) noexcept;

    MmapRegion(const MmapRegion&)            = delete;
    MmapRegion& operator=(const MmapRegion&) = delete;

    std::byte*  data()  noexcept       { return data_; }
    const std::byte* data() const noexcept { return data_; }
    std::size_t size()  const noexcept { return size_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // 把 [offset, offset+length) 的脏页同步到磁盘。
    void Sync(std::size_t offset, std::size_t length);

    // 显式关闭+unmap，幂等。析构会自动调用。
    void Close() noexcept;

private:
    std::filesystem::path path_;
    std::byte*            data_{nullptr};
    std::size_t           size_{0};
#if defined(_WIN32)
    void* file_handle_    = nullptr;   // INVALID_HANDLE_VALUE-equivalent sentinel kept as nullptr internally
    void* mapping_handle_ = nullptr;
#else
    int   fd_ = -1;
#endif
};

}  // namespace logger::mmap
