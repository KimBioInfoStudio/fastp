#include <gtest/gtest.h>
#include "adaptertrimmer.h"

TEST(AdapterTrimmerTest, trimBySequence) {
    Read r("@name",
        "TTTTAACCCCCCCCCCCCCCCCCCCCCCCCCCCCAATTTTAAAATTTTCCACGGGGATACTACTG",
        "+",
        "///EEEEEEEEEEEEEEEEEEEEEEEEEE////EEEEEEEEEEEEE////E////EEEEEEEEE");
    string adapter = "TTTTCCACGGGGATACTACTG";
    bool trimmed = AdapterTrimmer::trimBySequence(&r, nullptr, adapter);
    EXPECT_TRUE(trimmed);
    EXPECT_EQ(*r.mSeq, "TTTTAACCCCCCCCCCCCCCCCCCCCCCCCCCCCAATTTTAAAA");
}

TEST(AdapterTrimmerTest, trimByMultiSequences) {
    Read read("@name",
        "TTTTAACCCCCCCCCCCCCCCCCCCCCCCCCCCCAATTTTAAAATTTTCCCCGGGGAAATTTCCCGGGAAATTTCCCGGGATCGATCGATCGATCGAATTCC",
        "+",
        "///EEEEEEEEEEEEEEEEEEEEEEEEEE////EEEEEEEEEEEEE////E////EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE");
    vector<string> adapterList;
    adapterList.push_back("GCTAGCTAGCTAGCTA");
    adapterList.push_back("AAATTTCCCGGGAAATTTCCCGGG");
    adapterList.push_back("ATCGATCGATCGATCG");
    adapterList.push_back("AATTCCGGAATTCCGG");
    AdapterTrimmer::trimByMultiSequences(&read, nullptr, adapterList);
    EXPECT_EQ(*read.mSeq, "TTTTAACCCCCCCCCCCCCCCCCCCCCCCCCCCCAATTTTAAAATTTTCCCCGGGG");
}
