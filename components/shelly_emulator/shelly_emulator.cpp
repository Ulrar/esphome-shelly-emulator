#include "shelly_emulator.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <map>
#include <vector>

#include "esp_wifi.h"
#include "mdns.h"

#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esphome/core/log.h"
#ifdef USE_SHELLY_EMULATOR_BLE
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include <esp_gap_ble_api.h>
#endif

namespace esphome {
namespace shelly_emulator {

static const char *const TAG = "shelly_emulator";
#ifdef USE_SHELLY_EMULATOR_BLE
static constexpr char RPC_SERVICE_UUID[] = "5f6d4f53-5f52-5043-5f53-56435f49445f";
static constexpr char DATA_CHAR_UUID[] = "5f6d4f53-5f52-5043-5f64-6174615f5f5f";
static constexpr char TX_CONTROL_CHAR_UUID[] = "5f6d4f53-5f52-5043-5f74-785f63746c5f";
static constexpr char RX_CONTROL_CHAR_UUID[] = "5f6d4f53-5f52-5043-5f72-785f63746c5f";
#endif
static constexpr uint32_t MAX_RPC_FRAME = 4096;
static constexpr int64_t BLE_ROTATION_INTERVAL_US = 5000000;
static constexpr int64_t BLE_AD_RESTART_DELAY_US = 100000;

static esp_err_t log_http_peer(httpd_handle_t, int sockfd) {
  sockaddr_in peer{};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(sockfd, reinterpret_cast<sockaddr *>(&peer), &peer_len) == 0) {
    char address[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
    ESP_LOGI(TAG, "HTTP TCP client connected: %s:%u", address,
             static_cast<unsigned>(ntohs(peer.sin_port)));
  } else {
    ESP_LOGW(TAG, "HTTP TCP client connected; getpeername failed");
  }
  return ESP_OK;
}

static esp_netif_t *active_netif() {
  esp_netif_t *ethernet = esp_netif_get_handle_from_ifkey("ETH_DEF");
  esp_netif_ip_info_t ethernet_info{};
  if (ethernet != nullptr && esp_netif_get_ip_info(ethernet, &ethernet_info) == ESP_OK &&
      ethernet_info.ip.addr != 0)
    return ethernet;
  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif == nullptr) netif = ethernet;
  if (netif == nullptr) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  return netif;
}

static bool get_netif_ip(esp_netif_t *netif, esp_netif_ip_info_t *info) {
  return netif != nullptr && esp_netif_get_ip_info(netif, info) == ESP_OK && info->ip.addr != 0;
}

static std::string ip_to_string(const esp_ip4_addr_t &ip) {
  const uint32_t address = ip.addr;
  char text[16];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", static_cast<unsigned>(address & 0xFF),
           static_cast<unsigned>((address >> 8) & 0xFF),
           static_cast<unsigned>((address >> 16) & 0xFF),
           static_cast<unsigned>((address >> 24) & 0xFF));
  return text;
}

static std::string escape_json(const std::string &value) {
  std::string out;
  for (char c : value) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out.push_back(c);
  }
  return out;
}

static std::string json_string_field(const std::string &json, const std::string &key) {
  const std::string token = "\"" + key + "\"";
  size_t pos = json.find(token);
  if (pos == std::string::npos || (pos = json.find(':', pos + token.size())) == std::string::npos)
    return "";
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return "";
  size_t end = json.find('"', pos + 1);
  return end == std::string::npos ? "" : json.substr(pos + 1, end - pos - 1);
}

static std::string json_raw_field(const std::string &json, const std::string &key,
                                  const std::string &fallback) {
  const std::string token = "\"" + key + "\"";
  size_t pos = json.find(token);
  if (pos == std::string::npos || (pos = json.find(':', pos + token.size())) == std::string::npos)
    return fallback;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  size_t end = pos;
  if (pos < json.size() && json[pos] == '"') {
    end = json.find('"', pos + 1);
    return end == std::string::npos ? fallback : json.substr(pos, end - pos + 1);
  }
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') ++end;
  return json.substr(pos, end - pos);
}

static std::string number(float value) {
  char buf[32];
  if (!std::isfinite(value)) value = 0.0f;
  snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(value));
  return buf;
}

void ShellyEmulator::setup() {
  if (this->devices_.empty() || this->devices_.size() > 4) {
    ESP_LOGE(TAG, "The emulator requires between one and four virtual Shelly devices");
    this->mark_failed();
    return;
  }
  for (auto &state : this->pending_switch_states_) state.store(-1);
  this->wifi_scan_mutex_ = xSemaphoreCreateMutex();
  if (this->wifi_scan_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Could not create Wi-Fi scan result mutex");
    this->mark_failed();
    return;
  }
  for (size_t device_index = 0; device_index < this->devices_.size(); device_index++) {
    auto &device = this->devices_[device_index];
    if (device.channel_count != 1 && device.channel_count != 2 && device.channel_count != 4) {
      ESP_LOGE(TAG, "Device %u has an unsupported channel count", static_cast<unsigned>(device_index));
      this->mark_failed();
      return;
    }
    if (this->devices_.size() > 1 && device.hostname.empty()) {
      ESP_LOGE(TAG, "Device %u requires a BLE hostname when multiple virtual devices are configured",
               static_cast<unsigned>(device_index));
      this->mark_failed();
      return;
    }
    for (uint8_t channel = 0; channel < device.channel_count; channel++) {
      if (device.channels[channel].power == nullptr) {
        ESP_LOGE(TAG, "Device %u channel %u has no power_sensor",
                 static_cast<unsigned>(device_index), channel);
        this->mark_failed();
        return;
      }
    }
  }
#ifdef USE_SHELLY_EMULATOR_BLE
  if (this->ble_enabled_) this->setup_ble_();
  if (this->is_failed()) return;
#endif
  ESP_LOGI(TAG, "Waiting for a network address before starting HTTP RPC");
}

