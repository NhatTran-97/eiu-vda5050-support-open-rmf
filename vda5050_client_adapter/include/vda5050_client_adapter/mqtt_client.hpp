#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid including paho headers in every TU
namespace mqtt {
class async_client;
}

namespace vda5050_adapter {

/**
 * @brief Configuration for the MQTT connection.
 */
struct MqttConfig {
  std::string broker_url{"tcp://localhost:1883"};
  std::string client_id;
  std::string username;
  std::string password;
  int         keep_alive_interval{60};  ///< seconds
  int         connect_timeout{10};      ///< seconds
  int         reconnect_delay_min{1};   ///< seconds
  int         reconnect_delay_max{30};  ///< seconds
  bool        clean_session{false};
  int         mqtt_version{4};          ///< 3=MQTT 3.1, 4=MQTT 3.1.1, 5=MQTT 5.0

  // Last Will (used for CONNECTIONBROKEN)
  std::string will_topic;
  std::string will_payload;
  int         will_qos{1};
  bool        will_retained{false};
};

/**
 * @brief Inbound MQTT message.
 */
struct MqttMessage {
  std::string topic;
  std::string payload;
  int         qos{0};
  bool        retained{false};
};

/**
 * @brief Async MQTT client wrapping the Eclipse Paho C++ library.
 *
 * Features:
 *  - Automatic reconnect with exponential back-off
 *  - Per-topic message callbacks
 *  - Thread-safe publish queue
 *  - Last-Will-and-Testament support (for VDA5050 CONNECTIONBROKEN)
 */
class MqttClient {
public:
  using MessageCallback    = std::function<void(const MqttMessage&)>;
  using ConnectionCallback = std::function<void(bool connected)>;

  explicit MqttClient(const MqttConfig& config);
  ~MqttClient();

  // Non-copyable, non-movable
  // Move is deleted because Impl holds a back-reference (MqttClient& outer_) to this
  // object; moving would leave that reference dangling.
  MqttClient(const MqttClient&)            = delete;
  MqttClient& operator=(const MqttClient&) = delete;
  MqttClient(MqttClient&&)                 = delete;
  MqttClient& operator=(MqttClient&&)      = delete;

  /**
   * @brief Connect to the broker (non-blocking).
   * @param on_connected  Called on every connect / disconnect event.
   */
  void connect(ConnectionCallback on_connected = nullptr);

  /**
   * @brief Disconnect gracefully.
   * @param timeout_ms  Time to wait for pending publishes (ms).
   */
  void disconnect(int timeout_ms = 5000);

  /**
   * @brief Publish a message.
   * @param topic    MQTT topic.
   * @param payload  UTF-8 JSON string.
   * @param qos      QoS level (0 or 1).
   * @param retained Retain flag.
   * @return true if the message was enqueued / dispatched.
   */
  bool publish(const std::string& topic,
               const std::string& payload,
               int                qos      = 0,
               bool               retained = false);

  /**
   * @brief Subscribe to a topic pattern.
   * @param topic_filter MQTT topic filter (wildcards + and # supported).
   * @param qos          Desired QoS.
   * @param callback     Callback invoked on every matching message.
   */
  void subscribe(const std::string& topic_filter,
                 int                qos,
                 MessageCallback    callback);

  /**
   * @brief Unsubscribe from a topic filter.
   */
  void unsubscribe(const std::string& topic_filter);

  bool is_connected() const { return connected_.load(); }

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool>     connected_{false};

  friend class Impl;
};

}  // namespace vda5050_adapter
