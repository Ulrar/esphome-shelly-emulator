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

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ShellyEmulator),
        cv.Optional(CONF_ENABLE_BLE, default=False): cv.boolean,
        cv.Optional(CONF_BLE_SERVER_ID): cv.use_id(esp32_ble_server.BLEServer),
        cv.Optional(CONF_CHANNELS): cv.All(cv.ensure_list(CHANNEL_SCHEMA), cv.Length(min=1, max=4)),
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


def resolve_profile(config):
    channels = config.get(CONF_CHANNELS)
    if channels is None:
        channels = [config]
    channel_count = len(channels)
    has_controls = bool(config.get(CONF_ON_SWITCH_STATE)) or any(
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
    if CONF_CHANNELS in config:
        if any(key in config for key in legacy_sensor_keys):
            raise cv.Invalid("Use either channels or the legacy top-level sensor keys, not both")
        if len(config[CONF_CHANNELS]) not in (1, 2, 4):
            raise cv.Invalid("channels must contain 1, 2, or 4 entries to match a supported Shelly model")
    else:
        if CONF_POWER_SENSOR not in config:
            raise cv.Invalid("power_sensor is required when channels is not configured")
    resolve_profile(config)
    return config


FINAL_VALIDATE_SCHEMA = validate_config


async def to_code(config):
    include_builtin_idf_component("esp_http_server")
    include_builtin_idf_component("esp_wifi")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_em_gen3(resolve_profile(config) == "em_gen3"))
    if config[CONF_ENABLE_BLE]:
        cg.add_build_flag("-DUSE_SHELLY_EMULATOR_BLE")
        ble_server = await cg.get_variable(config[CONF_BLE_SERVER_ID])
        cg.add(var.set_ble_server(ble_server))
        cg.add(var.set_ble_enabled(True))
    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_wifi_ssid(config[CONF_WIFI_SSID]))
    channels = config.get(CONF_CHANNELS)
    if channels is None:
        channels = [
            {
                key: config[key]
                for key in (CONF_POWER_SENSOR, CONF_VOLTAGE_SENSOR, CONF_FREQUENCY_SENSOR, CONF_CURRENT_SENSOR, CONF_ENERGY_SENSOR)
                if key in config
            }
        ]
    cg.add(var.set_channel_count(len(channels)))
    for channel_id, channel in enumerate(channels):
        for key, setter in (
            (CONF_POWER_SENSOR, "set_channel_power_sensor"),
            (CONF_VOLTAGE_SENSOR, "set_channel_voltage_sensor"),
            (CONF_FREQUENCY_SENSOR, "set_channel_frequency_sensor"),
            (CONF_CURRENT_SENSOR, "set_channel_current_sensor"),
            (CONF_ENERGY_SENSOR, "set_channel_energy_sensor"),
        ):
            if key in channel:
                sensor_var = await cg.get_variable(channel[key])
                cg.add(getattr(var, setter)(channel_id, sensor_var))
        if "output_switch" in channel:
            output_switch = await cg.get_variable(channel["output_switch"])
            cg.add(var.set_channel_output_switch(channel_id, output_switch))

    for conf in config.get(CONF_ON_SWITCH_STATE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_switch_state_trigger(trigger))
        await automation.build_automation(
            trigger, [(cg.uint8, "channel_id"), (cg.bool_, "is_on")], conf
        )
