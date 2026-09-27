#ifndef TB3_VDA5050_BRIDGE__ODOM_DISTANCE_TRACKER_HPP_
#define TB3_VDA5050_BRIDGE__ODOM_DISTANCE_TRACKER_HPP_

namespace tb3_vda5050_bridge {

// Tracks odometry distance for VDA5050 distanceSinceLastNode.
class OdomDistanceTracker
{
public:
  // Adds the distance from the previous position.
  void update(double x, double y);

  // Returns the accumulated distance and resets it.
  double take();

  // Returns the accumulated distance without resetting it.
  double current() const;

private:
  double distance_{0.0};
  double last_x_{0.0};
  double last_y_{0.0};
  bool valid_{false};
};

}  // namespace tb3_vda5050_bridge

#endif  // TB3_VDA5050_BRIDGE__ODOM_DISTANCE_TRACKER_HPP_
