// Stage 0 smoke tests: they prove the harness itself builds, links and runs.
// Real behavioural tests arrive with the code they cover.
#include <string_view>

#include "redis_lite/version.hpp"
#include "testing.hpp"

TEST(harness_runs) {
    CHECK(true);
    CHECK_EQ(1 + 1, 2);
}

TEST(version_header_is_consistent) {
    CHECK_EQ(redis_lite::kVersionMajor, 0);
    CHECK_EQ(redis_lite::kVersionMinor, 1);
    CHECK_EQ(redis_lite::kVersionPatch, 0);
    CHECK_EQ(std::string_view{redis_lite::kVersion}, std::string_view{"0.1.0"});
}
