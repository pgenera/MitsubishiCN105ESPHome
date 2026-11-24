#pragma once
#include "cn105_protocol.h"
#include "cn105_types.h"
#include "heatpumpFunctions.h"
#include "localization.h"
#include <cmath>
#include "esphome/components/climate/climate_mode.h"

namespace esphome::cn105 {

// Production PAC state transitions; the standalone tests use these same helpers.
struct HeatPumpProtocol {
    struct SetpointState {
        uint8_t version;
        uint8_t mode;
        float target_low;
        float target_high;
    } __attribute__((packed));
    static SetpointState saved_setpoint_state(climate::ClimateMode mode, float low, float high) {
        return {1, static_cast<uint8_t>(mode), low, high};
    }
    static std::optional<SetpointState> restored_setpoint_state(bool enabled, bool loaded, const SetpointState& state, bool supports_dual) {
        if (!enabled || !loaded || state.version != 1 || state.mode != static_cast<uint8_t>(climate::CLIMATE_MODE_HEAT_COOL) ||
            std::isnan(state.target_low) || std::isnan(state.target_high) || !supports_dual) return std::nullopt;
        return state;
    }

    // Per-mode setpoint persistence (COOL / HEAT / AUTO), akin to the Mitsubishi IR remote.
    // Stored as 0.5°C steps from 10.0°C (covering both Encoding A 16..31°C and Encoding B
    // 10..31°C) in a single 4-byte packed record (1 ESP8266 preference slot).
    struct ModeSetpoints {
        static constexpr uint8_t kVersion = 1;
        static constexpr uint8_t kUnset = 0xFF;
        static constexpr float kMinTemp = 10.0f;
        static constexpr float kMaxTemp = 31.0f;
        static constexpr float kStep = 0.5f;

        uint8_t version = kVersion;
        uint8_t cool_steps = kUnset;
        uint8_t heat_steps = kUnset;
        uint8_t auto_steps = kUnset;

        static uint8_t encode(float temp) {
            if (std::isnan(temp) || temp < kMinTemp || temp > kMaxTemp) return kUnset;
            return static_cast<uint8_t>(std::lround((temp - kMinTemp) / kStep));
        }

        static std::optional<float> decode(uint8_t steps) {
            if (steps == kUnset) return std::nullopt;
            const float temp = kMinTemp + (steps * kStep);
            if (temp < kMinTemp || temp > kMaxTemp) return std::nullopt;
            return temp;
        }

        std::optional<float> get(climate::ClimateMode mode) const {
            switch (mode) {
                case climate::CLIMATE_MODE_COOL: return decode(cool_steps);
                case climate::CLIMATE_MODE_HEAT: return decode(heat_steps);
                case climate::CLIMATE_MODE_AUTO: return decode(auto_steps);
                default: return std::nullopt;
            }
        }

        std::optional<float> set(climate::ClimateMode mode, float temp) {
            const uint8_t encoded = encode(temp);
            if (encoded == kUnset) return std::nullopt;
            uint8_t* slot = nullptr;
            switch (mode) {
                case climate::CLIMATE_MODE_COOL: slot = &cool_steps; break;
                case climate::CLIMATE_MODE_HEAT: slot = &heat_steps; break;
                case climate::CLIMATE_MODE_AUTO: slot = &auto_steps; break;
                default: return std::nullopt;
            }
            *slot = encoded;
            return decode(encoded);
        }

        bool update_from_mode_str(const char* power, const char* mode, float temp) {
            if (power == nullptr || std::strcmp(power, "ON") != 0 || mode == nullptr) return false;
            const uint8_t encoded = encode(temp);
            if (encoded == kUnset) return false;
            uint8_t* slot = nullptr;
            if (std::strcmp(mode, "COOL") == 0) slot = &cool_steps;
            else if (std::strcmp(mode, "HEAT") == 0) slot = &heat_steps;
            else if (std::strcmp(mode, "AUTO") == 0) slot = &auto_steps;
            else return false;
            if (*slot == encoded) return false;
            *slot = encoded;
            return true;
        }

