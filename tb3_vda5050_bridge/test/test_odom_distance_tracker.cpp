#include <gtest/gtest.h>

#include <cmath>

#include "tb3_vda5050_bridge/odom_distance_tracker.hpp"

using tb3_vda5050_bridge::OdomDistanceTracker;

TEST(OdomDistanceTracker, NoUpdatesYieldsZero)
{
  OdomDistanceTracker tracker;
  EXPECT_DOUBLE_EQ(tracker.take(), 0.0);
}

TEST(OdomDistanceTracker, FirstUpdateEstablishesBaselineOnly)
{
  OdomDistanceTracker tracker;
  tracker.update(1.0, 1.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 0.0);
}

TEST(OdomDistanceTracker, StraightLineAccumulatesExactDistance)
{
  OdomDistanceTracker tracker;
  tracker.update(0.0, 0.0);
  tracker.update(1.0, 0.0);
  tracker.update(3.0, 0.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 3.0);
}

// Verifies accumulated path length instead of straight-line displacement.
TEST(OdomDistanceTracker, LShapedMoveSumsLegsNotStraightLine)
{
  OdomDistanceTracker tracker;
  tracker.update(0.0, 0.0);
  tracker.update(2.0, 0.0);   // 2 m east
  tracker.update(2.0, 3.0);   // 3 m north

  const double driven = tracker.take();
  EXPECT_NEAR(driven, 5.0, 1e-9);
  EXPECT_GT(driven, std::hypot(2.0, 3.0));
}

TEST(OdomDistanceTracker, CurrentPeeksWithoutResetting)
{
  OdomDistanceTracker tracker;
  tracker.update(0.0, 0.0);
  tracker.update(1.0, 0.0);
  EXPECT_DOUBLE_EQ(tracker.current(), 1.0);
  EXPECT_DOUBLE_EQ(tracker.current(), 1.0);   // current() does not reset the accumulator.
  tracker.update(3.0, 0.0);
  EXPECT_DOUBLE_EQ(tracker.current(), 3.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 3.0);
  EXPECT_DOUBLE_EQ(tracker.current(), 0.0);
}

TEST(OdomDistanceTracker, TakeResetsAccumulatorButKeepsBaseline)
{
  OdomDistanceTracker tracker;
  tracker.update(0.0, 0.0);
  tracker.update(1.0, 0.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 1.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 0.0);

  // take() resets distance but preserves the last position.
  tracker.update(1.0, 1.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 1.0);
}