void ShellyEmulator::setup_ble_() {
  if (this->ble_server_ == nullptr) {
    ESP_LOGE(TAG, "ble_server_id must reference esp32_ble_server");
    this->mark_failed();
    return;
  }
  if (this->devices_.size() > 1) {
    auto *ble = esp32_ble::global_ble;
    if (ble == nullptr) {
      ESP_LOGE(TAG, "ESP32 BLE component is not initialized");
      this->mark_failed();
      return;
    }
    for (size_t i = 0; i < this->devices_.size(); i++) {
      auto &device = this->devices_[i];
      ble->set_name(device.hostname.c_str());
      char name[esp32_ble::ESP32BLE::DEVICE_NAME_BUFFER_SIZE];
      device.effective_hostname = ble->device_name(name);
      auto normalized_name = [](std::string value) {
        for (char &character : value)
          character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
        return value;
      };
      for (size_t previous = 0; previous < i; previous++) {
        if (normalized_name(this->devices_[previous].effective_hostname) ==
            normalized_name(device.effective_hostname)) {
          ESP_LOGE(TAG, "Virtual Shelly BLE names must be unique after ESPHome applies its name settings");
          this->mark_failed();
          return;
        }
      }
    }
    this->set_ble_device_name_(0);
    this->ble_advertised_device_.store(0);
  }
  this->ble_server_->set_advertising_required(true);
  using esp32_ble::ESPBTUUID;
  using esp32_ble_server::BLECharacteristic;
  this->ble_service_ = this->ble_server_->create_service(ESPBTUUID::from_raw(RPC_SERVICE_UUID), true, 12);
  if (this->ble_service_ == nullptr) {
    ESP_LOGE(TAG, "Could not create Shelly BLE RPC service");
    this->mark_failed();
    return;
  }
  if (!this->ble_server_->is_running())
    this->ble_server_->enqueue_start_service(this->ble_service_);
  if (this->devices_.size() > 1) {
    this->ble_server_->on_connect([this](uint16_t) {
      if (this->ble_connected_clients_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        this->ble_active_device_.store(this->ble_advertised_device_.load(std::memory_order_acquire),
                                       std::memory_order_release);
      }
    });
    this->ble_server_->on_disconnect([this](uint16_t) {
      const uint8_t previous = this->ble_connected_clients_.fetch_sub(1, std::memory_order_acq_rel);
      if (previous == 1) this->ble_rotation_pending_.store(true, std::memory_order_release);
    });
  }
  this->data_char_ = this->ble_service_->create_characteristic(
      ESPBTUUID::from_raw(DATA_CHAR_UUID),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  this->tx_control_char_ = this->ble_service_->create_characteristic(
      ESPBTUUID::from_raw(TX_CONTROL_CHAR_UUID),
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  this->rx_control_char_ = this->ble_service_->create_characteristic(
      ESPBTUUID::from_raw(RX_CONTROL_CHAR_UUID),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  if (this->data_char_ == nullptr || this->tx_control_char_ == nullptr ||
      this->rx_control_char_ == nullptr) {
    ESP_LOGE(TAG, "Could not create Shelly BLE RPC characteristics");
    this->mark_failed();
    return;
  }
  this->tx_control_char_->on_write([this](std::span<const uint8_t> data, uint16_t) {
    if (data.size() < 4) return;
    this->ble_expected_request_length_ = (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
    this->ble_request_.clear();
    this->ble_response_.clear();
    if (this->ble_expected_request_length_ == 0 || this->ble_expected_request_length_ > MAX_RPC_FRAME) {
      this->ble_expected_request_length_ = 0;
      ESP_LOGW(TAG, "Rejected invalid BLE RPC frame length");
    }
    this->rx_control_char_->set_value(std::vector<uint8_t>{0, 0, 0, 0});
  });
  this->data_char_->on_write([this](std::span<const uint8_t> data, uint16_t) {
    this->handle_ble_write_(data);
  });
}

void ShellyEmulator::handle_ble_write_(std::span<const uint8_t> data) {
  if (this->ble_expected_request_length_ == 0) return;
  const size_t remaining = this->ble_expected_request_length_ - this->ble_request_.size();
  const size_t count = std::min(remaining, data.size());
  this->ble_request_.insert(this->ble_request_.end(), data.begin(), data.begin() + count);
  if (this->ble_request_.size() < this->ble_expected_request_length_) return;

  const std::string request(this->ble_request_.begin(), this->ble_request_.end());
  const std::string method = json_string_field(request, "method");
  ESP_LOGI(TAG, "BLE RPC request: %s", method.empty() ? "<invalid>" : method.c_str());
  const std::string request_id = json_raw_field(request, "id", "1");
  int component_id = 0;
  const size_t params = request.find("\"params\"");
  if (params != std::string::npos)
    component_id = atoi(json_raw_field(request.substr(params), "id", "0").c_str());

  const uint8_t device = this->ble_active_device_.load(std::memory_order_acquire);
  const std::string response =
      this->dispatch_rpc_(device, method, component_id, true, request_id, request);
  if (method == "Wifi.GetStatus" || method == "WiFi.GetStatus") {
    ESP_LOGI(TAG, "BLE RPC response (%u bytes): %s",
             static_cast<unsigned>(response.size()), response.c_str());
  }
  this->ble_response_.assign(response.begin(), response.end());
  const uint32_t n = this->ble_response_.size();
  this->data_char_->set_value(std::move(this->ble_response_));
  this->rx_control_char_->set_value(std::vector<uint8_t>{
      static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
      static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)});
  this->rx_control_char_->notify();
  this->ble_expected_request_length_ = 0;
  this->ble_request_.clear();
}
#endif

void ShellyEmulator::start_http_() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.open_fn = log_http_peer;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.lru_purge_enable = true;
  esp_err_t err = httpd_start(&this->http_server_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not start HTTP server: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  httpd_uri_t get_uri = {};
  get_uri.uri = "/*";
  get_uri.method = HTTP_GET;
  get_uri.handler = &ShellyEmulator::http_request_handler_;
  get_uri.user_ctx = this;
  err = httpd_register_uri_handler(this->http_server_, &get_uri);
  if (err == ESP_OK) {
    httpd_uri_t post_uri = {};
    post_uri.uri = "/rpc";
    post_uri.method = HTTP_POST;
    post_uri.handler = &ShellyEmulator::http_request_handler_;
    post_uri.user_ctx = this;
    err = httpd_register_uri_handler(this->http_server_, &post_uri);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not register RPC handler: %s", esp_err_to_name(err));
    httpd_stop(this->http_server_);
    this->http_server_ = nullptr;
    this->mark_failed();
  }
}

esp_err_t ShellyEmulator::http_request_handler_(httpd_req_t *request) {
  auto *self = static_cast<ShellyEmulator *>(request->user_ctx);
  return self->handle_http_request_(request);
}

esp_err_t ShellyEmulator::handle_http_request_(httpd_req_t *request) {
  std::string uri(request->uri);
  const size_t query_pos = uri.find('?');
  std::string path = uri.substr(0, query_pos);
  const char *http_method = request->method == HTTP_GET ? "GET" :
                            request->method == HTTP_POST ? "POST" : "OTHER";
  ESP_LOGI(TAG, "HTTP request: %s %s", http_method, path.c_str());
  const bool log_switch_rpc = path == "/rpc/Switch.Set" || path == "/rpc/Switch.Toggle";
  if (log_switch_rpc)
    ESP_LOGI(TAG, "HTTP RPC request arguments: %s", uri.c_str());
  std::string method;
  std::string request_id = "1";
  int component_id = 0;
  std::string body;

  if (request->method == HTTP_GET && path.rfind("/rpc/", 0) == 0) {
    method = path.substr(5);
    if (query_pos != std::string::npos) {
      char query[128];
      if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK) {
        char value[24];
        if (httpd_query_key_value(query, "id", value, sizeof(value)) == ESP_OK)
          component_id = atoi(value);
      }
    }
  } else if (request->method == HTTP_POST && path == "/rpc") {
    if (request->content_len > MAX_RPC_FRAME) {
      httpd_resp_set_status(request, "413 Payload Too Large");
      return httpd_resp_send(request, nullptr, 0);
    }
    body.resize(request->content_len);
    size_t received = 0;
    while (received < body.size()) {
      int count = httpd_req_recv(request, body.data() + received, body.size() - received);
      if (count <= 0) {
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        return ESP_FAIL;
      }
      received += static_cast<size_t>(count);
    }
    method = json_string_field(body, "method");
    request_id = json_raw_field(body, "id", "1");
    const size_t params = body.find("\"params\"");
    if (params != std::string::npos)
      component_id = atoi(json_raw_field(body.substr(params), "id", "0").c_str());
  }

  if (method.empty()) {
    httpd_resp_set_status(request, "404 Not Found");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"error\":\"unsupported request\"}");
  }

  uint8_t device_index = 0;
  if (this->devices_.size() > 1) {
    char host[128]{};
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
      ESP_LOGW(TAG, "Could not route request without an HTTP Host header");
      httpd_resp_set_status(request, "421 Misdirected Request");
      return httpd_resp_sendstr(request, "{\"error\":\"unknown Shelly hostname\"}");
    }
    const int found = this->device_for_host_(host);
    if (found < 0) {
      ESP_LOGW(TAG, "No virtual Shelly matches HTTP Host: %s", host);
      httpd_resp_set_status(request, "421 Misdirected Request");
      return httpd_resp_sendstr(request, "{\"error\":\"unknown Shelly hostname\"}");
    }
    device_index = static_cast<uint8_t>(found);
  }

  const std::string &rpc_request = request->method == HTTP_POST ? body : uri;
  const std::string response = this->dispatch_rpc_(device_index, method, component_id,
                                                    false, request_id, rpc_request);
  httpd_resp_set_type(request, "application/json");
  const esp_err_t send_result = httpd_resp_send(request, response.c_str(), response.size());
  if (log_switch_rpc) {
    ESP_LOGI(TAG, "HTTP RPC response: %s", response.c_str());
    if (send_result == ESP_OK)
      ESP_LOGI(TAG, "HTTP RPC response sent successfully");
    else
      ESP_LOGW(TAG, "HTTP RPC response send failed: %s", esp_err_to_name(send_result));
  }
  return send_result;
}

