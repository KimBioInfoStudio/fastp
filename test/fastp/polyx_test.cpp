#include <gtest/gtest.h>
#include "polyx.h"

TEST(PolyXTest, trimPolyX) {
    Read r("@name",
        "ATTTTAAAAAAAAAATAAAAAAAAAAAAACAAAAAAAAAAAAAAAAAAAAAAAAAT",
        "+",
        "///EEEEEEEEEEEEEEEEEEEEEEEEEE////EEEEEEEEEEEEE////E////E");

    FilterResult fr(nullptr, false);
    PolyX::trimPolyX(&r, &fr, 10);

    EXPECT_EQ(*r.mSeq, "ATTTT");
    EXPECT_EQ(fr.getTotalPolyXTrimmedReads(), 1);
    EXPECT_EQ(fr.getTotalPolyXTrimmedBases(), 51);
}
