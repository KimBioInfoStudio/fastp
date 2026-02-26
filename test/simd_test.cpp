#include <gtest/gtest.h>
#include "../src/simd.h"

TEST(SimdTest, allTests) {
    EXPECT_TRUE(fastplong_simd::testSimd());
}
