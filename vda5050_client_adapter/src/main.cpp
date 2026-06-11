#include <rclcpp/rclcpp.hpp>
#include "vda5050_client_adapter/vda5050_node.hpp"

int main(int argc, char* argv[]) 
{
  rclcpp::init(argc, argv);

  auto node = std::make_shared<vda5050_adapter::VDA5050Node>
  (
    rclcpp::NodeOptions().use_intra_process_comms(false));

    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
