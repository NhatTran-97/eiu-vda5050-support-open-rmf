#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "vda5050_fleet_adapter_full_control/util/log_throttle.hpp"

using vda5050_fleet_adapter_full_control::util::LogThrottle;
using std::chrono::seconds;

namespace {
const std::chrono::steady_clock::time_point kStart{std::chrono::hours(1)};
}  // namespace

TEST(LogThrottleTest, TheFirstMessageOfAKeyIsLogged)
{
    LogThrottle throttle(seconds(30));
    EXPECT_EQ(throttle.admit("a", kStart), 0u);
}

TEST(LogThrottleTest, RepeatsWithinTheIntervalAreHeldBackAndCounted)
{
    LogThrottle throttle(seconds(30));
    EXPECT_EQ(throttle.admit("a", kStart), 0u);
    EXPECT_FALSE(throttle.admit("a", kStart + seconds(1)).has_value());
    EXPECT_FALSE(throttle.admit("a", kStart + seconds(29)).has_value());
    EXPECT_FALSE(throttle.admit("a", kStart + seconds(29)).has_value());
    EXPECT_EQ(throttle.admit("a", kStart + seconds(30)), 3u);
    EXPECT_FALSE(throttle.admit("a", kStart + seconds(31)).has_value());
    EXPECT_EQ(throttle.admit("a", kStart + seconds(60)), 1u);
}

TEST(LogThrottleTest, KeysAreIndependent)
{
    LogThrottle throttle(seconds(30));
    EXPECT_EQ(throttle.admit("a", kStart), 0u);
    EXPECT_EQ(throttle.admit("b", kStart), 0u);
    EXPECT_FALSE(throttle.admit("a", kStart + seconds(1)).has_value());
    EXPECT_FALSE(throttle.admit("b", kStart + seconds(1)).has_value());
}

TEST(LogThrottleTest, KeysBeyondTheLimitShareOneEntry)
{
    LogThrottle throttle(seconds(30), 3);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(throttle.admit("k" + std::to_string(i), kStart), 0u);
    }
    EXPECT_EQ(throttle.admit("extra1", kStart + seconds(1)), 0u);
    EXPECT_FALSE(throttle.admit("extra2", kStart + seconds(2)).has_value());
    EXPECT_FALSE(throttle.admit("extra3", kStart + seconds(3)).has_value());
    EXPECT_FALSE(throttle.admit("extra4", kStart + seconds(25)).has_value());
}

TEST(LogThrottleTest, ExpiredKeysMakeRoomForNewOnes)
{
    LogThrottle throttle(seconds(30), 2);
    EXPECT_EQ(throttle.admit("a", kStart), 0u);
    EXPECT_EQ(throttle.admit("b", kStart), 0u);
    EXPECT_EQ(throttle.admit("c", kStart + seconds(31)), 0u);
    EXPECT_FALSE(throttle.admit("c", kStart + seconds(32)).has_value());
}

TEST(LogThrottleTest, ALimitOfZeroKeysStillWorks)
{
    LogThrottle throttle(seconds(30), 0);
    EXPECT_EQ(throttle.admit("a", kStart), 0u);
    EXPECT_FALSE(throttle.admit("a", kStart + seconds(1)).has_value());
}

TEST(LogThrottleTest, AtMostOneMoreThanTheKeyLimitIsLoggedPerInterval)
{
    LogThrottle throttle(seconds(30), 16);
    int logged = 0;
    for (int i = 0; i < 100000; ++i)
    {
        logged += throttle.admit("topic/" + std::to_string(i), kStart + std::chrono::microseconds(i)).has_value() ? 1 : 0;
    }
    EXPECT_EQ(logged, 17);
}

TEST(LogThrottleTest, ALongFloodIsBoundedInEveryInterval)
{
    LogThrottle throttle(seconds(30), 16);
    int logged = 0;
    for (int i = 0; i < 100000; ++i)
    {
        logged += throttle.admit("topic/" + std::to_string(i), kStart + std::chrono::milliseconds(i)).has_value() ? 1 : 0;
    }
    // 100 s of messages span four intervals.
    EXPECT_LE(logged, 4 * 17);
}
