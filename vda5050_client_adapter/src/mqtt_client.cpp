#include "vda5050_client_adapter/mqtt_client.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

#include <mqtt/async_client.h>

namespace vda5050_adapter {

// ─────────────────────────────────────────────────────────────────────────────
// Internal implementation (Paho callback class)
// ─────────────────────────────────────────────────────────────────────────────

class MqttClient::Impl: public mqtt::callback,public mqtt::iaction_listener {
public:
  struct SubscriptionEntry 
  {
    int                         qos{0};
    MqttClient::MessageCallback callback;
  };

  MqttClient& outer_;
  MqttConfig  config_;

  std::unique_ptr<mqtt::async_client> client_;
  mqtt::connect_options              conn_opts_;

  std::mutex                                      subs_mutex_;
  std::unordered_map<std::string,
    SubscriptionEntry>                            subscriptions_;

  MqttClient::ConnectionCallback on_connected_;
  std::atomic<bool>               reconnect_pending_{false};

  Impl(MqttClient& outer, const MqttConfig& cfg): outer_(outer), config_(cfg)
  {
    client_ = std::make_unique<mqtt::async_client>(
      config_.broker_url,
      config_.client_id,
      mqtt::create_options(config_.mqtt_version));

    client_->set_callback(*this);

    // Build connection options
    auto conn_builder = mqtt::connect_options_builder()
      .keep_alive_interval(std::chrono::seconds(config_.keep_alive_interval))
      .clean_session(config_.clean_session)
      .automatic_reconnect(
        std::chrono::seconds(config_.reconnect_delay_min),
        std::chrono::seconds(config_.reconnect_delay_max));

    if (!config_.username.empty()) 
    {
      conn_builder.user_name(config_.username).password(config_.password);
    }

    // Last Will (CONNECTIONBROKEN for VDA5050)
    if (!config_.will_topic.empty()) {
      auto will = mqtt::message(
        config_.will_topic,
        config_.will_payload,
        config_.will_qos,
        config_.will_retained);
      conn_builder.will(will);
    }

    conn_opts_ = conn_builder.finalize();
  }

  // ─── mqtt::callback interface ──────────────────────────────────────────────

  void connected(const std::string& /*cause*/) override 
  {
    outer_.connected_.store(true);
    std::cerr << "[MqttClient] Connected to " << config_.broker_url << "\n";

    std::vector<std::pair<std::string, int>> subscriptions;
    MqttClient::ConnectionCallback on_connected;
    {
      std::lock_guard<std::mutex> lock(subs_mutex_);
      subscriptions.reserve(subscriptions_.size());
      for (const auto& [filter, entry] : subscriptions_) 
      {
        subscriptions.emplace_back(filter, entry.qos);
      }
      on_connected = on_connected_;
    }

    for (const auto& [filter, qos] : subscriptions) 
    {
      try 
      {
        client_->subscribe(filter, qos);
      } catch (const mqtt::exception& ex) {
        std::cerr << "[MqttClient] re-subscribe() failed for '" << filter
                  << "': " << ex.what() << "\n";
      }
    }

    if (on_connected) on_connected(true);
  }

  void connection_lost(const std::string& cause) override {
    outer_.connected_.store(false);
    std::cerr << "[MqttClient] Connection lost: " << cause << "\n";
    MqttClient::ConnectionCallback on_connected;
    {
      std::lock_guard<std::mutex> lock(subs_mutex_);
      on_connected = on_connected_;
    }
    if (on_connected) on_connected(false);
    // Paho automatic_reconnect handles reconnection
  }

  void message_arrived(mqtt::const_message_ptr msg) override 
  {
    MqttMessage m
    {
      msg->get_topic(),
      msg->to_string(),
      msg->get_qos(),
      msg->is_retained()
    };

    std::vector<MqttClient::MessageCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(subs_mutex_);
      callbacks.reserve(subscriptions_.size());
      for (const auto& [filter, entry] : subscriptions_) {
        if (topic_matches(filter, m.topic) && entry.callback) 
        {
          callbacks.push_back(entry.callback);
        }
      }
    }
    for (const auto& cb : callbacks) cb(m);
  }

