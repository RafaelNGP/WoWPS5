// PS5: the payload SDK platform layer owns the heap (tools/ps5/link.sh wraps
// malloc & co. into ps5platform's direct-memory heap), so there is no libc
// arena growth to count. The PS4 port's diagnostics read zeros.
#include "heap_growth.hpp"

namespace wowee::platform::ps4 {
HeapGrowthStats heapGrowthStats() noexcept { return HeapGrowthStats{0, 0, 0, 0}; }
} // namespace wowee::platform::ps4
