#include "proflame2.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <algorithm>

namespace esphome {
namespace proflame2 {

static const char *TAG = "proflame2";

void ProFlame2Component::setup() {
    // Keyed on the serial number so a different remote's settings are never restored.
    this->pref_ = global_preferences->make_preference<SavedState>(
        fnv1_hash_extend(fnv1_hash("proflame2_v3"), this->serial_number_), true);

    SavedState saved{};
    if (this->pref_.load(&saved)) {
        this->current_state_ = saved.state;
        this->override_ = saved.override_engaged;
        this->state_valid_ = saved.state_valid;
        ESP_LOGI(TAG, "Restored last state (display only, not transmitted)%s%s",
                 this->override_ ? " - power override engaged" : "",
                 this->state_valid_ ? "" : " - not yet set by the remote");
    }
    // Publish even with nothing saved, so the sensors show 0 instead of unknown.
    this->publish_state_();
}

void ProFlame2Component::loop() {
    if (!this->tx_pending_) {
        this->disable_loop();  // re-enabled by transmit_command()
        return;
    }
    this->try_start_pending_tx_();
}

void ProFlame2Component::dump_config() {
    ESP_LOGCONFIG(TAG, "ProFlame 2:");
    ESP_LOGCONFIG(TAG, "  Serial Number: 0x%06X",
                  static_cast<unsigned>(this->serial_number_ & 0xFFFFFF));
    ESP_LOGCONFIG(TAG, "  Checksum constants: C1=0x%X D1=0x%X C2=0x%X D2=0x%X",
                  this->chk_c1_, this->chk_d1_, this->chk_c2_, this->chk_d2_);
    ESP_LOGCONFIG(TAG, "  On-air bit rate: %u baud", static_cast<unsigned>(BIT_RATE));
    if (this->rx_enabled_) {
        ESP_LOGCONFIG(TAG, "  Proxying remote: 0x%06X", static_cast<unsigned>(this->rx_serial_));
        ESP_LOGCONFIG(TAG, "    Checksum constants: C1=0x%X D1=0x%X C2=0x%X D2=0x%X",
                      this->rx_chk_c1_, this->rx_chk_d1_, this->rx_chk_c2_, this->rx_chk_d2_);
    }
}

uint8_t ProFlame2Component::calculate_parity(uint8_t data, uint8_t pad) {
    // Parity over the 8 data bits plus the padding bit:
    // 1 if odd number of ones, 0 if even.
    uint8_t ones = pad & 1;
    for (int i = 0; i < 8; i++) {
        if (data & (1 << i)) ones++;
    }
    return ones & 1;
}

uint8_t ProFlame2Component::calculate_checksum(uint8_t cmd_byte, uint8_t c_const, uint8_t d_const) {
    uint8_t high_nibble = (cmd_byte >> 4) & 0x0F;
    uint8_t low_nibble = cmd_byte & 0x0F;

    uint8_t x = (c_const ^ (high_nibble << 1) ^ high_nibble ^ (low_nibble << 1)) & 0x0F;
    uint8_t y = (d_const ^ high_nibble ^ low_nibble) & 0x0F;

    return (x << 4) | y;
}

void ProFlame2Component::build_packet(uint8_t *packet) {
    // Clear packet buffer (91 bits = 12 bytes)
    memset(packet, 0, 12);

    const ProFlame2Command st = this->effective_state();

    // Build command bytes
    // Command 1: CPI | Light[3] | 0 0 | Thermostat | Power
    uint8_t cmd1 = (st.pilot_cpi ? 0x80 : 0x00) |
                   ((st.light_level & 0x07) << 4) |
                   (st.thermostat ? 0x02 : 0x00) |
                   (st.power ? 0x01 : 0x00);

    // Command 2: Front | Fan[3] | Aux | Flame[3]
    uint8_t cmd2 = (st.front_flame ? 0x80 : 0x00) |
                   ((st.fan_level & 0x07) << 4) |
                   (st.aux_power ? 0x08 : 0x00) |
                   (st.flame_level & 0x07);

    // The 7 data bytes: 3 serial bytes, 2 command bytes, 2 error-detection bytes.
    // Checksum constants are device specific (verified against SDR captures of the
    // paired remote).
    const uint8_t data_bytes[7] = {
        static_cast<uint8_t>((this->serial_number_ >> 16) & 0xFF),
        static_cast<uint8_t>((this->serial_number_ >> 8) & 0xFF),
        static_cast<uint8_t>(this->serial_number_ & 0xFF),
        cmd1,
        cmd2,
        this->calculate_checksum(cmd1, this->chk_c1_, this->chk_d1_),
        this->calculate_checksum(cmd2, this->chk_c2_, this->chk_d2_),
    };

    // This line should match the rtl_433 proflame2 decode of our transmission
    // (fields id/cmd1/cmd2/err1/err2) - the primary SDR verification hook.
    ESP_LOGI(TAG, "Frame: id=%02x%02x%02x cmd1=%02x cmd2=%02x err1=%02x err2=%02x",
             data_bytes[0], data_bytes[1], data_bytes[2],
             data_bytes[3], data_bytes[4], data_bytes[5], data_bytes[6]);

    // Build 7 words of 13 bits each:
    //   bit12: S (sync placeholder, Manchester-encoded as '11')
    //   bit11: start guard = 1
    //   bits10-3: data byte (MSB first)
    //   bit2: padding (1 for the first word only)
    //   bit1: parity over data + padding
    //   bit0: end guard = 1
    int bit_index = 0;
    for (int w = 0; w < 7; w++) {
        const uint8_t pad = (w == 0) ? 1 : 0;
        const uint16_t word = 0x1000 | 0x0800 |
                              (static_cast<uint16_t>(data_bytes[w]) << 3) |
                              (pad << 2) |
                              (this->calculate_parity(data_bytes[w], pad) << 1) |
                              0x0001;

        // Pack word into the bit array, MSB first (91 bits total)
        for (int b = 12; b >= 0; b--) {
            if (word & (1 << b)) {
                packet[bit_index / 8] |= (1 << (7 - (bit_index % 8)));
            }
            bit_index++;
        }
    }
}

bool ProFlame2Component::decode_packet_(const std::vector<bool> &bits, size_t start, uint8_t *data) {
    // Inverse of build_packet() + encode_manchester(): 7 words of 26 half-bits each.
    // Caller guarantees start + 182 <= bits.size().
    size_t pos = start;
    for (int w = 0; w < 7; w++) {
        if (!bits[pos] || !bits[pos + 1]) {
            return false;  // sync symbol '11'
        }
        pos += 2;
        uint16_t word = 0;
        for (int b = 0; b < 12; b++, pos += 2) {
            if (bits[pos] == bits[pos + 1]) {
                return false;  // not a Manchester pair (10 -> 1, 01 -> 0)
            }
            word = (word << 1) | (bits[pos] ? 1 : 0);
        }
        // word: bit11 start guard | bits10-3 data | bit2 padding | bit1 parity | bit0 end guard
        const uint8_t byte = (word >> 3) & 0xFF;
        const uint8_t pad = (word >> 2) & 1;
        if (!(word & 0x800) || !(word & 0x001)) {
            return false;
        }
        if (pad != (w == 0 ? 1 : 0) || ((word >> 1) & 1) != this->calculate_parity(byte, pad)) {
            return false;
        }
        data[w] = byte;
    }
    return true;
}

bool ProFlame2Component::on_receive(remote_base::RemoteReceiveData data) {
    if (!this->rx_enabled_) {
        return false;
    }

    // Rebuild the on-air bit stream: every mark/space is a run of whole bit periods.
    // Rounding to the nearest period absorbs the OOK demodulator's edge jitter.
    const auto &raw = data.get_raw_data();
    std::vector<bool> bits;
    for (int32_t t : raw) {
        const uint32_t dur = std::min<uint32_t>(t < 0 ? -t : t, 100000);
        const uint32_t n = std::min<uint32_t>((dur * BIT_RATE + 500000) / 1000000, 16);
        bits.insert(bits.end(), n, t > 0);
    }
    // The packet's last half-bit is a 0 that can merge into the unreported idle space.
    bits.insert(bits.end(), 2, false);
    if (bits.size() < 182) {
        // Too short to hold a packet - demodulator noise. Very frequent, so VERY_VERBOSE.
        ESP_LOGVV(TAG, "RX: %u timings / %u bits - too short, ignored",
                  static_cast<unsigned>(raw.size()), static_cast<unsigned>(bits.size()));
        return false;
    }

    // Find the first aligned packet; leading noise just shifts where it starts.
    uint8_t d[7];
    size_t start = 0;
    for (; start + 182 <= bits.size(); start++) {
        if (this->decode_packet_(bits, start, d)) {
            break;
        }
    }
    if (start + 182 > bits.size()) {
        // Long enough to be a packet but nothing decoded: a garbled frame, or the
        // receiver's idle/filter/tolerance don't suit this signal.
        ESP_LOGV(TAG, "RX: %u timings / %u bits - no valid packet",
                 static_cast<unsigned>(raw.size()), static_cast<unsigned>(bits.size()));
        return false;
    }

    const uint32_t serial = (static_cast<uint32_t>(d[0]) << 16) | (d[1] << 8) | d[2];
    // Same field layout as the TX "Frame:" line and rtl_433's proflame2 decode.
    ESP_LOGD(TAG, "RX frame: id=%02x%02x%02x cmd1=%02x cmd2=%02x err1=%02x err2=%02x (bit offset %u)",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], static_cast<unsigned>(start));
    if (serial != this->rx_serial_) {
        // Our own transmissions are heard on the shared pin and land here too.
        ESP_LOGD(TAG, "RX: ignoring frame from 0x%06X (%s)", static_cast<unsigned>(serial),
                 serial == (this->serial_number_ & 0xFFFFFF) ? "our own transmission"
                                                             : "not the configured remote");
        return false;
    }
    const uint8_t cmd1 = d[3];
    const uint8_t cmd2 = d[4];
    const uint8_t err1 = this->calculate_checksum(cmd1, this->rx_chk_c1_, this->rx_chk_d1_);
    const uint8_t err2 = this->calculate_checksum(cmd2, this->rx_chk_c2_, this->rx_chk_d2_);
    if (d[5] != err1 || d[6] != err2) {
        ESP_LOGW(TAG, "RX: checksum mismatch from 0x%06X: cmd1=%02x cmd2=%02x err1=%02x err2=%02x "
                      "(expected err1=%02x err2=%02x) - check receive checksum constants",
                 static_cast<unsigned>(serial), cmd1, cmd2, d[5], d[6], err1, err2);
        return false;
    }