void ShellyEmulator::loop() {
  if (this->is_failed()) return;
  for (size_t flat_channel = 0; flat_channel < this->pending_switch_states_.size(); flat_channel++) {
    const int8_t requested_state =
        this->pending_switch_states_[flat_channel].exchange(-1, std::memory_order_acq_rel);
    if (requested_state < 0) continue;
    const size_t device_index = flat_channel / 4;
    const uint8_t channel_index = flat_channel % 4;
    if (device_index >= this->devices_.size()) continue;
    auto &device = this->devices_[device_index];
    if (channel_index >= device.channel_count) continue;
    if (auto *output = device.channels[channel_index].output) {
      if (requested_state != 0)
        output->turn_on();
      else
        output->turn_off();
    }
    for (auto *trigger : this->switch_state_triggers_)
      trigger->trigger(static_cast<uint8_t>(flat_channel), requested_state != 0);
  }
#ifdef USE_SHELLY_EMULATOR_BLE
  if (this->ble_enabled_ && this->devices_.size() > 1 && this->ble_server_ != nullptr &&
      this->ble_server_->is_running() && esp32_ble::global_ble != nullptr) {
    const int64_t now = esp_timer_get_time();
    const uint8_t connected = this->ble_connected_clients_.load(std::memory_order_acquire);
    if (connected == 0 && this->ble_rotation_pending_.exchange(false, std::memory_order_acq_rel)) {
      if (!this->ble_advertising_paused_) {
        esp32_ble::global_ble->advertising_stop();
        this->ble_advertising_paused_ = true;
      }
      const uint8_t current = this->ble_advertised_device_.load(std::memory_order_acquire);
      const uint8_t next = static_cast<uint8_t>((current + 1) % this->devices_.size());
      this->set_ble_device_name_(next);
      this->ble_advertised_device_.store(next, std::memory_order_release);
      this->ble_restart_due_us_ = now + BLE_AD_RESTART_DELAY_US;
      this->ble_rotation_due_us_ = now + BLE_ROTATION_INTERVAL_US;
    }
    if (this->ble_rotation_due_us_ == 0)
      this->ble_rotation_due_us_ = now + BLE_ROTATION_INTERVAL_US;
    if (connected == 0 && this->ble_advertising_paused_ && now >= this->ble_restart_due_us_) {
      if (this->ble_connected_clients_.load(std::memory_order_acquire) == 0) {
        esp32_ble::global_ble->advertising_start();
        this->ble_advertising_paused_ = false;
      }
      this->ble_restart_due_us_ = 0;
    } else if (connected == 0 && !this->ble_advertising_paused_ && now >= this->ble_rotation_due_us_) {
      esp32_ble::global_ble->advertising_stop();
      this->ble_advertising_paused_ = true;
      const uint8_t current = this->ble_advertised_device_.load(std::memory_order_acquire);
      const uint8_t next = static_cast<uint8_t>((current + 1) % this->devices_.size());
      this->set_ble_device_name_(next);
      this->ble_advertised_device_.store(next, std::memory_order_release);
      this->ble_restart_due_us_ = now + BLE_AD_RESTART_DELAY_US;
      this->ble_rotation_due_us_ = now + BLE_ROTATION_INTERVAL_US;
    }
  }
#endif
  if (this->http_server_ == nullptr) {
    esp_netif_ip_info_t info{};
    if (!get_netif_ip(active_netif(), &info)) return;

    for (auto &device : this->devices_) {
      if (device.effective_hostname.empty() || device.hostname_registered) continue;
      const esp_err_t err = mdns_hostname_add(device.effective_hostname.c_str());
      if (err == ESP_OK) {
        device.hostname_registered = true;
        ESP_LOGI(TAG, "Registered virtual Shelly hostname: %s.local",
                 device.effective_hostname.c_str());
      } else {
        ESP_LOGW(TAG, "Could not register virtual Shelly hostname %s: %s",
                 device.effective_hostname.c_str(), esp_err_to_name(err));
      }
    }

    this->start_http_();
    if (!this->is_failed())
      ESP_LOGI(TAG, "Serving %u virtual Shelly device(s) on HTTP port 80",
               static_cast<unsigned>(this->devices_.size()));
  }
  this->update_wifi_scan_();
}