        static ModeSetpoints validated(bool loaded, const ModeSetpoints& raw) {
            if (!loaded || raw.version != kVersion) return ModeSetpoints{};
            ModeSetpoints out{};
            out.cool_steps = decode(raw.cool_steps).has_value() ? raw.cool_steps : kUnset;
            out.heat_steps = decode(raw.heat_steps).has_value() ? raw.heat_steps : kUnset;
            out.auto_steps = decode(raw.auto_steps).has_value() ? raw.auto_steps : kUnset;
            return out;
        }
    } __attribute__((packed));

    static std::optional<float> quantize_setpoint(float temp) {
        return ModeSetpoints::decode(ModeSetpoints::encode(temp));
    }

    static bool update_heat_cool_band(float& low, float& high, bool is_high, float temp) {
        const auto quantized = quantize_setpoint(temp);
        if (!quantized.has_value()) return false;
        if (is_high) {
            high = *quantized;
            if (!std::isnan(low) && low > high) low = high;
        } else {
            low = *quantized;
            if (!std::isnan(high) && high < low) high = low;
        }
        return true;
    }
    static float deadband_setting(float current, float low, float high) {
        if (current < low) return low;
        if (current > high) return high;
        return current;
    }
    template<typename Targets>
    static void reconcile_target_temperatures(Targets& targets, climate::ClimateMode mode, bool two_point, float temperature) {
        if (!two_point) {
            targets.setTargetTemperature(temperature);
            return;
        }
        if (mode == climate::CLIMATE_MODE_HEAT) {
            targets.setTargetTemperatureLow(temperature);
            if (std::isnan(targets.getTargetTemperatureHigh())) targets.setTargetTemperatureHigh(temperature);
        } else if (mode == climate::CLIMATE_MODE_COOL || mode == climate::CLIMATE_MODE_DRY) {
            targets.setTargetTemperatureHigh(temperature);
            if (std::isnan(targets.getTargetTemperatureLow())) targets.setTargetTemperatureLow(temperature);
        } else if (mode == climate::CLIMATE_MODE_AUTO || mode == climate::CLIMATE_MODE_HEAT_COOL) {
            // The hardware setpoint carries no information about an existing band.
            // Recentring it also ratchets Fahrenheit bands through the non-idempotent
            // conversion table (issue #673). Preserve both known bounds exactly.
            const bool low_defined = !std::isnan(targets.getTargetTemperatureLow());
            const bool high_defined = !std::isnan(targets.getTargetTemperatureHigh());
            if (low_defined && !high_defined) {
                targets.setTargetTemperatureHigh(targets.getTargetTemperatureLow() + 2.0f);
            } else if (!low_defined && high_defined) {
                targets.setTargetTemperatureLow(targets.getTargetTemperatureHigh() - 2.0f);
            } else if (!low_defined && !high_defined) {
                targets.setTargetTemperatureLow(temperature - 2.0f);
                targets.setTargetTemperatureHigh(temperature + 2.0f);
            }
        } else {
            // OFF / FAN_ONLY: keep the band for the next HEAT_COOL. Only write an edge
            // that changed: the Fahrenheit table round trip is not idempotent (#673).
            const float low = targets.getTargetTemperatureLow();
            const float high = targets.getTargetTemperatureHigh();
            const cn105_protocol::DualBand band = cn105_protocol::idle_dual_band(
                low, high, temperature,
                [&targets](float setting) { return targets.calculateTemperatureSetting(setting); });
            if (std::isnan(low) || band.low != low) targets.setTargetTemperatureLow(band.low);
            if (std::isnan(high) || band.high != high) targets.setTargetTemperatureHigh(band.high);
        }
    }

