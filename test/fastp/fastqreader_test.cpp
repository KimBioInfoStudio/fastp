#include <gtest/gtest.h>
#include "fastqreader.h"

TEST(FastqReaderTest, readPairedFiles) {
    FastqReader reader1("testdata/R1.fq");
    FastqReader reader2("testdata/R2.fq");
    int count = 0;
    while (true) {
        Read* r1 = reader1.read();
        Read* r2 = reader2.read();
        if (r1 == nullptr || r2 == nullptr) {
            delete r1;
            delete r2;
            break;
        }
        count++;
        delete r1;
        delete r2;
    }
    EXPECT_GT(count, 0);
}