#ifdef USE_SHELLY_EMULATOR_BLE
void ShellyEmulator::set_ble_device_name_(uint8_t device) {
  if (device >= this->devices_.size() || esp32_ble::global_ble == nullptr) return;
  esp32_ble::global_ble->set_name(this->devices_[device].hostname.c_str());
  if (this->ble_server_ != nullptr && this->ble_server_->is_running()) {
    char name[esp32_ble::ESP32BLE::DEVICE_NAME_BUFFER_SIZE];
    const esp_err_t err = esp_ble_gap_set_device_name(esp32_ble::global_ble->device_name(name));
    if (err != ESP_OK) ESP_LOGW(TAG, "Could not update BLE device name: %s", esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "BLE identity set to %s (%s)", this->devices_[device].name.c_str(),
           this->devices_[device].effective_hostname.c_str());
}
#endif

void ShellyEmulator::update_wifi_scan_() {
  if (!this->wifi_scan_driver_ready_) {
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    const bool initialized_here = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "Could not initialize Wi-Fi scan driver: %s", esp_err_to_name(err));
      return;
    }

    if (initialized_here) {
      err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
      if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
      if (err == ESP_OK) err = esp_wifi_start();
      if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "Could not start Wi-Fi scan driver: %s", esp_err_to_name(err));
        return;
      }
    } else {
      wifi_mode_t mode = WIFI_MODE_NULL;
      err = esp_wifi_get_mode(&mode);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi driver is already initialized but unavailable for scanning");
        return;
      }
      if (mode == WIFI_MODE_NULL) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
          ESP_LOGW(TAG, "Could not start Wi-Fi scan driver: %s", esp_err_to_name(err));
          return;
        }
      } else if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        ESP_LOGW(TAG, "Wi-Fi is active in AP mode; leaving its mode unchanged");
        this->wifi_scan_next_ms_ = (esp_timer_get_time() / 1000) + 60000;
        return;
      }
    }

    this->wifi_scan_driver_ready_ = true;
    ESP_LOGI(TAG, "Wi-Fi scan driver ready (station mode, no Wi-Fi connection)");
  }

  const int64_t now_ms = esp_timer_get_time() / 1000;
  if (this->wifi_scan_in_progress_) {
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) return;

    this->wifi_scan_in_progress_ = false;
    this->wifi_scan_requested_.store(false);
    this->wifi_scan_next_ms_ = now_ms + 60000;
    const uint16_t read_count = std::min<uint16_t>(ap_count, 100);
    std::vector<wifi_ap_record_t> records(read_count);
    uint16_t records_read = read_count;
    if (records_read > 0 &&
        esp_wifi_scan_get_ap_records(&records_read, records.data()) != ESP_OK) {
      ESP_LOGW(TAG, "Could not read Wi-Fi scan results");
      return;
    }
    records.resize(records_read);

    // Shelly returns visible APs only, one strongest result per SSID, and at
    // most 20 entries. Ignore very weak beacons just like the device firmware.
    std::map<std::string, wifi_ap_record_t> best_by_ssid;
    for (const auto &record : records) {
      if (record.rssi <= -85) continue;
      const size_t ssid_len = strnlen(reinterpret_cast<const char *>(record.ssid), sizeof(record.ssid));
      if (ssid_len == 0) continue;
      std::string ssid(reinterpret_cast<const char *>(record.ssid), ssid_len);
      auto found = best_by_ssid.find(ssid);
      if (found == best_by_ssid.end() || record.rssi > found->second.rssi)
        best_by_ssid[ssid] = record;
    }

    std::vector<std::pair<std::string, wifi_ap_record_t>> results(best_by_ssid.begin(), best_by_ssid.end());
    std::sort(results.begin(), results.end(), [](const auto &left, const auto &right) {
      return left.second.rssi > right.second.rssi;
    });
    if (results.size() > 20) results.resize(20);

    std::string json = "{\"results\":[";
    bool first = true;
    for (const auto &entry : results) {
      const auto &record = entry.second;
      if (!first) json += ",";
      first = false;
      char bssid[18];
      snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
               record.bssid[0], record.bssid[1], record.bssid[2],
               record.bssid[3], record.bssid[4], record.bssid[5]);
      int auth = static_cast<int>(record.authmode);
      if (auth > 6) auth = 6;
      json += "{\"ssid\":\"" + escape_json(entry.first) + "\",\"bssid\":\"" + bssid +
              "\",\"auth\":" + std::to_string(auth) +
              ",\"channel\":" + std::to_string(record.primary) +
              ",\"rssi\":" + std::to_string(record.rssi) + "}";
    }
    json += "]}";

    if (this->wifi_scan_mutex_ != nullptr) xSemaphoreTake(this->wifi_scan_mutex_, portMAX_DELAY);
    this->wifi_scan_results_ = std::move(json);
    if (this->wifi_scan_mutex_ != nullptr) xSemaphoreGive(this->wifi_scan_mutex_);
    ESP_LOGI(TAG, "Wi-Fi scan complete: %u visible networks",
             static_cast<unsigned>(results.size()));
    return;
  }

  if (!this->wifi_scan_requested_.exchange(false) && now_ms < this->wifi_scan_next_ms_) return;
  wifi_scan_config_t scan_config{};
  scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_config.scan_time.active.min = 100;
  scan_config.scan_time.active.max = 300;
  esp_err_t err = esp_wifi_scan_start(&scan_config, false);
  if (err == ESP_OK) {
    this->wifi_scan_in_progress_ = true;
    this->wifi_scan_next_ms_ = now_ms + 60000;
  } else {
    this->wifi_scan_next_ms_ = now_ms + 5000;
    ESP_LOGW(TAG, "Could not start Wi-Fi scan: %s", esp_err_to_name(err));
  }
}

