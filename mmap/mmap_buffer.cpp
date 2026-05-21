#include "mmap/mmap_buffer.h"

#include <algorithm>
#include <thread>

namespace logger::mmap {

namespace {

void WriteU32LE(std::byte* dst, std::uint32_t v) {
    dst[0] = static_cast<std::byte>(v & 0xFF);
    dst[1] = static_cast<std::byte>((v >> 8) & 0xFF);
    dst[2] = static_cast<std::byte>((v >> 16) & 0xFF);
    dst[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}

}  // namespace

MmapBuffer::MmapBuffer(const std::filesystem::path& path, std::size_t capacity_bytes)
    : region_(MmapRegion::Create(path, capacity_bytes)) {}

bool MmapBuffer::Write(const std::byte* data, std::size_t len) {
    if (len == 0 || len > 0xFFFFFFFFu) return false;

    // Two-phase fence with sealed_: increment writers_ first, then check sealed_.
    // Pairs with Seal(): store sealed_, then load writers_. seq_cst guarantees
    // either we see sealed=true here, or Seal sees writers>0 there.
    writers_.fetch_add(1, std::memory_order_seq_cst);
    if (sealed_.load(std::memory_order_seq_cst)) {
        writers_.fetch_sub(1, std::memory_order_release);
        return false;
    }

    const std::size_t need = 4 + len;
    const std::size_t cap  = region_.size();
    const std::size_t old  = offset_.fetch_add(need, std::memory_order_acq_rel);
    if (old + need > cap) {
        // Overrun. Intentionally do NOT roll back offset_ (avoids ABA where
        // another writer takes the freed slot, then drain reads stale bytes).
        writers_.fetch_sub(1, std::memory_order_release);
        return false;
    }

    std::byte* slot = region_.data() + old;
    WriteU32LE(slot, static_cast<std::uint32_t>(len));
    std::memcpy(slot + 4, data, len);

    writers_.fetch_sub(1, std::memory_order_release);
    return true;
}

void MmapBuffer::Seal() {
    // Stop accepting new writers, then drain in-flight ones.
    sealed_.store(true, std::memory_order_seq_cst);
    while (writers_.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
}

void MmapBuffer::ResetForReuse() {
    offset_.store(0, std::memory_order_relaxed);
    sealed_.store(false, std::memory_order_release);
    // writers_ is already 0 (Seal waited for it).
    // Zero the leading bytes of the region so stale frame headers from the
    // previous generation can't be mistaken for valid frames.
    if (region_.data() && region_.size() >= 4) {
        std::memset(region_.data(), 0, std::min<std::size_t>(region_.size(), 4096));
    }
}

void MmapBuffer::Sync() {
    const std::size_t used = std::min<std::size_t>(
        offset_.load(std::memory_order_acquire), region_.size());
    if (used > 0) region_.Sync(0, used);
}

std::size_t MmapBuffer::Used() const noexcept {
    return std::min<std::size_t>(offset_.load(std::memory_order_acquire), region_.size());
}

}  // namespace logger::mmap
