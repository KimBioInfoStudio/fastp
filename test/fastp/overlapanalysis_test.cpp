#include <gtest/gtest.h>
#include "overlapanalysis.h"

TEST(OverlapAnalysisTest, analyzeAndMerge) {
    string* r1 = new string("CAGCGCCTACGGGCCCCTTTTTCTGCGCGACCGCGTGGCTGTGGGCGCGGATGCCTTTGAGCGCGGTGACTTCTCACTGCGTATCGAGC");
    string* r2 = new string("ACCTCCAGCGGCTCGATACGCAGTGAGAAGTCACCGCGCTCAAAGGCATCCGCGCCCACAGCCACGCGGTCGCGCAGAAAAAGGGGTCC");
    string* qual1 = new string("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
    string* qual2 = new string("#########################################################################################");

    OverlapResult ov = OverlapAnalysis::analyze(r1, r2, 2, 30, 0.2);
    EXPECT_TRUE(ov.overlapped);
    EXPECT_EQ(ov.offset, 10);
    EXPECT_EQ(ov.overlap_len, 79);
    EXPECT_EQ(ov.diff, 1);

    Read read1(new string("name1"), r1, new string("+"), qual1);
    Read read2(new string("name2"), r2, new string("+"), qual2);
    Read* merged = OverlapAnalysis::merge(&read1, &read2, ov);
    ASSERT_NE(merged, nullptr);
    delete merged;
}
