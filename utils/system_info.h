#pragma once

#include <cstddef>
#include <cstdint>

namespace logger::utils {

std::uint64_t GetPid() noexcept;

std::uint64_t GetTid() noexcept;

std::size_t GetPageSize() noexcept;

}  // namespace logger::utils
