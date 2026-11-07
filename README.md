# Shelly multi-channel emulator for ESPHome

This external component exposes ESPHome sensor readings as Shelly Gen 2 switch metering channels. It serves Shelly RPC over HTTP on port 80. The HTTP server accepts connections on the device's IP stack, so it works with ESPHome Wi-Fi or Ethernet configurations.

Each channel requires a power sensor. Voltage, current, and cumulative energy sensors are optional. The number of entries selects the emulated Shelly profile automatically: one channel uses a Plus 1PM, two use a Plus 2PM, and four use a Pro 4PM. These profiles provide the supported channel counts of one, two, and four:

~~~yaml
external_components:
  - source:
      type: local
      path: components
    components: [shelly_emulator]

esp32:
  board: esp32dev
  framework:
    type: esp-idf

shelly_emulator:
  device_name: ESPHome Metering Device
  wifi_ssid: Ethernet
  channels:
    - power_sensor: meter_a_power
      voltage_sensor: meter_a_voltage
      current_sensor: meter_a_current
      energy_sensor: meter_a_energy
    - power_sensor: meter_b_power
      voltage_sensor: meter_b_voltage
      current_sensor: meter_b_current
      energy_sensor: meter_b_energy
~~~

Make sure each referenced sensor has an `id`. Power is in watts, voltage in volts, current in amps, and cumulative energy in watt-hours. The component reports zero for any omitted optional sensor. Existing single-channel configs using top-level `power_sensor`, `voltage_sensor`, `current_sensor`, and `energy_sensor` remain supported.

## Using the MyEnergi Ethernet decoder

The [myenergi-ethernet component](https://github.com/Ulrar/esphome-myenergi-ethernet) exposes CT channel power and current as ESPHome sensors. Assign IDs to the CT sensors you want to reuse, then reference those IDs in the Shelly channel entries. For example, two Zappis can feed a two-channel emulator:

~~~yaml
myenergi_ethernet:
  ethernet_id: wired_lan
  devices:
    - serial: 12345678 # Replace with the first charger's serial.
      ct_channels:
        - channel: 1
          power:
            id: zappi_one_ct_power
          current:
            id: zappi_one_ct_current
    - serial: 87654321 # Replace with the second charger's serial.
      ct_channels:
        - channel: 1
          power:
            id: zappi_two_ct_power
          current:
            id: zappi_two_ct_current

shelly_emulator:
  channels:
    - power_sensor: zappi_one_ct_power
      current_sensor: zappi_one_ct_current
    - power_sensor: zappi_two_ct_power
      current_sensor: zappi_two_ct_current
~~~

The decoder's CT power and current units match the emulator's expected units (watts and amps). Its per-session energy sensor is in kWh and resets between sessions, while the Shelly energy field is expected in Wh, so it should not be connected directly without an appropriate conversion and energy source.

Bluetooth RPC is optional. HTTP RPC continues to work without it. To enable BLE RPC, configure ESPHome's BLE components alongside the emulator:

~~~yaml
esp32_ble:
  name: ShellyEmulator

esp32_ble_server:
  id: shelly_ble_server

shelly_emulator:
  enable_ble: true
  ble_server_id: shelly_ble_server
  channels:
    - power_sensor: meter_power
~~~

The ESP32 board must have network connectivity configured independently. The emulator supports ESPHome Wi-Fi or Ethernet and serves HTTP RPC on port 80. `WiFi.Scan` returns nearby access points. For BLE provisioning, `WiFi.GetStatus` reports the configured `wifi_ssid` and active IP; `WiFi.SetConfig` updates the emulated SSID without changing the ESPHome network or retaining the supplied password.

Each channel can map an ESPHome switch to Shelly's output state and switch commands. The `on_switch_state` automation can also handle those commands without configuring an output switch.
