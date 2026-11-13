#pragma once

#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>

#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#ifdef USE_SHELLY_EMULATOR_BLE
#include "esphome/components/esp32_ble_server/ble_server.h"
#endif

namespace esphome {
namespace shelly_emulator {

class SwitchStateTrigger : public Trigger<uint8_t, bool> {};

class ShellyEmulator : public Component {
 public:
#ifdef USE_SHELLY_EMULATOR_BLE
  void set_ble_server(esp32_ble_server::BLEServer *server) { this->ble_server_ = server; }
  void set_ble_enabled(bool enabled) { this->ble_enabled_ = enabled; }
#endif
  void set_channel_count(uint8_t count) { this->channel_count_ = count; }
  void set_em_gen3(bool enabled) { this->em_gen3_ = enabled; }
  void add_switch_state_trigger(SwitchStateTrigger *trigger) {
    this->switch_state_triggers_.push_back(trigger);
  }
  void set_channel_output_switch(uint8_t channel, switch_::Switch *output) {
    if (channel < this->channels_.size()) this->channels_[channel].output = output;
  }
  void set_channel_power_sensor(uint8_t channel, sensor::Sensor *sensor) {
    if (channel < this->channels_.size()) this->channels_[channel].power = sensor;
  }
  void set_channel_voltage_sensor(uint8_t channel, sensor::Sensor *sensor) {
    if (channel < this->channels_.size()) this->channels_[channel].voltage = sensor;
  }
  void set_channel_frequency_sensor(uint8_t channel, sensor::Sensor *sensor) {
    if (channel < this->channels_.size()) this->channels_[channel].frequency = sensor;
  }
  void set_channel_current_sensor(uint8_t channel, sensor::Sensor *sensor) {
    if (channel < this->channels_.size()) this->channels_[channel].current = sensor;
  }
  void set_channel_energy_sensor(uint8_t channel, sensor::Sensor *sensor) {
    if (channel < this->channels_.size()) this->channels_[channel].energy = sensor;
  }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_wifi_ssid(const std::string &ssid) { this->wifi_ssid_ = ssid; }

  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  void start_http_();
#ifdef USE_SHELLY_EMULATOR_BLE
  void setup_ble_();
  void handle_ble_write_(std::span<const uint8_t> data);
#endif
  esp_err_t handle_http_request_(httpd_req_t *request);
  static esp_err_t http_request_handler_(httpd_req_t *request);
  std::string dispatch_rpc_(const std::string &method, int component_id, bool ble,
                            const std::string &request_id, const std::string &rpc_request);
  std::string device_info_();
  std::string switch_status_(int channel_id);
  std::string switch_config_(int channel_id);
  std::string em1_status_(int channel_id) const;
  std::string em1_data_status_(int channel_id) const;
  std::string em1_config_(int channel_id) const;
  void update_wifi_scan_();
  std::string wifi_scan_();
  std::string wifi_status_();
  std::string wifi_config_();
  std::string sys_status_();
  std::string shelly_status_();
  std::string device_id_();
  const char *model_() const;
  const char *app_name_() const;
  const char *device_id_prefix_() const;
  std::string mac_address_();
  std::string local_ip_address_();
  std::string network_status_();
  float sensor_value_(sensor::Sensor *sensor) const;
  std::string rpc_result_(const std::string &request_id, const std::string &result) const;

#ifdef USE_SHELLY_EMULATOR_BLE
  esp32_ble_server::BLEServer *ble_server_{nullptr};
  esp32_ble_server::BLEService *ble_service_{nullptr};
  esp32_ble_server::BLECharacteristic *data_char_{nullptr};
  esp32_ble_server::BLECharacteristic *tx_control_char_{nullptr};
  esp32_ble_server::BLECharacteristic *rx_control_char_{nullptr};
  bool ble_enabled_{false};
#endif
  struct ChannelSensors {
    sensor::Sensor *power{nullptr};
    sensor::Sensor *voltage{nullptr};
    sensor::Sensor *frequency{nullptr};
    sensor::Sensor *current{nullptr};
    sensor::Sensor *energy{nullptr};
    switch_::Switch *output{nullptr};
  };
  std::array<ChannelSensors, 4> channels_{};
  std::array<std::atomic<int8_t>, 4> pending_switch_states_{};
  std::vector<SwitchStateTrigger *> switch_state_triggers_;
  uint8_t channel_count_{1};
  bool em_gen3_{false};
  std::string device_name_{"ESPHome Metering Device"};
  std::string wifi_ssid_{"Ethernet"};
  bool wifi_scan_driver_ready_{false};
  bool wifi_scan_in_progress_{false};
  int64_t wifi_scan_next_ms_{0};
  std::atomic<bool> wifi_scan_requested_{false};
  SemaphoreHandle_t wifi_scan_mutex_{nullptr};
  std::string wifi_scan_results_{"{\"results\":[]}"};
  httpd_handle_t http_server_{nullptr};
#ifdef USE_SHELLY_EMULATOR_BLE
  std::vector<uint8_t> ble_request_;
  std::vector<uint8_t> ble_response_;
  uint32_t ble_expected_request_length_{0};
#endif
};

}  // namespace shelly_emulator
}  // namespace esphome