std::string ShellyEmulator::wifi_scan_() {
  this->wifi_scan_requested_.store(true);
  if (this->wifi_scan_mutex_ == nullptr) return "{\"results\":[]}";
  xSemaphoreTake(this->wifi_scan_mutex_, portMAX_DELAY);
  std::string result = this->wifi_scan_results_;
  xSemaphoreGive(this->wifi_scan_mutex_);
  return result;
}

int ShellyEmulator::device_for_host_(const std::string &host) const {
  auto normalize = [](std::string value) {
    const size_t port = value.find(':');
    if (port != std::string::npos) value.resize(port);
    while (!value.empty() && value.back() == '.') value.pop_back();
    for (char &character : value)
      character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
    constexpr char suffix[] = ".local";
    if (value.size() >= sizeof(suffix) - 1 &&
        value.compare(value.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) == 0)
      value.resize(value.size() - (sizeof(suffix) - 1));
    return value;
  };
  const std::string requested = normalize(host);
  for (size_t i = 0; i < this->devices_.size(); i++)
    if (normalize(this->devices_[i].effective_hostname) == requested) return static_cast<int>(i);
  return -1;
}

std::string ShellyEmulator::dispatch_rpc_(uint8_t device_index, const std::string &method,
                                          int component_id, bool ble, const std::string &request_id,
                                          const std::string &rpc_request) {
  if (device_index >= this->devices_.size()) {
    if (ble) return "{\"id\":" + request_id +
        ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
    return "{\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
  }
  auto &device = this->devices_[device_index];
  std::string result;
  const std::string rpc_method = method.rfind("Wifi.", 0) == 0
      ? "WiFi." + method.substr(5)
      : method;
  if (rpc_method == "Shelly.GetDeviceInfo") result = this->device_info_(device_index);
  else if (rpc_method == "Shelly.GetStatus") result = this->shelly_status_(device_index);
  else if (rpc_method == "Shelly.GetConfig") {
    result = "{\"device\":{\"name\":\"" + escape_json(device.name) +
             "\",\"id\":\"" + this->device_id_(device_index) + "\"},\"wifi\":" +
             this->wifi_config_(device_index);
    for (uint8_t i = 0; i < device.channel_count; i++) {
      if (device.em_gen3) {
        result += ",\"em1:" + std::to_string(i) + "\":" + this->em1_config_(i);
        result += ",\"em1data:" + std::to_string(i) + "\":{}";
      } else {
        result += ",\"switch:" + std::to_string(i) + "\":" + this->switch_config_(i);
      }
    }
    result += "}";
  } else if (rpc_method == "Shelly.ListMethods") {
    result = "{\"methods\":[\"Shelly.GetDeviceInfo\",\"Shelly.GetStatus\","
             "\"Shelly.GetConfig\",\"Shelly.ListMethods\",\"Sys.GetStatus\","
             "\"WiFi.GetStatus\",\"WiFi.GetConfig\",\"WiFi.Scan\",\"WiFi.SetConfig\"";
    if (device.em_gen3) {
      result += ",\"EM1.GetStatus\",\"EM1.GetConfig\",\"EM1Data.GetStatus\",\"EM1Data.GetConfig\"";
    } else {
      result += ",\"Switch.GetStatus\",\"Switch.GetConfig\",\"Switch.Set\",\"Switch.Toggle\"";
    }
    result += "]}";
  } else if (rpc_method == "Sys.GetStatus") result = this->sys_status_(device_index);
  else if (rpc_method == "WiFi.GetStatus") result = this->wifi_status_(device_index);
  else if (rpc_method == "WiFi.Scan") result = this->wifi_scan_();
  else if (rpc_method == "WiFi.GetConfig") result = this->wifi_config_(device_index);
  else if (rpc_method == "WiFi.SetConfig") {
    const std::string ssid = json_string_field(rpc_request, "ssid");
    if (!ssid.empty()) device.wifi_ssid = ssid;
    ESP_LOGI(TAG, "WiFi.SetConfig received for %s (SSID %s)",
             device.name.c_str(), ssid.empty() ? "missing" : "provided");
    result = "{\"restart_required\":false}";
  } else if (device.em_gen3 && rpc_method == "EM1.GetStatus" &&
             component_id >= 0 && component_id < device.channel_count)
    result = this->em1_status_(device_index, component_id);
  else if (device.em_gen3 && rpc_method == "EM1Data.GetStatus" &&
           component_id >= 0 && component_id < device.channel_count)
    result = this->em1_data_status_(device_index, component_id);
  else if (device.em_gen3 && rpc_method == "EM1.GetConfig" &&
           component_id >= 0 && component_id < device.channel_count)
    result = this->em1_config_(component_id);
  else if (device.em_gen3 && rpc_method == "EM1Data.GetConfig" &&
           component_id >= 0 && component_id < device.channel_count)
    result = "{\"id\":" + std::to_string(component_id) + "}";
  else if (!device.em_gen3 && rpc_method == "Switch.GetStatus" &&
           component_id >= 0 && component_id < device.channel_count)
    result = this->switch_status_(device_index, component_id);
  else if (!device.em_gen3 && rpc_method == "Switch.GetConfig" &&
           component_id >= 0 && component_id < device.channel_count)
    result = this->switch_config_(component_id);
  else if (!device.em_gen3 &&
           (rpc_method == "Switch.Set" || rpc_method == "Switch.Toggle") &&
           component_id >= 0 && component_id < device.channel_count) {
    const bool was_on = device.channels[component_id].output != nullptr &&
                        device.channels[component_id].output->state;
    bool requested_state = was_on;
    if (rpc_method == "Switch.Toggle") {
      requested_state = !was_on;
    } else {
      std::string on_value;
      const size_t query = rpc_request.find('?');
      if (query != std::string::npos) {
        const size_t on_param = rpc_request.find("on=", query + 1);
        if (on_param != std::string::npos) {
          const size_t value_start = on_param + 3;
          const size_t value_end = rpc_request.find('&', value_start);
          on_value = rpc_request.substr(value_start, value_end == std::string::npos
                                                        ? std::string::npos
                                                        : value_end - value_start);
        }
      } else {
        const size_t params = rpc_request.find("\"params\"");
        const std::string request_params = params == std::string::npos
            ? rpc_request
            : rpc_request.substr(params);
        on_value = json_raw_field(request_params, "on", "");
      }
      if (on_value == "true" || on_value == "1") {
        requested_state = true;
      } else if (on_value == "false" || on_value == "0") {
        requested_state = false;
      } else {
        const std::string error = "{\"code\":-32602,\"message\":\"Invalid argument\"}";
        return ble ? "{\"id\":" + request_id + ",\"error\":" + error + "}"
                   : "{\"error\":" + error + "}";
      }
    }
    if (requested_state != was_on) {
      const size_t flat_channel = static_cast<size_t>(device_index) * 4 + component_id;
      this->pending_switch_states_[flat_channel].store(requested_state ? 1 : 0,
                                                       std::memory_order_release);
    }
    result = std::string("{\"was_on\":") + (was_on ? "true}" : "false}");
  } else {
    if (ble) return "{\"id\":" + request_id +
        ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
    return "{\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
  }
  return ble ? this->rpc_result_(request_id, result) : result;
}

