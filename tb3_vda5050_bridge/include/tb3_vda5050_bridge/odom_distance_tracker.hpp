#ifndef TB3_VDA5050_BRIDGE__ODOM_DISTANCE_TRACKER_HPP_
#define TB3_VDA5050_BRIDGE__ODOM_DISTANCE_TRACKER_HPP_

namespace tb3_vda5050_bridge {

// Accumulate odometry distance for VDA5050 distanceSinceLastNode.
class OdomDistanceTracker
{
public:
  // Feed the latest odometry position; accumulates the delta from the previous call.
  void update(double x, double y);

  // Return the accumulated distance and reset it to zero.
  double take();

  // Read current leg distance without resetting the accumulator.
  double current() const;

private:
  double distance_{0.0};
  double last_x_{0.0};
  double last_y_{0.0};
  bool valid_{false};
};

}  // namespace tb3_vda5050_bridge

#endif  // TB3_VDA5050_BRIDGE__ODOM_DISTANCE_TRACKER_HPP_
