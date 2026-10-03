#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/remote_base/remote_base.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"

#include <cstring>

namespace esphome {
namespace proflame2 {

// ProFlame 2 packet structure
struct ProFlame2Command {
    // Command Word 1: CPI | Light[3] | 00 | Thermostat | Power
    bool pilot_cpi;      // 0=IPI, 1=CPI
    uint8_t light_level; // 0-6
    bool thermostat;
    bool power;

    // Command Word 2: Front | Fan[3] | Aux | Flame[3]
    bool front_flame;
    uint8_t fan_level;   // 0-6
    bool aux_power;
    uint8_t flame_level; // 0-6
};

// Protocol-only component: builds the ProFlame 2 frame and hands the on-air burst
// to a remote_transmitter as mark/space timings. The radio itself (reset, register
// setup, calibration, TX/RX switching) is owned by ESPHome's cc1101 component, driven
// from the transmitter's on_transmit/on_complete triggers.
class ProFlame2Component : public Component, public remote_base::RemoteTransmittable {
 public:
    void setup() override;
    void loop() override;
    void dump_config() override;

    // Configuration methods
    void set_serial_number(uint32_t serial) { this->serial_number_ = serial; }
    // Error-detection word constants. These are device specific: derive them from an
    // rtl_433 capture of the paired remote (see README "Checksum Constants").
    void set_checksum_constants(uint8_t c1, uint8_t d1, uint8_t c2, uint8_t d2) {
        this->chk_c1_ = c1 & 0x0F;
        this->chk_d1_ = d1 & 0x0F;
        this->chk_c2_ = c2 & 0x0F;
        this->chk_d2_ = d2 & 0x0F;
    }

    // Control methods. Every call transmits: each frame carries the full state, so
    // re-sending an unchanged value is harmless and keeps commands like "off" working
    // even when the fireplace was changed by the physical remote.
    // Replace the whole state at once and send it as a single frame.
    void set_state(const ProFlame2Command &state);
    void set_power(bool state);
    void set_pilot_mode(bool cpi_mode);
    void set_flame_level(uint8_t level);
    void set_fan_level(uint8_t level);
    void set_light_level(uint8_t level);
    void set_aux_power(bool state);
    void set_front_flame(bool state);
    void set_thermostat(bool state);

    // Switch components
    void set_power_switch(switch_::Switch *sw) { this->power_switch_ = sw; }
    void set_pilot_switch(switch_::Switch *sw) { this->pilot_switch_ = sw; }
    void set_aux_switch(switch_::Switch *sw) { this->aux_switch_ = sw; }
    void set_front_switch(switch_::Switch *sw) { this->front_switch_ = sw; }
    void set_thermostat_switch(switch_::Switch *sw) { this->thermostat_switch_ = sw; }

    // Number components for levels
    void set_flame_number(number::Number *num) { this->flame_number_ = num; }
    void set_fan_number(number::Number *num) { this->fan_number_ = num; }
    void set_light_number(number::Number *num) { this->light_number_ = num; }

    ProFlame2Command current_state_{};
    // Queue the current state for transmission (starts immediately if the rate limit allows).
    void transmit_command();
    void build_packet(uint8_t *packet);
    void encode_manchester(const uint8_t *input, uint8_t *output, size_t input_bits);
    uint8_t calculate_checksum(uint8_t cmd_byte, uint8_t c_const, uint8_t d_const);

 protected:
    uint8_t calculate_parity(uint8_t data, uint8_t pad);

    // Build a single on-air burst: 5x Manchester-encoded packets separated by 12 zero bits.
    // This matches the Proflame 2 burst structure described in FCC docs / smartfire reference.
    // Returns the number of bits written (0 on failure).
    size_t build_tx_burst_(const uint8_t *encoded, size_t encoded_bits,
                           uint8_t *out, size_t out_max_bytes,
                           uint8_t repeats = 5, uint8_t separator_zero_bits = 12);

    // Convert an MSB-first on-air bit stream into mark (1) / space (0) timings.
    void encode_timings_(const uint8_t *bits, size_t num_bits, remote_base::RemoteTransmitData *dst);

    void try_start_pending_tx_();

    // Clamp, persist, publish to entities and transmit current_state_.
    void state_changed_();
    void publish_state_();

    // current_state_ survives reboots so Home Assistant shows the last commanded
    // settings. It is restored for display only - nothing is transmitted at boot, so
    // a reboot can never re-light the fireplace on its own.
    ESPPreferenceObject pref_;

    // Configuration
    uint32_t serial_number_{0x12345678};  // 24 bits used; must be cloned from the paired remote
    // Defaults are the smartfire reference device's constants; override per device.
    uint8_t chk_c1_{0x0D};
    uint8_t chk_d1_{0x00};
    uint8_t chk_c2_{0x00};
    uint8_t chk_d2_{0x07};

    // Component references
    switch_::Switch *power_switch_{nullptr};
    switch_::Switch *pilot_switch_{nullptr};
    switch_::Switch *aux_switch_{nullptr};
    switch_::Switch *front_switch_{nullptr};
    switch_::Switch *thermostat_switch_{nullptr};

    number::Number *flame_number_{nullptr};
    number::Number *fan_number_{nullptr};
    number::Number *light_number_{nullptr};

    // On-air bit rate of the Manchester-encoded stream (~416.7us per bit).
    static const uint32_t BIT_RATE = 2400;
    // Quiet time between the end of one burst and the start of the next.
    static const uint32_t MIN_TRANSMISSION_GAP = 200;  // ms

    // Timing
    uint32_t last_transmission_{0};  // start of the last burst
    uint32_t last_burst_ms_{0};      // airtime of the last burst
    bool tx_pending_{false};
};

// Switch implementations
class ProFlame2PowerSwitch : public switch_::Switch, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void write_state(bool state) override {
        this->parent_->set_power(state);
    }
 protected:
    ProFlame2Component *parent_;
};

class ProFlame2PilotSwitch : public switch_::Switch, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void write_state(bool state) override {
        this->parent_->set_pilot_mode(state);
    }
 protected:
    ProFlame2Component *parent_;
};

class ProFlame2AuxSwitch : public switch_::Switch, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void write_state(bool state) override {
        this->parent_->set_aux_power(state);
    }
 protected:
    ProFlame2Component *parent_;
};

class ProFlame2FrontSwitch : public switch_::Switch, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void write_state(bool state) override {
        this->parent_->set_front_flame(state);
    }
 protected:
    ProFlame2Component *parent_;
};

class ProFlame2ThermostatSwitch : public switch_::Switch, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void write_state(bool state) override {
        this->parent_->set_thermostat(state);
    }
 protected:
    ProFlame2Component *parent_;
};

// Number component implementations
class ProFlame2FlameNumber : public number::Number, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void control(float value) override {
        uint8_t level = static_cast<uint8_t>(value);
        this->parent_->set_flame_level(level);
    }
 protected:
    ProFlame2Component *parent_;
};

class ProFlame2FanNumber : public number::Number, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void control(float value) override {
        uint8_t level = static_cast<uint8_t>(value);
        this->parent_->set_fan_level(level);
    }
 protected:
    ProFlame2Component *parent_;
};

class ProFlame2LightNumber : public number::Number, public Component {
 public:
    void set_parent(ProFlame2Component *parent) { this->parent_ = parent; }
    void control(float value) override {
        uint8_t level = static_cast<uint8_t>(value);
        this->parent_->set_light_level(level);
    }
 protected:
    ProFlame2Component *parent_;
};

}  // namespace proflame2
}  // namespace esphome