std::string ShellyEmulator::rpc_result_(const std::string &id, const std::string &result) const {
  return "{\"id\":" + id + ",\"result\":" + result + "}";
}

std::string ShellyEmulator::mac_address_(uint8_t device_index) {
  uint8_t mac[6];
  esp_netif_t *netif = active_netif();
  if (netif == nullptr || esp_netif_get_mac(netif, mac) != ESP_OK)
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
  mac[5] = static_cast<uint8_t>(mac[5] + device_index);
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  return text;
}

std::string ShellyEmulator::local_ip_address_() {
  esp_netif_ip_info_t info{};
  return get_netif_ip(active_netif(), &info) ? ip_to_string(info.ip) : "0.0.0.0";
}

std::string ShellyEmulator::network_status_() {
  esp_netif_t *netif = active_netif();
  esp_netif_ip_info_t info{};
  if (netif == nullptr || !get_netif_ip(netif, &info)) return "";
  const char *key = esp_netif_get_ifkey(netif);
  if (key == nullptr || strncmp(key, "ETH", 3) != 0) return "";
  return ",\"eth\":{\"ip\":\"" + ip_to_string(info.ip) + "\"}";
}

std::string ShellyEmulator::device_id_(uint8_t device) {
  std::string id = this->device_id_prefix_(device);
  for (char c : this->mac_address_(device))
    if (c != ':') id.push_back(static_cast<char>(tolower(c)));
  return id;
}

