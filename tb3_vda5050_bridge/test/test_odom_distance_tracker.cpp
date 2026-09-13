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

// The real point of this class: an L-shaped move (2 m then 3 m = 5 m actually
// driven) must not collapse to the straight-line distance between the
// endpoints (hypot(2, 3) ~= 3.6 m) -- that was the bug distance_driven fixes.
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
  EXPECT_DOUBLE_EQ(tracker.current(), 1.0);   // unchanged by repeated peeks
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

  // Baseline survives take() -- the next delta is measured from the last
  // known position, not from a cold start.
  tracker.update(1.0, 1.0);
  EXPECT_DOUBLE_EQ(tracker.take(), 1.0);
}
