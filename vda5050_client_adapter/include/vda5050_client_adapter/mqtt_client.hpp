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
 * @brief Async MQTT client wrapping Eclipse Paho C++ library for VDA5050 communication.
 *
 * Manages MQTT connection lifecycle, subscribes to inbound topics (order, instantActions),
 * publishes outbound messages (state, connection, visualization, factsheet), and handles
 * reconnection with exponential back-off. Callbacks are invoked on message arrival and
 * connection state changes. Thread-safe: publish queue and subscription callbacks are
 * synchronized.
 *
 * Key features:
 *  - Automatic reconnect with exponential back-off (configurable delays)
 *  - Per-topic message callbacks with QoS and topic filter support
 *  - Thread-safe async publish queue
 *  - Last-Will-and-Testament (LWT) support for VDA5050 CONNECTIONBROKEN on disconnect
 *  - Retains published messages as configured
 */
class MqttClient {
public:
  /**
   * @brief Callback type for inbound MQTT messages.
   * @param message The received message (topic + payload).
   */
  using MessageCallback    = std::function<void(const MqttMessage&)>;

  /**
   * @brief Callback type for connection state changes.
   * @param connected true when connected, false when disconnected.
   */
  using ConnectionCallback = std::function<void(bool connected)>;

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /**
   * @brief Construct MQTT client with the given config (does not connect yet).
   * @param config MQTT connection and broker configuration.
   */
  explicit MqttClient(const MqttConfig& config);

  /**
   * @brief Disconnect if connected and clean up resources.
   */
  ~MqttClient();

  // Non-copyable, non-movable: Impl holds a back-reference to this object that would dangle.
  MqttClient(const MqttClient&)            = delete;
  MqttClient& operator=(const MqttClient&) = delete;
  MqttClient(MqttClient&&)                 = delete;
  MqttClient& operator=(MqttClient&&)      = delete;

  // ── Connection management ──────────────────────────────────────────────────

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

  // ── Publishing ─────────────────────────────────────────────────────────────

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

  // ── Subscription management ────────────────────────────────────────────────

  /**
   * @brief Subscribe to a topic pattern.
   * @param topic_filter MQTT topic filter (wildcards + and # supported).
   * @param qos          Desired QoS.
   * @param callback     Callback invoked on every matching message.
   */
  void subscribe(const std::string& topic_filter,
                 int qos, MessageCallback    callback);

  /**
   * @brief Unsubscribe from a topic filter.
   */
  void unsubscribe(const std::string& topic_filter);

  // ── State queries ──────────────────────────────────────────────────────────

  bool is_connected() const { return connected_.load(); }

private:
  // ── Internal ───────────────────────────────────────────────────────────────

  class Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool>     connected_{false};

  friend class Impl;
};

}  // namespace vda5050_adapter