const char *ShellyEmulator::model_(uint8_t device) const {
  const auto &virtual_device = this->devices_[device];
  if (virtual_device.em_gen3) return "S3EM-002CXCEU";
  if (virtual_device.channel_count == 2) return "SNSW-002P16EU";
  if (virtual_device.channel_count == 4) return "SPSW-004PE16EU";
  return "SNSW-001P16EU";
}

const char *ShellyEmulator::app_name_(uint8_t device) const {
  const auto &virtual_device = this->devices_[device];
  if (virtual_device.em_gen3) return "EMG3";
  if (virtual_device.channel_count == 2) return "Plus2PM";
  if (virtual_device.channel_count == 4) return "Pro4PM";
  return "Plus1PM";
}

const char *ShellyEmulator::device_id_prefix_(uint8_t device) const {
  const auto &virtual_device = this->devices_[device];
  if (virtual_device.em_gen3) return "shellyemg3-";
  if (virtual_device.channel_count == 2) return "shellyplus2pm-";
  if (virtual_device.channel_count == 4) return "shellypro4pm-";
  return "shellyplus1pm-";
}

std::string ShellyEmulator::device_info_(uint8_t device) {
  const char *firmware_id = this->devices_[device].em_gen3
      ? "20260311-095848/1.7.5-g9979d16"
      : "20260311-095847/1.7.5-g9979d16";
  return "{\"name\":\"" + escape_json(this->devices_[device].name) + "\",\"id\":\"" +
         this->device_id_(device) + "\",\"mac\":\"" + this->mac_address_(device) +
         "\",\"model\":\"" + this->model_(device) + "\",\"gen\":" +
         (this->devices_[device].em_gen3 ? "3" : "2") + ",\"fw_id\":\"" + firmware_id +
         "\",\"ver\":\"1.7.5\",\"app\":\"" + this->app_name_(device) +
         "\",\"auth_en\":false,\"discoverable\":"
#ifdef USE_SHELLY_EMULATOR_BLE
         + std::string(this->ble_enabled_ ? "true" : "false") + "}";
#else
         "false}";
#endif
}

float ShellyEmulator::sensor_value_(sensor::Sensor *sensor) const {
  return sensor != nullptr && std::isfinite(sensor->state) ? sensor->state : 0.0f;
}

std::string ShellyEmulator::switch_status_(uint8_t device, int channel_id) {
  const auto &channel = this->devices_[device].channels[channel_id];
  const float power = this->sensor_value_(channel.power);
  const float voltage = this->sensor_value_(channel.voltage);
  const float current = this->sensor_value_(channel.current);
  const float apparent_power = voltage * current;
  const float power_factor = apparent_power > 0.0f
      ? std::clamp(power / apparent_power, -1.0f, 1.0f)
      : 0.0f;
  return "{\"id\":" + std::to_string(channel_id) +
         ",\"source\":\"HTTP\",\"output\":" +
         std::string(channel.output != nullptr && channel.output->state ? "true" : "false") +
         ",\"apower\":" + number(power) +
         ",\"voltage\":" + number(voltage) +
         ",\"freq\":" + number(this->sensor_value_(channel.frequency)) +
         ",\"current\":" + number(current) +
         ",\"pf\":" + number(power_factor) +
         ",\"aenergy\":{\"total\":" + number(this->sensor_value_(channel.energy)) +
         ",\"by_minute\":[0,0,0],\"minute_ts\":0}"
         ",\"ret_aenergy\":{\"total\":0.000,\"by_minute\":[0,0,0],\"minute_ts\":0}"
         ",\"temperature\":{\"tC\":0,\"tF\":32}}";
}

std::string ShellyEmulator::switch_config_(int channel_id) {
  return "{\"id\":" + std::to_string(channel_id) + ",\"name\":\"Metering Channel " +
         std::to_string(channel_id + 1) + "\",\"in_mode\":\"follow\",\"initial_state\":\"off\"}";
}

std::string ShellyEmulator::em1_status_(uint8_t device, int channel_id) const {
  const auto &channel = this->devices_[device].channels[channel_id];
  const float power = this->sensor_value_(channel.power);
  const float voltage = this->sensor_value_(channel.voltage);
  const float current = this->sensor_value_(channel.current);
  const float apparent_power = std::fabs(voltage * current);
  const float power_factor = apparent_power > 0.0f
      ? std::clamp(power / apparent_power, -1.0f, 1.0f)
      : 0.0f;
  return "{\"id\":" + std::to_string(channel_id) +
         ",\"voltage\":" + number(voltage) +
         ",\"current\":" + number(current) +
         ",\"act_power\":" + number(power) +
         ",\"aprt_power\":" + number(apparent_power) +
         ",\"pf\":" + number(power_factor) +
         ",\"freq\":" + number(this->sensor_value_(channel.frequency)) +
         ",\"errors\":[]}";
}

