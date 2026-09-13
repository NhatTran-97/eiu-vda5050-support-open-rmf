#include "tb3_vda5050_bridge/odom_distance_tracker.hpp"

#include <cmath>

namespace tb3_vda5050_bridge {

void OdomDistanceTracker::update(double x, double y)
{
  if (valid_) {
    distance_ += std::hypot(x - last_x_, y - last_y_);
  }
  last_x_ = x;
  last_y_ = y;
  valid_ = true;
}

double OdomDistanceTracker::take()
{
  const double d = distance_;
  distance_ = 0.0;
  return d;
}

double OdomDistanceTracker::current() const
{
  return distance_;
}

}  // namespace tb3_vda5050_bridge
