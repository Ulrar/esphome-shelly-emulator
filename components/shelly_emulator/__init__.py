import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components.esp32 import include_builtin_idf_component
from esphome.components import esp32_ble_server, sensor, switch
from esphome.core import CORE
from esphome.const import CONF_ID

DEPENDENCIES = []

CONF_BLE_SERVER_ID = "ble_server_id"
CONF_ENABLE_BLE = "enable_ble"
CONF_CHANNELS = "channels"
CONF_POWER_SENSOR = "power_sensor"
CONF_VOLTAGE_SENSOR = "voltage_sensor"
CONF_FREQUENCY_SENSOR = "frequency_sensor"
CONF_CURRENT_SENSOR = "current_sensor"
CONF_ENERGY_SENSOR = "energy_sensor"
CONF_DEVICE_NAME = "device_name"
CONF_PROFILE = "profile"
CONF_WIFI_SSID = "wifi_ssid"
CONF_TRIGGER_ID = "trigger_id"
CONF_ON_SWITCH_STATE = "on_switch_state"
CONF_DEVICES = "devices"
CONF_NAME = "name"
CONF_HOSTNAME = "hostname"

shelly_ns = cg.esphome_ns.namespace("shelly_emulator")
ShellyEmulator = shelly_ns.class_("ShellyEmulator", cg.Component)
SwitchStateTrigger = shelly_ns.class_(
    "SwitchStateTrigger", automation.Trigger.template(cg.uint8, cg.bool_)
)

