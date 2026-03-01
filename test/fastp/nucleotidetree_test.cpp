#include <gtest/gtest.h>
#include "nucleotidetree.h"

TEST(NucleotideTreeTest, getDominantPath) {
    NucleotideTree tree(nullptr);
    for (int i = 0; i < 100; i++) {
        tree.addSeq("AAAATTTT");
        tree.addSeq("AAAATTTTGGGG");
        tree.addSeq("AAAATTTTGGGGCCCC");
        tree.addSeq("AAAATTTTGGGGCCAA");
    }
    tree.addSeq("AAAATTTTGGGACCCC");

    bool reachedLeaf = true;
    string path = tree.getDominantPath(reachedLeaf);
    EXPECT_EQ(path, "AAAATTTTGGGGCC");
}
