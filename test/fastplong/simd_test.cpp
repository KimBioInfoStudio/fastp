#include <gtest/gtest.h>
#include "simd.h"

TEST(SimdTest, allTests) {
    EXPECT_TRUE(fastp_simd::testSimd());
}
