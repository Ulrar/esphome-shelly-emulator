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

The emulator can notify ESPHome automations when a Shelly switch changes state. The trigger arguments are channel_id (zero-based Shelly channel) and is_on. Repeated idempotent Switch.Set requests do not trigger another action.

Use on_switch_state with ESPHome's Home Assistant Select platform to connect a Shelly channel to a cloud-backed Home Assistant mode select. select.set on the imported ESPHome select calls Home Assistant's select.select_option action.

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


## Mapping Shelly channels to Home Assistant modes

Each emulator channel can reference an ESPHome switch whose state supplies Shelly's `output` field. An internal template switch can read the current Home Assistant mode and set it when the gateway sends a switch command:

~~~yaml
switch:
  - platform: template
    id: charger_one_fast_mode
    internal: true
    lambda: 'return id(charger_one_mode).state == "Fast";'
    turn_on_action:
      - homeassistant.action:
          action: select.select_option
          data:
            entity_id: select.charger_one_charge_mode
            option: Fast
    turn_off_action:
      - homeassistant.action:
          action: select.select_option
          data:
            entity_id: select.charger_one_charge_mode
            option: Stopped

shelly_emulator:
  channels:
    - power_sensor: charger_one_power
      output_switch: charger_one_fast_mode
~~~

The emulator reads the referenced switch's state for `Switch.GetStatus.output`, and routes `Switch.Set` and `Switch.Toggle` to that switch. If `output_switch` is omitted, the channel reports `false`.
