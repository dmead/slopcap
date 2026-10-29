// How much RAM the machine has, and how much of it is free: a recording sizes its frame buffer from them.
#pragma once

#include <cstdint>
#include <optional>

namespace slopcap {

std::optional<uint64_t> totalRam();       ///< physical memory, in bytes
std::optional<uint64_t> availableRam();   ///< what programs can still get without the system paging: free plus reclaimable cache

}  // namespace slopcap
