#include <gtest/gtest.h>
#include "evaluator.h"

TEST(EvaluatorTest, int2seqRoundTrip) {
    EXPECT_TRUE(Evaluator::test());
}
