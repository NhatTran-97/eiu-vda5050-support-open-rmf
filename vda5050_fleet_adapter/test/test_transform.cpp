#include <gtest/gtest.h>

#include <cmath>

#include "vda5050_fleet_adapter/transform.hpp"

using vda5050_fleet_adapter::Transform;

TEST(Transform, IdentityIsNoOp)
{
  Transform tf;  // identity
  const auto r = tf.to_robot(1.5, -2.5, 0.3);
  EXPECT_NEAR(r[0], 1.5, 1e-9);
  EXPECT_NEAR(r[1], -2.5, 1e-9);
  EXPECT_NEAR(r[2], 0.3, 1e-9);
}

TEST(Transform, RoundTrip)
{
  Transform tf(0.7 /*rad*/, 2.0 /*scale*/, 3.0, -4.0);
  const double x = 5.0, y = 6.0, th = 1.1;
  const auto r = tf.to_robot(x, y, th);
  const auto back = tf.to_rmf(r[0], r[1], r[2]);
  EXPECT_NEAR(back[0], x, 1e-9);
  EXPECT_NEAR(back[1], y, 1e-9);
  EXPECT_NEAR(back[2], th, 1e-9);
}

TEST(Transform, TranslationAndRotationApplied)
{
  Transform tf(M_PI_2, 1.0, 1.0, 2.0);  // +90deg, +(1,2)
  const auto r = tf.to_robot(1.0, 0.0, 0.0);
  // R(90)*(1,0) = (0,1); + (1,2) = (1,3); theta + pi/2
  EXPECT_NEAR(r[0], 1.0, 1e-9);
  EXPECT_NEAR(r[1], 3.0, 1e-9);
  EXPECT_NEAR(r[2], M_PI_2, 1e-9);
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
