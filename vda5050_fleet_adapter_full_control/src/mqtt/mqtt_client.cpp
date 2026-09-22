#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"

#include <chrono>

namespace vda5050_fleet_adapter_full_control::mqtt {

MqttClient::MqttClient(const std::string &broker_url, const std::string &client_id,
                        std::optional<std::string> username,
                        std::optional<std::string> password,
                        const MqttOptions &options): _client(std::make_shared<::mqtt::async_client>(broker_url, client_id))
{
    _client->set_callback(*this);
    _conn_opts.set_clean_session(true);
    _conn_opts.set_keep_alive_interval(static_cast<int>(options.keep_alive.count()));
    _max_payload_bytes = options.max_payload_bytes;
    _tls_enabled = options.tls.enabled;
    if (options.tls.enabled)
    {
        ::mqtt::ssl_options ssl;
        if (!options.tls.ca_file.empty())
        {
            ssl.set_trust_store(options.tls.ca_file);
        }
        if (!options.tls.client_cert.empty())
        {
            ssl.set_key_store(options.tls.client_cert);
            ssl.set_private_key(options.tls.client_key);
        }
        ssl.set_enable_server_cert_auth(true);
        ssl.set_verify(options.tls.verify_hostname);
        _conn_opts.set_ssl(ssl);
    }
    _conn_opts.set_connect_timeout(options.connect_timeout);
    _conn_opts.set_automatic_reconnect(options.retry_min, options.retry_max);
    
    if (username.has_value())
    {
        _conn_opts.set_user_name(*username);
    }
    if (password.has_value())
    {
        _conn_opts.set_password(*password);
    }
    _connected_worker = std::thread([this] { run_connected_worker(); });
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
    _client->connect(_conn_opts, nullptr, _connect_listener);
}

void MqttClient::ConnectListener::on_failure(const ::mqtt::token &token)
{
    if (_owner._shutdown) { return; }
    std::string what = ::mqtt::exception::error_str(token.get_return_code()) + " (rc " + std::to_string(token.get_return_code()) + ")";
    if (_owner._tls_enabled)
    {
        what += " -- TLS is on: check the CA file, that the broker's certificate names this host, the client certificate, and that the port speaks TLS";
    }
    _owner.report_error("connect to " + _owner._client->get_server_uri(), what);
}

void MqttClient::shutdown()
{
    if (_shutdown.exchange(true)) { return; }
    {
        std::lock_guard<std::mutex> lock(_connected_mutex);
    }
    _connected_cv.notify_all();
    if (_connected_worker.joinable())
    {
        if (std::this_thread::get_id() == _connected_worker.get_id())
        {
            _connected_worker.detach();
        }
        else
        {
            _connected_worker.join();
        }
    }
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

MqttClient::Stats MqttClient::stats() const
{
    return {_connects.value(), _connections_lost.value(), _oversize_dropped.value(), _errors.value()};
}

void MqttClient::report_error(const std::string &context, const std::string &what) noexcept
{
    _errors.add();
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


// ::mqtt::callback

void MqttClient::connected(const std::string &)
{
    if (_shutdown) 
    { 
        return; 
    }
    _connects.add();
    resubscribe_all();
    {
        std::lock_guard<std::mutex> lock(_connected_mutex);
        _connected_pending = true;
    }
    _connected_cv.notify_one();
}

void MqttClient::run_connected_worker()
{
    std::unique_lock<std::mutex> lock(_connected_mutex);
    while (true)
    {
        _connected_cv.wait(lock, [this] { return _connected_pending || _shutdown; });
        if (_shutdown) 
        { 
            return; 
        }
        _connected_pending = false;
        lock.unlock();
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
        lock.lock();
    }
}


void MqttClient::connection_lost(const std::string &cause)
{
    if (_shutdown) { return; }
    _connections_lost.add();
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
    const std::size_t size = msg->get_payload().size();
    if (size > _max_payload_bytes)
    {
        _oversize_dropped.add();
        report_error("dropped message on " + msg->get_topic(),
                     std::to_string(size) + " bytes exceed the limit of " + std::to_string(_max_payload_bytes) + " bytes");
        return;
    }
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
