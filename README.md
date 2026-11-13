# Shelly multi-channel emulator for ESPHome

This external component exposes ESPHome sensor readings through Shelly RPC over HTTP on port 80. It supports ESPHome Wi-Fi and Ethernet, and emulates Shelly Gen 2 switch metering or Shelly EM Gen 3 meter channels.

Each channel requires a power sensor. Voltage, current, and cumulative energy sensors are optional. The default `profile: auto` selects Plus 1PM for one channel and Pro 4PM for four channels. With two channels, it selects EM Gen 3 when no switch controls are configured, or Plus 2PM when `output_switch` or `on_switch_state` is configured. EM Gen 3 exposes meter RPCs without switch controls. You can set `profile` to `plus1pm`, `plus2pm`, `pro4pm`, or `em_gen3` to choose a profile explicitly. The example below has two channels and no controls, so it selects EM Gen 3:

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

The [myenergi-ethernet component](https://github.com/Ulrar/esphome-myenergi-ethernet) exposes CT channel power and current as ESPHome sensors. The example below connects two Zappis to the emulator and integrates each charger's positive power into a persistent watt-hour total. Negative CT power is clamped to zero before integration, which avoids reducing the total when a channel reports negative power at idle. Its two channels have no switch controls, so the emulator selects the EM Gen 3 profile automatically.

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

sensor:
  - platform: template
    name: Zappi 1 positive power
    id: zappi_one_positive_power
    lambda: |-
      const float power = id(zappi_one_ct_power).state;
      return power > 0.0f ? power : 0.0f;
    update_interval: 1s
  - platform: integration
    name: Zappi 1 energy total
    id: zappi_one_energy_wh
    sensor: zappi_one_positive_power
    time_unit: h
    restore: true
    unit_of_measurement: Wh
    device_class: energy
    state_class: total_increasing
  - platform: template
    name: Zappi 2 positive power
    id: zappi_two_positive_power
    lambda: |-
      const float power = id(zappi_two_ct_power).state;
      return power > 0.0f ? power : 0.0f;
    update_interval: 1s
  - platform: integration
    name: Zappi 2 energy total
    id: zappi_two_energy_wh
    sensor: zappi_two_positive_power
    time_unit: h
    restore: true
    unit_of_measurement: Wh
    device_class: energy
    state_class: total_increasing

preferences:
  flash_write_interval: 60s

shelly_emulator:
  channels:
    - power_sensor: zappi_one_ct_power
      current_sensor: zappi_one_ct_current
      energy_sensor: zappi_one_energy_wh
    - power_sensor: zappi_two_ct_power
      current_sensor: zappi_two_ct_current
      energy_sensor: zappi_two_energy_wh
~~~

The integration input is in watts and uses hours as its integration unit, producing watt-hours for the Shelly energy fields. `restore: true` restores the totals after restarts; ESPHome writes preferences at the configured interval.

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

The emulator can notify ESPHome automations when a Shelly switch changes state. The trigger arguments are `channel_id` (zero-based Shelly channel) and `is_on`. Repeated idempotent `Switch.Set` requests do not trigger another action.

Use `on_switch_state` with ESPHome's Home Assistant Select platform to connect a Shelly channel to a cloud-backed Home Assistant mode select. Calling `select.set` on the imported ESPHome select invokes Home Assistant's `select.select_option` action.

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

The emulator reads the referenced switch state for `Switch.GetStatus.output` and routes `Switch.Set` and `Switch.Toggle` to that switch. If `output_switch` is omitted, the channel reports `false`.