    // The remote sends each command as a burst of 5 packets; act on the first only.
    const uint32_t now = millis();
    const bool repeat = this->last_rx_ != 0 && now - this->last_rx_ < RX_REPEAT_WINDOW &&
                        cmd1 == this->last_rx_cmd1_ && cmd2 == this->last_rx_cmd2_;
    this->last_rx_ = now;
    this->last_rx_cmd1_ = cmd1;
    this->last_rx_cmd2_ = cmd2;
    if (repeat) {
        ESP_LOGD(TAG, "RX: repeat packet of the same press, ignored");
        return true;
    }

    ProFlame2Command st{};
    st.pilot_cpi = cmd1 & 0x80;
    st.light_level = (cmd1 >> 4) & 0x07;
    st.thermostat = cmd1 & 0x02;
    st.power = cmd1 & 0x01;
    st.front_flame = cmd2 & 0x80;
    st.fan_level = (cmd2 >> 4) & 0x07;
    st.aux_power = cmd2 & 0x08;
    st.flame_level = cmd2 & 0x07;

    ESP_LOGI(TAG, "RX command from remote 0x%06X: Power=%d, Pilot=%s, Flame=%d, Fan=%d, Light=%d, "
                  "Aux=%d, Front=%d, Thermo=%d - retransmitting%s",
             static_cast<unsigned>(serial), st.power, st.pilot_cpi ? "CPI" : "IPI", st.flame_level,
             st.fan_level, st.light_level, st.aux_power, st.front_flame, st.thermostat,
             this->override_ && st.power ? " with power forced off (override)" : "");
    if (this->override_ && !st.power) {
        // Someone turned it off at the remote: hand control back to it.
        this->override_ = false;
        ESP_LOGI(TAG, "Remote sent power off - power override released");
    }
    this->state_valid_ = true;
    this->set_state(st);
    return true;
}

void ProFlame2Component::encode_manchester(const uint8_t *input, uint8_t *output, size_t input_bits) {
    // Thomas Manchester variant:
    //   0 -> 01, 1 -> 10, Sync (first bit of each 13-bit word) -> 11
    const size_t out_bytes = (input_bits * 2 + 7) / 8;
    memset(output, 0, out_bytes);

    size_t out_index = 0;
    auto put_bit = [&](uint8_t bit) {
        if (bit) {
            output[out_index / 8] |= (1 << (7 - (out_index % 8)));
        }
        out_index++;
    };

    for (size_t i = 0; i < input_bits; i++) {
        const bool bit = (input[i / 8] >> (7 - (i % 8))) & 1;

        if ((i % 13) == 0) {
            // Sync symbol: 11 (regardless of placeholder bit value)
            put_bit(1);
            put_bit(1);
        } else if (bit) {
            put_bit(1);
            put_bit(0);
        } else {
            put_bit(0);
            put_bit(1);
        }
    }
}

size_t ProFlame2Component::build_tx_burst_(const uint8_t *encoded, size_t encoded_bits,
                                          uint8_t *out, size_t out_max_bytes,
                                          uint8_t repeats, uint8_t separator_zero_bits) {
    // Pack bits MSB-first. encoded_bits is 182 for Proflame2.
    if (encoded == nullptr || out == nullptr || out_max_bytes == 0 || repeats == 0) {
        return 0;
    }

    const size_t per_packet_bits = encoded_bits;
    const size_t total_bits = (per_packet_bits * repeats) + (separator_zero_bits * (repeats - 1));
    const size_t total_bytes = (total_bits + 7) / 8;
    if (total_bytes > out_max_bytes) {
        ESP_LOGE(TAG, "Burst too large: need %u bytes (max=%u)",
                 static_cast<unsigned>(total_bytes), static_cast<unsigned>(out_max_bytes));
        return 0;
    }

    memset(out, 0, total_bytes);

    auto get_encoded_bit = [&](size_t bit_index) -> uint8_t {
        // bit_index 0 is MSB of encoded[0]
        const size_t byte_index = bit_index / 8;
        const uint8_t bit_in_byte = 7 - (bit_index % 8);
        return (encoded[byte_index] >> bit_in_byte) & 0x01;
    };

    size_t out_bitpos = 0;
    auto put_bit = [&](uint8_t bit) {
        const size_t byte_index = out_bitpos / 8;
        const uint8_t bit_in_byte = 7 - (out_bitpos % 8);
        if (bit) {
            out[byte_index] |= (1u << bit_in_byte);
        }
        out_bitpos++;
    };

    for (uint8_t r = 0; r < repeats; r++) {
        for (size_t b = 0; b < per_packet_bits; b++) {
            put_bit(get_encoded_bit(b));
        }
        if (r + 1 < repeats) {
            // separator: 12 zero bits between packets
            for (uint8_t z = 0; z < separator_zero_bits; z++) {
                put_bit(0);
            }
        }
    }

    return total_bits;
}

void ProFlame2Component::encode_timings_(const uint8_t *bits, size_t num_bits,
                                         remote_base::RemoteTransmitData *dst) {
    // Each on-air bit lasts 1/BIT_RATE (~416.7us). Runs of equal bits merge into one
    // mark (carrier on) or space (carrier off). Edges are placed on the absolute
    // timeline and rounded there, so per-run rounding never accumulates into drift
    // across the ~400ms burst.
    auto get_bit = [&](size_t i) -> bool { return (bits[i / 8] >> (7 - (i % 8))) & 1; };
    auto edge_us = [](size_t i) -> uint32_t {
        return static_cast<uint32_t>((i * 1000000ULL + BIT_RATE / 2) / BIT_RATE);
    };

    size_t i = 0;
    while (i < num_bits) {
        const bool level = get_bit(i);
        size_t j = i + 1;
        while (j < num_bits && get_bit(j) == level) {
            j++;
        }
        const uint32_t duration = edge_us(j) - edge_us(i);
        if (level) {
            dst->mark(duration);
        } else {
            dst->space(duration);
        }
        i = j;
    }
}

void ProFlame2Component::transmit_command() {
    // Always queue: the pending flag is consumed by try_start_pending_tx_() once the
    // rate limit has elapsed. Because the packet is built at send time from
    // current_state_, rapid changes coalesce into the latest state instead of being
    // dropped.
    this->tx_pending_ = true;
    this->enable_loop();
    this->try_start_pending_tx_();
}

void ProFlame2Component::try_start_pending_tx_() {
    if (!this->tx_pending_) {
        return;
    }

    // Wait out the previous burst's airtime plus a quiet gap. remote_transmitter is
    // non-blocking, so starting earlier would stall the main loop until it finished.
    const uint32_t now = millis();
    if (this->last_transmission_ != 0 &&
        now - this->last_transmission_ < this->last_burst_ms_ + MIN_TRANSMISSION_GAP) {
        return;  // loop() retries shortly
    }
    // Let the external remote finish its burst first so ours doesn't collide with it.
    if (this->last_rx_ != 0 && now - this->last_rx_ < RX_REPEAT_WINDOW) {
        return;
    }
    this->tx_pending_ = false;

    const ProFlame2Command st = this->effective_state();
    ESP_LOGI(TAG, "TX state: Power=%d%s, Pilot=%s, Flame=%d, Fan=%d, Light=%d, Aux=%d, Front=%d, Thermo=%d",
             st.power, this->override_ ? " (override)" : "",
             st.pilot_cpi ? "CPI" : "IPI",
             st.flame_level,
             st.fan_level,
             st.light_level,
             st.aux_power,
             st.front_flame,
             st.thermostat);

    // Build packet (91 bits)
    uint8_t packet[12];
    this->build_packet(packet);

    ESP_LOGD(TAG, "Raw packet (91 bits): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             packet[0], packet[1], packet[2], packet[3], packet[4], packet[5],
             packet[6], packet[7], packet[8], packet[9], packet[10], packet[11]);

    // Manchester encode: 91 bits -> 182 bits = 23 bytes
    uint8_t encoded[23];
    this->encode_manchester(packet, encoded, 91);

    // Build the on-air burst: 5 identical packets separated by 12 zero bits,
    // matching the captured remote (395ms burst at 2400 baud).
    uint8_t burst[120];
    const size_t burst_bits = this->build_tx_burst_(encoded, 182, burst, sizeof(burst), 5, 12);
    if (burst_bits == 0) {
        ESP_LOGE(TAG, "Failed to build TX burst");
        return;
    }

    auto call = this->transmitter_->transmit();
    this->encode_timings_(burst, burst_bits, call.get_data());
    this->last_burst_ms_ = (burst_bits * 1000 + BIT_RATE - 1) / BIT_RATE;

    ESP_LOGD(TAG, "Sending burst: 5 packets, 12 zero-bit separators, %u bits, %u timings, %ums",
             static_cast<unsigned>(burst_bits),
             static_cast<unsigned>(call.get_data()->get_data().size()),
             static_cast<unsigned>(this->last_burst_ms_));

    call.perform();
    this->last_transmission_ = now;
}

// Control method implementations
void ProFlame2Component::state_changed_() {
    ProFlame2Command &st = this->current_state_;
    st.flame_level = std::min<uint8_t>(st.flame_level, 6);
    st.fan_level = std::min<uint8_t>(st.fan_level, 6);
    st.light_level = std::min<uint8_t>(st.light_level, 6);

    this->save_state_();
    this->publish_state_();
    this->transmit_command();
}

void ProFlame2Component::save_state_() {
    SavedState saved{this->current_state_, this->override_, this->state_valid_};
    this->pref_.save(&saved);
}

ProFlame2Command ProFlame2Component::effective_state() const {
    ProFlame2Command st = this->current_state_;
    if (this->override_) {
        st.power = false;
    }
    return st;
}

void ProFlame2Component::publish_state_() {
    // Power reflects what the fireplace was actually sent, i.e. off while overridden.
    const ProFlame2Command st = this->effective_state();
    if (this->power_sensor_) this->power_sensor_->publish_state(st.power);
    if (this->pilot_sensor_) this->pilot_sensor_->publish_state(st.pilot_cpi);
    if (this->aux_sensor_) this->aux_sensor_->publish_state(st.aux_power);
    if (this->front_sensor_) this->front_sensor_->publish_state(st.front_flame);
    if (this->thermostat_sensor_) this->thermostat_sensor_->publish_state(st.thermostat);
    if (this->flame_sensor_) this->flame_sensor_->publish_state(st.flame_level);
    if (this->fan_sensor_) this->fan_sensor_->publish_state(st.fan_level);
    if (this->light_sensor_) this->light_sensor_->publish_state(st.light_level);
    if (this->override_switch_) this->override_switch_->publish_state(this->override_);
    if (this->state_valid_sensor_) this->state_valid_sensor_->publish_state(this->state_valid_);
}

void ProFlame2Component::sync_state() {
    if (!this->state_valid_) {
        ESP_LOGW(TAG, "Sync: no state received from the remote yet - nothing sent");
        return;
    }
    const ProFlame2Command st = this->effective_state();
    ESP_LOGI(TAG, "Sync: re-sending current state: Power=%d%s, Pilot=%s, Flame=%d, Fan=%d, "
                  "Light=%d, Aux=%d, Front=%d, Thermo=%d",
             st.power, this->override_ ? " (override)" : "", st.pilot_cpi ? "CPI" : "IPI",
             st.flame_level, st.fan_level, st.light_level, st.aux_power, st.front_flame,
             st.thermostat);
    this->transmit_command();
}

void ProFlame2Component::set_override(bool engaged) {
    if (!engaged && !this->override_) {
        this->publish_state_();  // nothing to release; just confirm the switch state
        return;
    }
    this->override_ = engaged;
    ESP_LOGI(TAG, "Power override %s", engaged ? "engaged - sending power off"
                                               : "released - re-sending the remote's last state");
    this->state_changed_();
}

void ProFlame2Component::set_state(const ProFlame2Command &state) {
    this->current_state_ = state;
    ESP_LOGI(TAG, "State replaced");
    this->state_changed_();
}

void ProFlame2Component::set_power(bool state) {
    this->current_state_.power = state;
    ESP_LOGI(TAG, "Power set to %s", state ? "ON" : "OFF");
    this->state_changed_();
}

void ProFlame2Component::set_pilot_mode(bool cpi_mode) {
    this->current_state_.pilot_cpi = cpi_mode;
    ESP_LOGI(TAG, "Pilot mode set to %s", cpi_mode ? "CPI" : "IPI");
    this->state_changed_();
}

void ProFlame2Component::set_flame_level(uint8_t level) {
    this->current_state_.flame_level = level;
    ESP_LOGI(TAG, "Flame level set to %d", std::min<uint8_t>(level, 6));
    this->state_changed_();
}

void ProFlame2Component::set_fan_level(uint8_t level) {
    this->current_state_.fan_level = level;
    ESP_LOGI(TAG, "Fan level set to %d", std::min<uint8_t>(level, 6));
    this->state_changed_();
}

void ProFlame2Component::set_light_level(uint8_t level) {
    this->current_state_.light_level = level;
    ESP_LOGI(TAG, "Light level set to %d", std::min<uint8_t>(level, 6));
    this->state_changed_();
}

void ProFlame2Component::set_aux_power(bool state) {
    this->current_state_.aux_power = state;
    ESP_LOGI(TAG, "Aux power set to %s", state ? "ON" : "OFF");
    this->state_changed_();
}

void ProFlame2Component::set_front_flame(bool state) {
    this->current_state_.front_flame = state;
    ESP_LOGI(TAG, "Front flame set to %s", state ? "ON" : "OFF");
    this->state_changed_();
}

void ProFlame2Component::set_thermostat(bool state) {
    this->current_state_.thermostat = state;
    ESP_LOGI(TAG, "Thermostat set to %s", state ? "ON" : "OFF");
    this->state_changed_();
}

}  // namespace proflame2
}  // namespace esphome
