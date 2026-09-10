#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"

#include <chrono>

namespace vda5050_fleet_adapter_full_control::mqtt {

MqttClient::MqttClient(std::string broker_url, std::string client_id,
                        std::optional<std::string> username,
                        std::optional<std::string> password)
{
    _client = std::make_shared<::mqtt::async_client>(broker_url, client_id);
    _client->set_callback(*this);

    _conn_opts.set_clean_session(true);
    _conn_opts.set_keep_alive_interval(60);
    _conn_opts.set_automatic_reconnect(true);
    if (username.has_value())
    {
        _conn_opts.set_user_name(*username);
    }
    if (password.has_value())
    {
        _conn_opts.set_password(*password);
    }
}

MqttClient::~MqttClient()
{
    shutdown();
}

void MqttClient::set_on_message(MessageCallback cb)
{
    _on_message = std::move(cb);
}

void MqttClient::set_on_connected(ConnectedCallback cb)
{
    _on_connected = std::move(cb);
}

void MqttClient::set_on_connection_lost(ConnectionLostCallback cb)
{
    _on_connection_lost = std::move(cb);
}

void MqttClient::set_on_error(ErrorCallback cb)
{
    _on_error = std::move(cb);
}

void MqttClient::connect()
{
    if (_shutdown) { return; }
    if (_client->is_connected()) { return; }
    _client->connect(_conn_opts)->wait();
}

void MqttClient::shutdown()
{
    if (_shutdown.exchange(true)) { return; }
    if (_client && _client->is_connected())
    {
        try
        {
            _client->disconnect()->wait_for(std::chrono::seconds(1));
        }
        catch (const ::mqtt::exception &) { }
    }
}

bool MqttClient::is_connected() const
{
    return _client->is_connected();
}

void MqttClient::report_error(const std::string &context, const std::string &what) noexcept
{
    if (!_on_error) { return; }
    try
    {
        _on_error(context, what);
    }
    catch (...) { }
}


bool MqttClient::publish(const std::string &topic, const std::string &payload, int qos)
{
    if (!_client->is_connected()) 
    { 
        return false; 
    }
    try
    {
        auto m = ::mqtt::make_message(topic, payload);
        m->set_qos(qos);
        _client->publish(m);
        return true;
    }
    catch (const ::mqtt::exception &e)
    {
        report_error("publish " + topic, e.what());
        return false;
    }
}

bool MqttClient::subscribe(const std::string &topic, int qos)
{
    {
        std::lock_guard<std::mutex> lock(_subs_mutex);
        _subscriptions[topic] = qos;
    }
    if (!_client->is_connected()) 
    { 
        return false; 
    }
    try
    {
        _client->subscribe(topic, qos);
        return true;
    }
    catch (const ::mqtt::exception &e)
    {
        report_error("subscribe " + topic, e.what());
        return false;
    }
}

void MqttClient::resubscribe_all()
{
    std::map<std::string, int> subs_copy;
    {
        std::lock_guard<std::mutex> lock(_subs_mutex);
        subs_copy = _subscriptions;
    }
    for (const auto &[topic, qos] : subs_copy)
    {
        try
        {
            _client->subscribe(topic, qos);
        }
        catch (const ::mqtt::exception &e)
        {
            report_error("resubscribe " + topic, e.what());
        }
    }
}


// ─── ::mqtt::callback ───────────────────────────────────────────────────────

void MqttClient::connected(const std::string &)
{
    if (_shutdown) { return; }
    resubscribe_all();
    try
    {
        if (_on_connected) 
        { 
            _on_connected(); 
        }
    }
    catch (const std::exception &e)
    {
        report_error("on_connected callback", e.what());
    }
}


void MqttClient::connection_lost(const std::string &cause)
{
    if (_shutdown) { return; }
    try
    {
        if (_on_connection_lost) 
        { 
            _on_connection_lost(cause); 
        }
    }
    catch (const std::exception &e)
    {
        report_error("on_connection_lost callback", e.what());
    }
}

void MqttClient::message_arrived(::mqtt::const_message_ptr msg)
{
    if (_shutdown) { return; }
    try
    {
        if (_on_message) 
        { 
            _on_message(msg->get_topic(), msg->get_payload_str()); 
        }
    }
    catch (const std::exception &e)
    {
        report_error("on_message callback (" + msg->get_topic() + ")", e.what());
    }
}

}  // namespace vda5050_fleet_adapter_full_control::mqtt
