#pragma once

#include <array>
#include <atomic>
#include <cstdint>
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
 protected:
  struct ChannelSensors {
    sensor::Sensor *power{nullptr};
    sensor::Sensor *voltage{nullptr};
    sensor::Sensor *frequency{nullptr};
    sensor::Sensor *current{nullptr};
    sensor::Sensor *energy{nullptr};
    switch_::Switch *output{nullptr};
  };
  struct VirtualDevice {
    std::array<ChannelSensors, 4> channels{};
    uint8_t channel_count{1};
    bool em_gen3{false};
    std::string name{"ESPHome Metering Device"};
    std::string hostname;
    std::string effective_hostname;
    std::string wifi_ssid{"Ethernet"};
    bool hostname_registered{false};
  };

 public:
#ifdef USE_SHELLY_EMULATOR_BLE
  void set_ble_server(esp32_ble_server::BLEServer *server) { this->ble_server_ = server; }
  void set_ble_enabled(bool enabled) { this->ble_enabled_ = enabled; }
#endif
  void set_device_count(uint8_t count) { this->devices_.resize(count); }
  void set_device_name(uint8_t device, const std::string &name) {
    if (device < this->devices_.size()) this->devices_[device].name = name;
  }
  void set_device_hostname(uint8_t device, const std::string &hostname) {
    if (device < this->devices_.size()) this->devices_[device].hostname = hostname;
  }
  void set_device_wifi_ssid(uint8_t device, const std::string &ssid) {
    if (device < this->devices_.size()) this->devices_[device].wifi_ssid = ssid;
  }
  void set_device_channel_count(uint8_t device, uint8_t count) {
    if (device < this->devices_.size()) this->devices_[device].channel_count = count;
  }
  void set_device_em_gen3(uint8_t device, bool enabled) {
    if (device < this->devices_.size()) this->devices_[device].em_gen3 = enabled;
  }
  void add_switch_state_trigger(SwitchStateTrigger *trigger) {
    this->switch_state_triggers_.push_back(trigger);
  }
  void set_channel_output_switch(uint8_t device, uint8_t channel, switch_::Switch *output) {
    if (device < this->devices_.size() && channel < this->devices_[device].channels.size())
      this->devices_[device].channels[channel].output = output;
  }
  void set_channel_power_sensor(uint8_t device, uint8_t channel, sensor::Sensor *sensor) {
    if (device < this->devices_.size() && channel < this->devices_[device].channels.size())
      this->devices_[device].channels[channel].power = sensor;
  }
  void set_channel_voltage_sensor(uint8_t device, uint8_t channel, sensor::Sensor *sensor) {
    if (device < this->devices_.size() && channel < this->devices_[device].channels.size())
      this->devices_[device].channels[channel].voltage = sensor;
  }
  void set_channel_frequency_sensor(uint8_t device, uint8_t channel, sensor::Sensor *sensor) {
    if (device < this->devices_.size() && channel < this->devices_[device].channels.size())
      this->devices_[device].channels[channel].frequency = sensor;
  }
  void set_channel_current_sensor(uint8_t device, uint8_t channel, sensor::Sensor *sensor) {
    if (device < this->devices_.size() && channel < this->devices_[device].channels.size())
      this->devices_[device].channels[channel].current = sensor;
  }
  void set_channel_energy_sensor(uint8_t device, uint8_t channel, sensor::Sensor *sensor) {
    if (device < this->devices_.size() && channel < this->devices_[device].channels.size())
      this->devices_[device].channels[channel].energy = sensor;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  void start_http_();
#ifdef USE_SHELLY_EMULATOR_BLE
  void setup_ble_();
  void handle_ble_write_(std::span<const uint8_t> data);
  void set_ble_device_name_(uint8_t device);
#endif
  esp_err_t handle_http_request_(httpd_req_t *request);
  static esp_err_t http_request_handler_(httpd_req_t *request);
  std::string dispatch_rpc_(uint8_t device, const std::string &method, int component_id, bool ble,
                            const std::string &request_id, const std::string &rpc_request);
  int device_for_host_(const std::string &host) const;
  std::string device_info_(uint8_t device);
  std::string switch_status_(uint8_t device, int channel_id);
  std::string switch_config_(int channel_id);
  std::string em1_status_(uint8_t device, int channel_id) const;
  std::string em1_data_status_(uint8_t device, int channel_id) const;
  std::string em1_config_(int channel_id) const;
  void update_wifi_scan_();
  std::string wifi_scan_();
  std::string wifi_status_(uint8_t device);
  std::string wifi_config_(uint8_t device);
  std::string sys_status_(uint8_t device);
  std::string shelly_status_(uint8_t device);
  std::string device_id_(uint8_t device);
  const char *model_(uint8_t device) const;
  const char *app_name_(uint8_t device) const;
  const char *device_id_prefix_(uint8_t device) const;
  std::string mac_address_(uint8_t device);
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
  std::vector<VirtualDevice> devices_{1};
  std::array<std::atomic<int8_t>, 16> pending_switch_states_{};
  std::vector<SwitchStateTrigger *> switch_state_triggers_;
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
  std::atomic<uint8_t> ble_connected_clients_{0};
  std::atomic<uint8_t> ble_advertised_device_{0};
  std::atomic<uint8_t> ble_active_device_{0};
  std::atomic<bool> ble_rotation_pending_{false};
  int64_t ble_rotation_due_us_{0};
  int64_t ble_restart_due_us_{0};
  bool ble_advertising_paused_{false};
#endif
};

}  // namespace shelly_emulator
}  // namespace esphome
