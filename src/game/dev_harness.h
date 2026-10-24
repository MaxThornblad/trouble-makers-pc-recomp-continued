// Headless test harness (MM_TEST_SCRIPT / MM_TEST_DUMP / MM_TEST_TRACE /
// MM_TEST_DIFF); does nothing unless one of those is set. See dev_harness.cpp.
#pragma once

#include <cstdint>

namespace mm::dev_harness {

void tick(uint8_t* rdram);

} // namespace mm::dev_harness
