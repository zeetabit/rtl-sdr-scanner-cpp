#include "mqtt.h"

#include <logger.h>

constexpr auto KEEP_ALIVE = 60;
constexpr auto LOOP_TIMEOUT_MS = 1;
constexpr auto QOS = 2;
constexpr auto TOPIC = "sdr/config";
constexpr auto RECONNECT_INTERVAL = std::chrono::seconds(1);
constexpr auto QUEUE_MAX_SIZE = 1000;

Mqtt::Mqtt(const Config& config)
    : m_client(mosquitto_new(nullptr, true, this)), m_isRunning(true), m_thread([this, config]() {
        mosquitto_username_pw_set(m_client, config.mqttUsername().c_str(), config.mqttPassword().c_str());
        mosquitto_connect_callback_set(m_client, [](mosquitto *, void *p, int) { reinterpret_cast<Mqtt *>(p)->onConnect(); });
        mosquitto_disconnect_callback_set(m_client, [](mosquitto *, void *p, int) { reinterpret_cast<Mqtt *>(p)->onDisconnect(); });
        mosquitto_message_callback_set(m_client, [](mosquitto *, void *p, const struct mosquitto_message *m) { reinterpret_cast<Mqtt *>(p)->onMessage(m); });
        mosquitto_connect(m_client, config.mqttHostname().c_str(), config.mqttPort(), KEEP_ALIVE);
        while (m_isRunning) {
          mosquitto_loop(m_client, LOOP_TIMEOUT_MS, 1);
          while (m_isRunning && !m_messages.empty()) {
            const auto &[topic, data] = m_messages.front();
            mosquitto_publish(m_client, nullptr, topic.c_str(), data.size(), data.data(), QOS, false);
            std::unique_lock lock(m_mutex);
            m_messages.pop();
          }
        }
      }) {}

Mqtt::~Mqtt() {
  m_isRunning = false;
  m_thread.join();
  mosquitto_disconnect(m_client);
  mosquitto_destroy(m_client);
}

void Mqtt::publish(const std::string &topic, const std::string &data) {
  std::unique_lock lock(m_mutex);
  if (m_messages.size() < QUEUE_MAX_SIZE) {
    m_messages.emplace(topic, std::vector<uint8_t>{data.begin(), data.end()});
    Logger::debug("mqtt", "queue size: {}", m_messages.size());
  }
}

void Mqtt::publish(const std::string &topic, const std::vector<uint8_t> &data) {
  std::unique_lock lock(m_mutex);
  if (m_messages.size() < QUEUE_MAX_SIZE) {
    m_messages.emplace(topic, data);
    Logger::debug("mqtt", "queue size: {}", m_messages.size());
  }
}

void Mqtt::publish(const std::string &topic, const std::vector<uint8_t> &&data) {
  std::unique_lock lock(m_mutex);
  if (m_messages.size() < QUEUE_MAX_SIZE) {
    m_messages.emplace(topic, std::move(data));
    Logger::debug("mqtt", "queue size: {}", m_messages.size());
  }
}

void Mqtt::onConnect() {
  Logger::info("mqtt", "connected");
  mosquitto_subscribe(m_client, nullptr, TOPIC, QOS);
}

void Mqtt::onDisconnect() {
  if (!m_isRunning) {
    return;
  }
  Logger::warn("mqtt", "disconnected");
  while (m_isRunning && mosquitto_reconnect(m_client) != MOSQ_ERR_SUCCESS) {
    Logger::info("mqtt", "reconnecting");
    std::this_thread::sleep_for(RECONNECT_INTERVAL);
  }
  Logger::info("mqtt", "reconnecting success");
  std::unique_lock lock(m_mutex);
  while (!m_messages.empty()) {
    m_messages.pop();
  }
}

void Mqtt::onMessage(const mosquitto_message *message) { Logger::info("mqtt", "topic: {}, data: {}", message->topic, static_cast<char *>(message->payload)); }