#include "lgrtk/version.h"

#include <gtest/gtest.h>

TEST(VersionTest, MatchesProject) { EXPECT_STREQ(lgrtk::kVersion, "0.2.0"); }
