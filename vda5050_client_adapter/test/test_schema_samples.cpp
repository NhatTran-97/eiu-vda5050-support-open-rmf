// Writes every kind of message the client adapter publishes to VDA5050_SCHEMA_SAMPLES,
// for validate_schemas.py to check against the official VDA5050 2.1 JSON schemas.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>

#include "vda5050_client_adapter/json_converter.hpp"
#include "vda5050_client_adapter/vda5050_node.hpp"

namespace {

std::filesystem::path samples_dir()
{
  const char* dir = std::getenv("VDA5050_SCHEMA_SAMPLES");
  return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

void write_sample(const std::string& name, const nlohmann::json& message)
{
  std::ofstream(samples_dir() / (name + ".json")) << message.dump(2);
}

vda5050::Header header(uint32_t id)
{
  vda5050::Header h;
  h.header_id = id;
  h.timestamp = "2026-09-26T12:00:00.000Z";
  h.manufacturer = "ROBOTIS";
  h.serial_number = "0001";
  return h;
}

vda5050::AgvPosition position()
{
  vda5050::AgvPosition p;
  p.position_initialized = true;
  p.localization_score = 0.9;
  p.x = 1.0;
  p.y = 2.0;
  p.theta = 0.5;
  p.map_id = "tb3_world";
  return p;
}

vda5050::State idle_state()
{
  vda5050::State s;
  s.header = header(1);
  s.agv_position = position();
  s.velocity = vda5050::Velocity{0.0, 0.0, 0.0};
  s.battery_state.battery_charge = 80.0;
  s.battery_state.battery_voltage = 12.1;
  s.maps.push_back({"tb3_world", "1", "", vda5050::MapStatus::ENABLED});
  return s;
}

}  // namespace

TEST(SchemaSamples, ClientAdapterMessages)
{
  if (samples_dir().empty())
  {
    GTEST_SKIP() << "set VDA5050_SCHEMA_SAMPLES to a directory";
  }
  std::filesystem::create_directories(samples_dir());

  write_sample("state__idle", idle_state());

  auto driving = idle_state();
  driving.header = header(2);
  driving.order_id = "order-1";
  driving.order_update_id = 1;
  driving.last_node_id = "B";
  driving.last_node_sequence_id = 2;
  driving.driving = true;
  driving.distance_since_last_node = 0.7;
  vda5050::NodeState node;
  node.node_id = "C";
  node.sequence_id = 4;
  node.released = true;
  node.node_position = vda5050::NodePosition{4.0, 0.0, 1.57, true, 0.5, 0.35, "tb3_world", ""};
  driving.node_states.push_back(node);
  driving.edge_states.push_back({"e_B_C", 3, "", true, std::nullopt});
  driving.action_states.push_back({"a-1", "startCharging", "", vda5050::ActionStatus::FINISHED, "Simulated charging started"});
  driving.action_states.push_back({"a-2", "initPosition", "", vda5050::ActionStatus::FAILED, "refused while navigating"});
  vda5050::Error error;
  error.error_type = "navigationError";
  error.error_level = vda5050::ErrorLevel::WARNING;
  error.error_description = "step failed";
  error.error_hint = "check Nav2";
  error.error_references = {{"orderId", "order-1"}, {"nodeId", "C"}};
  driving.errors.push_back(error);
  driving.information.push_back({"debug", {{"nodeId", "C"}}, "stepping", vda5050::InfoLevel::DEBUG});
  vda5050::Load load;
  load.load_id = "box-1";
  load.load_type = "box";
  load.bounding_box_reference = vda5050::BoundingBoxReference{0.0, 0.0, 0.1, 0.0};
  load.load_dimensions = vda5050::LoadDimensions{0.2, 0.2, 0.1};
  load.weight = 0.3;
  driving.loads.push_back(load);
  driving.battery_state.charging = true;
  driving.safety_state.e_stop = vda5050::EStop::NONE;
  write_sample("state__driving_with_actions_errors_loads", driving);

  auto manual = idle_state();
  manual.header = header(3);
  manual.operating_mode = vda5050::OperatingMode::MANUAL;
  manual.paused = true;
  manual.safety_state.e_stop = vda5050::EStop::MANUAL;
  manual.safety_state.field_violation = true;
  manual.agv_position.reset();
  manual.velocity.reset();
  write_sample("state__manual_estop_without_position", manual);

  for (const auto& [name, state] : {std::pair<std::string, vda5050::ConnectionState>{"online", vda5050::ConnectionState::ONLINE},
                                    {"offline", vda5050::ConnectionState::OFFLINE},
                                    {"connectionbroken", vda5050::ConnectionState::CONNECTIONBROKEN}})
  {
    vda5050::Connection connection;
    connection.header = header(4);
    connection.connection_state = state;
    write_sample("connection__" + name, connection);
  }

  vda5050::Visualization visualization;
  visualization.header = header(5);
  visualization.agv_position = position();
  visualization.velocity = vda5050::Velocity{0.2, 0.0, 0.1};
  write_sample("visualization__position_and_velocity", visualization);
  vda5050::Visualization bare;
  bare.header = header(6);
  write_sample("visualization__header_only", bare);

  rclcpp::NodeOptions options;
  options.use_global_arguments(false);
  options.arguments({"--ros-args", "-r", "__ns:=/schema_samples", "--params-file", VDA5050_CLIENT_PARAMS_FILE});
  options.parameter_overrides({{"mqtt.broker_url", "tcp://127.0.0.1:1"}});
  auto adapter = std::make_shared<vda5050_adapter::VDA5050Node>(options);
  auto factsheet = adapter->build_factsheet_from_params();
  factsheet.header = header(7);
  write_sample("factsheet__from_parameters", factsheet);
  adapter.reset();
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
