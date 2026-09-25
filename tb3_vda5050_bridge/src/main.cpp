#include <rclcpp/rclcpp.hpp>

#include "tb3_vda5050_bridge/bridge_node.hpp"

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<tb3_vda5050_bridge::BridgeNode>());
  rclcpp::shutdown();
  return 0;
}