  void delivery_complete(mqtt::delivery_token_ptr /*tok*/) override {}

  // ─── mqtt::iaction_listener interface ─────────────────────────────────────

  void on_failure(const mqtt::token& tok) override {
    std::cerr << "[MqttClient] Action failed: " << tok.get_message_id() << "\n";
  }

  void on_success(const mqtt::token& /*tok*/) override {}

  // ─── Helpers ──────────────────────────────────────────────────────────────

  static bool topic_matches(const std::string& filter,
                             const std::string& topic)
  {
    // Simple iterative matcher
    size_t fi = 0, ti = 0;
    while (fi < filter.size() && ti < topic.size()) {
      if (filter[fi] == '#') {
        return true;  
      }
      if (filter[fi] == '+') {
  
        while (ti < topic.size() && topic[ti] != '/') ++ti;
        ++fi;
      } else if (filter[fi] == topic[ti]) {

        ++fi; ++ti;
      } else 
      {
        return false;
      }
    }

    if (fi < filter.size() && filter[fi] == '#') return true;
    return fi == filter.size() && ti == topic.size();
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// MqttClient public API
// ─────────────────────────────────────────────────────────────────────────────

MqttClient::MqttClient(const MqttConfig& config)
  : impl_(std::make_unique<Impl>(*this, config))
{}

MqttClient::~MqttClient() 
{
  if (is_connected()) 
  {
    disconnect(3000);
  }
}

void MqttClient::connect(ConnectionCallback on_connected) 
{
  {
    std::lock_guard<std::mutex> lock(impl_->subs_mutex_);
    impl_->on_connected_ = std::move(on_connected);
  }
  try 
  {
    impl_->client_->connect(impl_->conn_opts_, nullptr, *impl_);
  } catch (const mqtt::exception& ex) 
  {
    throw std::runtime_error(
      std::string("[MqttClient] connect() failed: ") + ex.what());
  }
}

void MqttClient::disconnect(int timeout_ms) 
{
  if (!is_connected()) return;
  try 
  {
    impl_->client_->disconnect()->wait_for(std::chrono::milliseconds(timeout_ms));
  } catch (...) { /* best-effort */ }
  connected_.store(false);
}

bool MqttClient::publish(const std::string& topic,
                         const std::string& payload,
                         int                qos,
                         bool               retained)
{
  if (!is_connected()) 
  {
    std::cerr << "[MqttClient] Cannot publish: not connected\n";
    return false;
  }
  try {
    auto msg = mqtt::make_message(topic, payload, qos, retained);
    impl_->client_->publish(msg);
    return true;
  } catch (const mqtt::exception& ex) 
  {
    std::cerr << "[MqttClient] publish() failed: " << ex.what() << "\n";
    return false;
  }
}

void MqttClient::subscribe(const std::string& topic_filter,
                            int                qos,
                            MessageCallback    callback)
{
  {
    std::lock_guard<std::mutex> lock(impl_->subs_mutex_);
    impl_->subscriptions_[topic_filter] = MqttClient::Impl::SubscriptionEntry{
      qos, std::move(callback)};
  }
  if (is_connected()) 
  {
    try 
    {
      impl_->client_->subscribe(topic_filter, qos);
    } catch (const mqtt::exception& ex) 
    {
      std::cerr << "[MqttClient] subscribe() failed: " << ex.what() << "\n";
    }
  }
}

void MqttClient::unsubscribe(const std::string& topic_filter) 
{
  {
    std::lock_guard<std::mutex> lock(impl_->subs_mutex_);
    impl_->subscriptions_.erase(topic_filter);
  }
  if (is_connected()) 
  {
    try 
    {
      impl_->client_->unsubscribe(topic_filter);
    } catch (const mqtt::exception& ex) 
    {
      std::cerr << "[MqttClient] unsubscribe() failed: " << ex.what() << "\n";
    }
  }
}

}  // namespace vda5050_client_adapter