CHANNEL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_POWER_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_VOLTAGE_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_FREQUENCY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_CURRENT_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional("output_switch"): cv.use_id(switch.Switch),
    }
)
DEVICE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string,
        cv.Required(CONF_HOSTNAME): cv.All(
            cv.string,
            cv.Length(min=1, max=20),
            cv.matches_regex(r"^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$"),
        ),
        cv.Optional(CONF_PROFILE, default="auto"): cv.one_of(
            "auto", "plus1pm", "plus2pm", "pro4pm", "em_gen3", lower=True
        ),
        cv.Required(CONF_CHANNELS): cv.All(cv.ensure_list(CHANNEL_SCHEMA), cv.Length(min=1, max=4)),
        cv.Optional(CONF_WIFI_SSID, default="Ethernet"): cv.string,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ShellyEmulator),
        cv.Optional(CONF_ENABLE_BLE, default=False): cv.boolean,
        cv.Optional(CONF_BLE_SERVER_ID): cv.use_id(esp32_ble_server.BLEServer),
        cv.Optional(CONF_CHANNELS): cv.All(cv.ensure_list(CHANNEL_SCHEMA), cv.Length(min=1, max=4)),
        cv.Optional(CONF_DEVICES): cv.All(cv.ensure_list(DEVICE_SCHEMA), cv.Length(min=2, max=4)),
        # Legacy single-channel sensor keys. These are normalized to channels[0].
        cv.Optional(CONF_POWER_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_VOLTAGE_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_FREQUENCY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_CURRENT_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_DEVICE_NAME, default="ESPHome Metering Device"): cv.string,
        cv.Optional(CONF_PROFILE, default="auto"): cv.one_of(
            "auto", "plus1pm", "plus2pm", "pro4pm", "em_gen3", lower=True
        ),
        cv.Optional(CONF_WIFI_SSID, default="Ethernet"): cv.string,
        cv.Optional(CONF_ON_SWITCH_STATE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SwitchStateTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def resolve_profile(config, has_switch_trigger=False):
    channels = config.get(CONF_CHANNELS)
    if channels is None:
        channels = [config]
    channel_count = len(channels)
    has_controls = has_switch_trigger or bool(config.get(CONF_ON_SWITCH_STATE)) or any(
        "output_switch" in channel for channel in channels
    )

    profile = config[CONF_PROFILE]
    if profile == "auto":
        if channel_count == 2 and not has_controls:
            return "em_gen3"
        profile = {1: "plus1pm", 2: "plus2pm", 4: "pro4pm"}[channel_count]

    profile_channels = {"plus1pm": 1, "plus2pm": 2, "pro4pm": 4, "em_gen3": 2}
    if channel_count != profile_channels[profile]:
        raise cv.Invalid(f"profile {profile} requires {profile_channels[profile]} channel(s)")
    if profile == "em_gen3" and has_controls:
        raise cv.Invalid("profile em_gen3 does not support switch controls")
    return profile


def validate_config(config):
    if not CORE.is_esp32:
        raise cv.Invalid("shelly_emulator is only available on ESP32")
    if CORE.using_arduino:
        raise cv.Invalid("shelly_emulator requires the ESP-IDF framework")
    if config[CONF_ENABLE_BLE] and CONF_BLE_SERVER_ID not in config:
        raise cv.Invalid("ble_server_id is required when enable_ble is true")
    if not config[CONF_ENABLE_BLE] and CONF_BLE_SERVER_ID in config:
        raise cv.Invalid("ble_server_id can only be set when enable_ble is true")

    legacy_sensor_keys = (
        CONF_POWER_SENSOR,
        CONF_VOLTAGE_SENSOR,
        CONF_FREQUENCY_SENSOR,
        CONF_CURRENT_SENSOR,
        CONF_ENERGY_SENSOR,
    )
    if CONF_DEVICES in config:
        if CONF_CHANNELS in config or any(key in config for key in legacy_sensor_keys):
            raise cv.Invalid("Use either devices or channels and top-level sensors, not both")
        if not config[CONF_ENABLE_BLE]:
            raise cv.Invalid("enable_ble is required when configuring multiple virtual Shelly devices")
        devices = config[CONF_DEVICES]
        names = [device[CONF_NAME].casefold() for device in devices]
        hostnames = [device[CONF_HOSTNAME].casefold() for device in devices]
        if len(set(names)) != len(names):
            raise cv.Invalid("Each virtual Shelly must have a unique name")
        if len(set(hostnames)) != len(hostnames):
            raise cv.Invalid("Each virtual Shelly must have a unique hostname")
        for device in devices:
            if len(device[CONF_CHANNELS]) not in (1, 2, 4):
                raise cv.Invalid("Each device must have 1, 2, or 4 channels")
            resolve_profile(device, bool(config.get(CONF_ON_SWITCH_STATE)))
    else:
        if CONF_CHANNELS in config:
            if any(key in config for key in legacy_sensor_keys):
                raise cv.Invalid("Use either channels or the legacy top-level sensor keys, not both")
            if len(config[CONF_CHANNELS]) not in (1, 2, 4):
                raise cv.Invalid("channels must contain 1, 2, or 4 entries to match a supported Shelly model")
        elif CONF_POWER_SENSOR not in config:
            raise cv.Invalid("power_sensor is required when channels is not configured")
        resolve_profile(config)
    return config

FINAL_VALIDATE_SCHEMA = validate_config


async def to_code(config):
    include_builtin_idf_component("esp_http_server")
    include_builtin_idf_component("esp_wifi")
    include_builtin_idf_component("mdns")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if config[CONF_ENABLE_BLE]:
        cg.add_build_flag("-DUSE_SHELLY_EMULATOR_BLE")
        ble_server = await cg.get_variable(config[CONF_BLE_SERVER_ID])
        cg.add(var.set_ble_server(ble_server))
        cg.add(var.set_ble_enabled(True))
    devices = config.get(CONF_DEVICES)
    if devices is None:
        channels = config.get(CONF_CHANNELS)
        if channels is None:
            channels = [
                {
                    key: config[key]
                    for key in (
                        CONF_POWER_SENSOR,
                        CONF_VOLTAGE_SENSOR,
                        CONF_FREQUENCY_SENSOR,
                        CONF_CURRENT_SENSOR,
                        CONF_ENERGY_SENSOR,
                    )
                    if key in config
                }
            ]
        devices = [
            {
                CONF_NAME: config[CONF_DEVICE_NAME],
                CONF_HOSTNAME: "",
                CONF_PROFILE: config[CONF_PROFILE],
                CONF_WIFI_SSID: config[CONF_WIFI_SSID],
                CONF_CHANNELS: channels,
            }
        ]
    cg.add(var.set_device_count(len(devices)))
    for device_index, device in enumerate(devices):
        cg.add(var.set_device_name(device_index, device[CONF_NAME]))
        cg.add(var.set_device_hostname(device_index, device[CONF_HOSTNAME]))
        cg.add(var.set_device_wifi_ssid(device_index, device[CONF_WIFI_SSID]))
        cg.add(var.set_device_channel_count(device_index, len(device[CONF_CHANNELS])))
        cg.add(var.set_device_em_gen3(device_index, resolve_profile(device, bool(config.get(CONF_ON_SWITCH_STATE))) == "em_gen3"))
        for channel_index, channel in enumerate(device[CONF_CHANNELS]):
            for key, setter in (
                (CONF_POWER_SENSOR, "set_channel_power_sensor"),
                (CONF_VOLTAGE_SENSOR, "set_channel_voltage_sensor"),
                (CONF_FREQUENCY_SENSOR, "set_channel_frequency_sensor"),
                (CONF_CURRENT_SENSOR, "set_channel_current_sensor"),
                (CONF_ENERGY_SENSOR, "set_channel_energy_sensor"),
            ):
                if key in channel:
                    sensor_var = await cg.get_variable(channel[key])
                    cg.add(getattr(var, setter)(device_index, channel_index, sensor_var))
            if "output_switch" in channel:
                output_switch = await cg.get_variable(channel["output_switch"])
                cg.add(var.set_channel_output_switch(device_index, channel_index, output_switch))

    for conf in config.get(CONF_ON_SWITCH_STATE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_switch_state_trigger(trigger))
        await automation.build_automation(
            trigger, [(cg.uint8, "channel_id"), (cg.bool_, "is_on")], conf
        )