std::string ShellyEmulator::em1_data_status_(uint8_t device, int channel_id) const {
  return "{\"id\":" + std::to_string(channel_id) +
         ",\"total_act_energy\":" +
         number(this->sensor_value_(this->devices_[device].channels[channel_id].energy)) +
         ",\"total_act_ret_energy\":0.000}";
}

std::string ShellyEmulator::em1_config_(int channel_id) const {
  return "{\"id\":" + std::to_string(channel_id) +
         ",\"name\":null,\"reverse\":false,\"ct_type\":\"50A\"}";
}

std::string ShellyEmulator::wifi_status_(uint8_t device) {
  const std::string ip = this->local_ip_address_();
  const bool connected = ip != "0.0.0.0";
  const char *status = connected ? "got ip" : "disconnected";
  ESP_LOGI(TAG, "WiFi.GetStatus response for %s: status=%s, sta_ip=%s, SSID %s",
           this->devices_[device].name.c_str(), status, ip.c_str(),
           this->devices_[device].wifi_ssid.empty() ? "not set" : "set");
  uint8_t wifi_mac[6]{};
  esp_read_mac(wifi_mac, ESP_MAC_WIFI_STA);
  const unsigned bssid_octet_0 = (wifi_mac[0] & 0xFC) | 0x02;
  const unsigned bssid_octet_5 = static_cast<uint8_t>(wifi_mac[5] ^ 0x01 ^ device);
  char bssid[18];
  snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", bssid_octet_0,
           static_cast<unsigned>(wifi_mac[1]), static_cast<unsigned>(wifi_mac[2]),
           static_cast<unsigned>(wifi_mac[3]), static_cast<unsigned>(wifi_mac[4]), bssid_octet_5);

  return "{\"sta_ip\":\"" + ip + "\",\"status\":\"" + status +
         "\",\"ssid\":\"" + escape_json(this->devices_[device].wifi_ssid) +
         "\",\"bssid\":\"" + bssid + "\",\"rssi\":-55,\"sta_ip6\":[]}";
}

std::string ShellyEmulator::wifi_config_(uint8_t device) {
  std::string ap_ssid = "Shelly" + std::string(this->app_name_(device)) + "-";
  for (char c : this->mac_address_(device))
    if (c != ':') ap_ssid.push_back(c);

  return "{\"ap\":{\"ssid\":\"" + ap_ssid +
         "\",\"is_open\":true,\"enable\":false,\"range_extender\":{\"enable\":false}},"
         "\"sta\":{\"ssid\":\"" + escape_json(this->devices_[device].wifi_ssid) +
         "\",\"is_open\":false,\"enable\":true,\"ipv4mode\":\"dhcp\","
         "\"ip\":null,\"netmask\":null,\"gw\":null,\"nameserver\":null},"
         "\"sta1\":{\"ssid\":null,\"is_open\":true,\"enable\":false,"
         "\"ipv4mode\":\"dhcp\",\"ip\":null,\"netmask\":null,"
         "\"gw\":null,\"nameserver\":null},"
         "\"roam\":{\"rssi_thr\":-80,\"interval\":60}}";
}

std::string ShellyEmulator::sys_status_(uint8_t device) {
  const uint64_t uptime = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
  char uptime_text[24];
  snprintf(uptime_text, sizeof(uptime_text), "%llu", static_cast<unsigned long long>(uptime));
  return "{\"mac\":\"" + this->mac_address_(device) +
         "\",\"restart_required\":false,\"uptime\":" + uptime_text +
         ",\"ram_size\":0,\"ram_free\":0}";
}

std::string ShellyEmulator::shelly_status_(uint8_t device_index) {
  const auto &device = this->devices_[device_index];
  bool ble_enabled = false;
#ifdef USE_SHELLY_EMULATOR_BLE
  ble_enabled = this->ble_enabled_;
#endif
  std::string status = "{\"ble\":{\"enable\":" + std::string(ble_enabled ? "true" : "false") +
         "},\"cloud\":{\"connected\":false},\"mqtt\":{\"connected\":false},"
         "\"sys\":" + this->sys_status_(device_index) +
         ",\"wifi\":" + this->wifi_status_(device_index) +
         this->network_status_();
  for (uint8_t i = 0; i < device.channel_count; i++) {
    if (device.em_gen3) {
      status += ",\"em1:" + std::to_string(i) + "\":" + this->em1_status_(device_index, i);
      status += ",\"em1data:" + std::to_string(i) + "\":" + this->em1_data_status_(device_index, i);
    } else {
      status += ",\"switch:" + std::to_string(i) + "\":" + this->switch_status_(device_index, i);
    }
  }
  status += "}";
  return status;
}

void ShellyEmulator::dump_config() {
  ESP_LOGCONFIG(TAG, "Shelly emulator devices: %u", static_cast<unsigned>(this->devices_.size()));
  for (size_t i = 0; i < this->devices_.size(); i++) {
    ESP_LOGCONFIG(TAG, "  %s (%s, Gen %u, %u channels)", this->devices_[i].name.c_str(),
                  this->model_(i), this->devices_[i].em_gen3 ? 3U : 2U,
                  this->devices_[i].channel_count);
    if (!this->devices_[i].hostname.empty())
      ESP_LOGCONFIG(TAG, "    Hostname: %s.local", this->devices_[i].effective_hostname.c_str());
  }
  ESP_LOGCONFIG(TAG, "  HTTP RPC port: 80");
  ESP_LOGCONFIG(TAG, "  BLE RPC: %s",
#ifdef USE_SHELLY_EMULATOR_BLE
                this->ble_enabled_ ? "enabled" : "disabled"
#else
                "disabled"
#endif
  );
}

}  // namespace shelly_emulator
}  // namespace esphome