    // Packet 0x09 data[3] is a bitmask. Bit 0x01 is the filter reminder and can
    // stay set for weeks (#758). When another bit is set, drop 0x01 so 0x03
    // still maps to DEFROST and 0x05 to PREHEAT. Bare 0x01 stays WARMUP.
    static std::optional<const char*> decode_sub_mode(uint8_t raw) {
        uint8_t byte = raw;
        if (byte & 0xFE) {
            byte = static_cast<uint8_t>(byte & 0xFE);
        }
        return cn105_protocol::lookup_value_opt(SUB_MODE_MAP, SUB_MODE, 6, byte);
    }

    // The protocol cache and action refresh must not depend on a diagnostic sensor.
    template<typename RefreshAction>
    static bool apply_sub_mode(heatpumpSettings& current, const char* sub_mode, RefreshAction refresh_action) {
        if (sub_mode == nullptr || (current.sub_mode != nullptr && std::strcmp(current.sub_mode, sub_mode) == 0))
            return false;
        current.sub_mode = sub_mode;
        refresh_action();
        return true;
    }
    static bool accepts_remote_temperature(float setting) { return !std::isnan(setting); }
    static float normalize_remote_temperature(float setting, FahrenheitSupport& fahrenheit) {
        return setting > 0 ? fahrenheit.normalizeUiTemperatureToHeatpumpTemperature(setting) : setting;
    }
    static bool can_control_airflow(const heatpumpSettings& current) {
        return current.wideVane != nullptr && std::strcmp(current.wideVane, WIDEVANE_MAP[7]) == 0;
    }
    static bool decode_functions(heatpumpFunctions& functions, uint8_t code, const uint8_t* data, size_t length) {
        if (length < 16 || data[0] != code || (code != 0x20 && code != 0x22)) return false;
        // Codes with a zero value still announce support (e.g. SEZ outside installer mode).
        if (std::all_of(data + 1, data + length, [](uint8_t byte) { return byte == 0; })) return false;
        if (code == 0x20) functions.setData1(data + 1);
        else functions.setData2(data + 1);
        return true;
    }
    static bool encode_functions(uint8_t* first, uint8_t* second, const heatpumpFunctions& functions) {
        if (!functions.isValid()) return false;
        first[5] = 0x1F;
        second[5] = 0x21;
        functions.getData1(first + 6);
        functions.getData2(second + 6);
        return true;
    }
    static heatpumpRunStates optimistic_run_states(const wantedHeatpumpRunStates& wanted, const heatpumpRunStates& current) {
        heatpumpRunStates states = current;
        if (wanted.airflow_control != nullptr) states.airflow_control = wanted.airflow_control;
        if (wanted.air_purifier > -1) states.air_purifier = wanted.air_purifier;
        if (wanted.night_mode > -1) states.night_mode = wanted.night_mode;
        if (wanted.circulator > -1) states.circulator = wanted.circulator;
        return states;
    }
    static void encode_run_states(uint8_t* packet, const wantedHeatpumpRunStates& wanted, const heatpumpRunStates& current) {
        packet[5] = 0x08;
        if (wanted.airflow_control != nullptr) {
            const int index = cn105_protocol::lookup_index(AIRFLOW_CONTROL_MAP, 3, wanted.airflow_control);
            if (index >= 0) { packet[11] = AIRFLOW_CONTROL[index]; packet[6] |= RUN_STATE_PACKET_1[4]; }
        }
        if (wanted.air_purifier > -1 && static_cast<bool>(wanted.air_purifier) != current.air_purifier) {
            packet[17] = wanted.air_purifier ? 1 : 0; packet[7] |= RUN_STATE_PACKET_2[1];
        }
        if (wanted.night_mode > -1 && static_cast<bool>(wanted.night_mode) != current.night_mode) {
            packet[18] = wanted.night_mode ? 1 : 0; packet[7] |= RUN_STATE_PACKET_2[2];
        }
        if (wanted.circulator > -1 && static_cast<bool>(wanted.circulator) != current.circulator) {
            packet[19] = wanted.circulator ? 1 : 0; packet[7] |= RUN_STATE_PACKET_2[3];
        }
    }
    static float calculate_temperature_setting(float setting, bool encoding_b, bool special_table = false) {
        if (special_table) {
            setting = std::round(2.0f * setting) / 2.0f;
            return setting < 16.0f ? 16.0f : (setting > 31.0f ? 31.0f : setting);
        }
        if (!encoding_b) {
            // Encoding A: TEMP_MAP is stored 31..16. Clamp to that range before lookup
            // so a miss never falls through to TEMP_MAP[0] (31°C) for a low setpoint.
            if (std::isnan(setting))
                return setting;
            setting = setting < 16.0f ? 16.0f : (setting > 31.0f ? 31.0f : setting);
            if (cn105_protocol::lookup_index(TEMP_MAP, 16, static_cast<int>(setting + 0.5)) > -1)
                return setting;
            return setting;  // already clamped to the nearer bound of 16–31
        }
        setting = std::round(2.0f * setting) / 2.0f;
        return setting < 10 ? 10 : (setting > 31 ? 31 : setting);
    }
    static void set_vane_setting(wantedHeatpumpSettings& wanted, const char* setting, uint32_t now) {
        const int index = cn105_protocol::lookup_index(VANE_MAP, 7, setting);
        wanted.vane = VANE_MAP[index >= 0 ? index : 0];
        if (index >= 0) {
            wanted.last_user_vane = wanted.vane;
            wanted.last_user_vane_ms = now;
        }
    }
    static const char* vane_for_packet(const wantedHeatpumpSettings& wanted) {
        return wanted.vane != nullptr ? wanted.vane : wanted.last_user_vane;
    }
    static const char* wide_vane_setting(wantedHeatpumpSettings& wanted, const heatpumpSettings& current) {
        if (wanted.wideVane == nullptr) return current.wideVane;
        // AIRFLOW CONTROL requires an active i-See sensor. Keep the current position otherwise,
        // including in the optimistic settings that will be published after this write.
        if (std::strcmp(wanted.wideVane, WIDEVANE_MAP[7]) == 0 && !current.iSee)
            wanted.wideVane = current.wideVane;
        return wanted.wideVane;
    }
    static void encode_control(uint8_t* packet, wantedHeatpumpSettings& wanted, const heatpumpSettings& current,
                               bool encoding_b, bool special_table, bool wide_adjust, bool split_horizontal) {
        const auto field = [&](const char* value, const char** names, const uint8_t* bytes,
                               int size, int offset, int mask_offset, uint8_t mask) {
            if (value == nullptr) return;
            const int index = cn105_protocol::lookup_index(names, size, value);
            if (index >= 0) { packet[offset] = bytes[index]; packet[mask_offset] |= mask; }
        };
        field(wanted.power, POWER_MAP, POWER, 2, 8, 6, CONTROL_PACKET_1[0]);
        field(wanted.mode, MODE_MAP, MODE, 5, 9, 6, CONTROL_PACKET_1[1]);
        if (wanted.temperature != -1.0f) {
            if (special_table) {
                packet[10] = cn105_protocol::encode_msz_a24na_setpoint(wanted.temperature);
                packet[6] |= CONTROL_PACKET_1[2];
            } else if (encoding_b) {
                packet[19] = static_cast<uint8_t>(wanted.temperature * 2 + 128);
                packet[6] |= CONTROL_PACKET_1[2];
            } else {
                const int index = cn105_protocol::lookup_index(TEMP_MAP, 16, static_cast<int>(wanted.temperature));
                if (index >= 0) { packet[10] = TEMP[index]; packet[6] |= CONTROL_PACKET_1[2]; }
            }
        }
        field(wanted.fan, FAN_MAP, FAN, 6, 11, 6, CONTROL_PACKET_1[3]);
        field(vane_for_packet(wanted), VANE_MAP, VANE, 7, 12, 6, CONTROL_PACKET_1[4]);
        if (wanted.wideVane != nullptr) {
            const char* setting = wide_vane_setting(wanted, current);
            const int index = setting != nullptr ? cn105_protocol::lookup_index(WIDEVANE_MAP, 8, setting) : -1;
            if (index >= 0) {
                packet[18] = WIDEVANE[index] | (wide_adjust ? 0x80 : 0x00);
                packet[7] |= CONTROL_PACKET_2[0];
                if (split_horizontal) packet[16] = WIDEVANE[index];
            }
        }
    }
    static void encode_remote_temperature(uint8_t* packet, float temperature) {
        packet[5] = 0x07;
        if (temperature > 0) {
            packet[6] = 0x01;
            cn105_protocol::encode_remote_temperature(temperature, packet[7], packet[8]);
        } else packet[8] = 0x80;
    }
    static climate::ClimateMode resolve_mode(const char* power, const char* reported,
                                             climate::ClimateMode mode, bool supports_dual) {
        if (power == nullptr) return mode;
        if (std::strcmp(power, "ON") != 0) return climate::CLIMATE_MODE_OFF;
        if (reported == nullptr) return mode;
        const bool hold = supports_dual && mode == climate::CLIMATE_MODE_HEAT_COOL;
        if (std::strcmp(reported, "HEAT") == 0) return hold ? mode : climate::CLIMATE_MODE_HEAT;
        if (std::strcmp(reported, "COOL") == 0) return hold ? mode : climate::CLIMATE_MODE_COOL;
        if (std::strcmp(reported, "DRY") == 0) return climate::CLIMATE_MODE_DRY;
        if (std::strcmp(reported, "FAN") == 0) return climate::CLIMATE_MODE_FAN_ONLY;
        if (std::strcmp(reported, "AUTO") == 0)
            return mode == climate::CLIMATE_MODE_HEAT_COOL ? mode : climate::CLIMATE_MODE_AUTO;
        return mode;
    }
    static climate::ClimateMode reconcile_power_mode(const heatpumpSettings& settings, heatpumpSettings& current,
                                                      climate::ClimateMode mode, bool supports_dual, bool update_current) {
        const auto changed = [](const char* value, const char* previous) {
            return value != nullptr && (previous == nullptr || std::strcmp(value, previous) != 0);
        };
        const bool update = changed(settings.power, current.power) || changed(settings.mode, current.mode);
        if (update_current) {
            if (settings.power != nullptr) current.power = settings.power;
            if (settings.mode != nullptr) current.mode = settings.mode;
        }
        return update ? resolve_mode(settings.power, settings.mode, mode, supports_dual) : mode;
    }
    static uint8_t compressor_frequency(const uint8_t* data, bool report_when_idle) {
        return report_when_idle || data[4] > 0 ? data[3] : 0;
    }
    static climate::ClimateAction operating_action(bool operating, bool use_stage, const char* stage,
                                                    climate::ClimateAction active_action) {
        const bool stage_active = use_stage && stage != nullptr && std::strcmp(stage, STAGE_MAP[0]) != 0;
        return operating || stage_active ? active_action : climate::CLIMATE_ACTION_IDLE;
    }
    static float decode_temperature(const uint8_t* data, float previous, bool special_table,
                                    bool& encoding_b, bool& encoding_b_latched) {
        if (special_table) return cn105_protocol::decode_msz_a24na_setpoint(data[5]);
        if (data[11] == 0x80) return previous;
        if (data[11] != 0x00) {
            encoding_b = true;
            encoding_b_latched = true;
            return static_cast<float>(data[11] - 128) / 2.0f;
        }
        const auto value = cn105_protocol::lookup_value_opt(TEMP_MAP, TEMP, 16, data[5]);
        return value ? static_cast<float>(*value) : previous;
    }
};

}  // namespace esphome::cn105
