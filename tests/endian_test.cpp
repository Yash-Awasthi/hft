#include "core/endian.hpp"

#include <gtest/gtest.h>

TEST(Endian, LoadsBigEndianBytes) {
    const unsigned char bytes[] = {0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(hft::load_be32(bytes), 0x01020304u);
}

TEST(Endian, SwapIsConstexpr) { static_assert(hft::be32_to_host(0x01020304u) == 0x04030201u); }
