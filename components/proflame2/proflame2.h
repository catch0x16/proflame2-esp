#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/remote_base/remote_base.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"

#include <cstring>
#include <vector>

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
//
// Optionally also a remote proxy: frames from a second (external) remote heard on a
// remote_receiver are decoded, adopted as the current state and re-sent under our own
// serial number and checksum constants. The remote owns the state; Home Assistant only
// sees it through read-only sensors.
//
// Power override: while engaged, every frame we send has power forced off, whatever the
// remote asks for. The remote keeps re-sending its last state, so a plain "off" from Home
// Assistant would be undone at its next frame. The override is released by the remote's
// next power-off frame (someone turned it off), after which the remote is in charge again.
class ProFlame2Component : public Component,
                           public remote_base::RemoteTransmittable,
                           public remote_base::RemoteReceiverListener {
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
    // Identity of the external remote whose commands we accept and re-send.
    void set_receive_config(uint32_t serial, uint8_t c1, uint8_t d1, uint8_t c2, uint8_t d2) {
        this->rx_enabled_ = true;
        this->rx_serial_ = serial & 0xFFFFFF;
        this->rx_chk_c1_ = c1 & 0x0F;
        this->rx_chk_d1_ = d1 & 0x0F;
        this->rx_chk_c2_ = c2 & 0x0F;
        this->rx_chk_d2_ = d2 & 0x0F;
    }

    // RemoteReceiverListener: called from remote_receiver's loop with one captured frame.
    bool on_receive(remote_base::RemoteReceiveData data) override;

    // Control methods (for lambdas; the remote proxy uses set_state). Every call
    // transmits: each frame carries the full state, so re-sending an unchanged value is
    // harmless. The power override still applies to what is sent.
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
    // Engaging sends power off immediately (even if already engaged). Releasing re-sends
    // the remote's last state, so the fireplace follows the remote again right away.
    void set_override(bool engaged);
    bool is_override_engaged() const { return this->override_; }
    // Re-send the current state (with the override applied), e.g. after the fireplace
    // missed a frame or was changed with the paired remote. A no-op until the external
    // remote has sent a state: the boot defaults are not a real state.
    void sync_state();
    bool is_state_valid() const { return this->state_valid_; }

    // Read-only state entities
    void set_power_sensor(binary_sensor::BinarySensor *s) { this->power_sensor_ = s; }
    void set_pilot_sensor(binary_sensor::BinarySensor *s) { this->pilot_sensor_ = s; }
    void set_aux_sensor(binary_sensor::BinarySensor *s) { this->aux_sensor_ = s; }
    void set_front_sensor(binary_sensor::BinarySensor *s) { this->front_sensor_ = s; }
    void set_thermostat_sensor(binary_sensor::BinarySensor *s) { this->thermostat_sensor_ = s; }
    void set_state_valid_sensor(binary_sensor::BinarySensor *s) { this->state_valid_sensor_ = s; }
    void set_flame_sensor(sensor::Sensor *s) { this->flame_sensor_ = s; }
    void set_fan_sensor(sensor::Sensor *s) { this->fan_sensor_ = s; }
    void set_light_sensor(sensor::Sensor *s) { this->light_sensor_ = s; }

    void set_override_switch(switch_::Switch *sw) { this->override_switch_ = sw; }

    // Last state requested by the remote (or a lambda). What is actually sent is
    // effective_state(): this with the power override applied.
    ProFlame2Command current_state_{};
    ProFlame2Command effective_state() const;
    // Queue the current state for transmission (starts immediately if the rate limit allows).
    void transmit_command();
    void build_packet(uint8_t *packet);
    void encode_manchester(const uint8_t *input, uint8_t *output, size_t input_bits);
    uint8_t calculate_checksum(uint8_t cmd_byte, uint8_t c_const, uint8_t d_const);

 protected:
    uint8_t calculate_parity(uint8_t data, uint8_t pad);

    // Decode one 182-bit Manchester packet starting at `start` in an on-air bit stream
    // into its 7 data bytes. Checks sync, guards, padding and parity only.
    bool decode_packet_(const std::vector<bool> &bits, size_t start, uint8_t *data);

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

    // current_state_ and the override survive reboots so Home Assistant shows the last
    // commanded settings. Restored for display only - nothing is transmitted at boot, so
    // a reboot can never re-light the fireplace on its own. Keeping the override means
    // the remote's next frame after a reboot is still forced off.
    struct SavedState {
        ProFlame2Command state;
        bool override_engaged;
        bool state_valid;
    };
    void save_state_();
    ESPPreferenceObject pref_;
    bool override_{false};
    // current_state_ came from the external remote (now or before a reboot), so
    // sync_state() has something real to send.
    bool state_valid_{false};

    // Configuration
    uint32_t serial_number_{0x12345678};  // 24 bits used; must be cloned from the paired remote
    // Defaults are the smartfire reference device's constants; override per device.
    uint8_t chk_c1_{0x0D};
    uint8_t chk_d1_{0x00};
    uint8_t chk_c2_{0x00};
    uint8_t chk_d2_{0x07};

    // External remote (receive) configuration
    bool rx_enabled_{false};
    uint32_t rx_serial_{0};
    uint8_t rx_chk_c1_{0};
    uint8_t rx_chk_d1_{0};
    uint8_t rx_chk_c2_{0};
    uint8_t rx_chk_d2_{0};

    // Entity references
    binary_sensor::BinarySensor *power_sensor_{nullptr};
    binary_sensor::BinarySensor *pilot_sensor_{nullptr};
    binary_sensor::BinarySensor *aux_sensor_{nullptr};
    binary_sensor::BinarySensor *front_sensor_{nullptr};
    binary_sensor::BinarySensor *thermostat_sensor_{nullptr};
    binary_sensor::BinarySensor *state_valid_sensor_{nullptr};

    sensor::Sensor *flame_sensor_{nullptr};
    sensor::Sensor *fan_sensor_{nullptr};
    sensor::Sensor *light_sensor_{nullptr};

    switch_::Switch *override_switch_{nullptr};

    // On-air bit rate of the Manchester-encoded stream (~416.7us per bit).
    static const uint32_t BIT_RATE = 2400;
    // Quiet time between the end of one burst and the start of the next.
    static const uint32_t MIN_TRANSMISSION_GAP = 200;  // ms
    // The remote repeats each packet every ~81ms within a burst. Repeats of the same
    // command closer than this are one button press; we also hold our own TX until the
    // remote has been quiet this long so the two bursts don't collide on air.
    static const uint32_t RX_REPEAT_WINDOW = 250;  // ms

    // Timing
    uint32_t last_transmission_{0};  // start of the last burst
    uint32_t last_burst_ms_{0};      // airtime of the last burst
    bool tx_pending_{false};
    uint32_t last_rx_{0};            // last accepted packet from the external remote
    uint8_t last_rx_cmd1_{0};
    uint8_t last_rx_cmd2_{0};
};

// Engages/releases the power override.
class ProFlame2OverrideSwitch : public switch_::Switch, public Parented<ProFlame2Component> {
 protected:
    void write_state(bool state) override { this->parent_->set_override(state); }
};

// Engages the override, which sends power off. Meant for automations ("turn it off at
// midnight if it's on"); check the power sensor first, because the override only
// releases when the remote next sends power off.
class ProFlame2ForceOffButton : public button::Button, public Parented<ProFlame2Component> {
 protected:
    void press_action() override { this->parent_->set_override(true); }
};

// Re-sends the current state. Does nothing until the remote has sent one.
class ProFlame2SyncButton : public button::Button, public Parented<ProFlame2Component> {
 protected:
    void press_action() override { this->parent_->sync_state(); }
};

}  // namespace proflame2
}  // namespace esphome
