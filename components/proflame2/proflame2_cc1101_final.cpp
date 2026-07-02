#include "proflame2_cc1101.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cmath>

namespace esphome {
namespace proflame2 {

static const char *TAG = "proflame2";

// CC1101 Configuration for ~315 MHz OOK at 2400 baud (26 MHz crystal).
// The frequency registers are computed from the configured frequency in
// configure_cc1101(), so they are not part of this table.
static const uint8_t CC1101_CONFIG[][2] = {
    // GDO0 used as a TX FIFO threshold indicator (assert when TX FIFO < threshold).
    {CC1101_IOCFG0,   0x02},

    // FIFOTHR: RX threshold default-ish, TX threshold 33 bytes so we get an early
    // refill warning (~110ms of airtime at 2400 baud before the FIFO empties).
    {CC1101_FIFOTHR,  0x47},
    // FIFO/packet mode with fixed length: PKTLEN is set per-transmission to the exact
    // frame length so the radio terminates TX cleanly (no async serial mode, no
    // infinite-length underflow at end of frame).
    {CC1101_PKTLEN,   125},
    {CC1101_PKTCTRL1, 0x00},  // No address check, no status append
    {CC1101_PKTCTRL0, 0x00},  // Fixed length, CRC off, whitening off, FIFO mode
    {CC1101_FSCTRL1,  0x06},  // Frequency Synthesizer Control

    // Data rate 2400 baud: DRATE_E=6, DRATE_M=0x83 -> 2399.5 baud.
    // This is the rate of the *Manchester-encoded* on-air bits: SDR capture of the
    // real remote shows ~416us per encoded bit and a 395ms burst
    // (5 x 182 bits + 4 x 12 separator bits at 2400 baud = 399ms).
    {CC1101_MDMCFG4,  0xF6},
    {CC1101_MDMCFG3,  0x83},
    {CC1101_MDMCFG2,  0x30},  // ASK/OOK, Manchester encoder OFF (we pre-encode), no preamble/sync
    {CC1101_MDMCFG1,  0x00},
    {CC1101_MDMCFG0,  0xF8},
    {CC1101_DEVIATN,  0x00},  // Not used for OOK

    // State machine: return to IDLE after TX, manual calibration (we SCAL before TX)
    {CC1101_MCSM1,    0x00},
    {CC1101_MCSM0,    0x04},

    // Front-end settings recommended for ASK/OOK
    {CC1101_FREND1,   0x56},
    // FREND0: PA_POWER=1 -> for OOK, PATABLE[0] is the '0' (off) level and
    // PATABLE[1] is the '1' (on) level.
    {CC1101_FREND0,   0x11},
    {CC1101_FSCAL3,   0xEA},
    {CC1101_FSCAL2,   0x2A},
    {CC1101_FSCAL1,   0x00},
    {CC1101_FSCAL0,   0x11},
    {CC1101_TEST2,    0x81},
    {CC1101_TEST1,    0x35},
    {CC1101_TEST0,    0x09},
};

void ProFlame2Component::setup() {
    ESP_LOGCONFIG(TAG, "Setting up ProFlame 2 CC1101...");

    this->spi_setup();

    if (this->gdo0_pin_ != nullptr) {
        this->gdo0_pin_->setup();
        this->gdo0_pin_->pin_mode(gpio::FLAG_INPUT);
    }

    // Reset and configure CC1101
    this->reset_cc1101();
    delay(10);
    this->configure_cc1101();

    // Verify PA table was written correctly
    uint8_t pa_verify[2];
    this->enable();
    this->write_byte(CC1101_PATABLE | 0xC0);  // Burst read
    pa_verify[0] = this->read_byte();
    pa_verify[1] = this->read_byte();
    this->disable();
    ESP_LOGD(TAG, "PA Table verified: OFF=0x%02X, ON=0x%02X", pa_verify[0], pa_verify[1]);

    // Initialize state
    this->current_state_ = ProFlame2Command{
        .pilot_cpi = false,
        .light_level = 0,
        .thermostat = false,
        .power = false,
        .front_flame = false,
        .fan_level = 0,
        .aux_power = false,
        .flame_level = 0
    };

    ESP_LOGCONFIG(TAG, "ProFlame 2 setup complete");
}

void ProFlame2Component::loop() {
    this->service_tx_();
    this->try_start_pending_tx_();
}

void ProFlame2Component::dump_config() {
    ESP_LOGCONFIG(TAG, "ProFlame 2 CC1101:");
    ESP_LOGCONFIG(TAG, "  Serial Number: 0x%06X", this->serial_number_ & 0xFFFFFF);
    ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz", this->frequency_mhz_);
    ESP_LOGCONFIG(TAG, "  Checksum constants: C1=0x%X D1=0x%X C2=0x%X D2=0x%X",
                  this->chk_c1_, this->chk_d1_, this->chk_c2_, this->chk_d2_);
    if (this->gdo0_pin_ != nullptr) {
        LOG_PIN("  GDO0 Pin: ", this->gdo0_pin_);
    }

    // Basic SPI sanity: these are CC1101 status registers.
    const uint8_t partnum = this->read_status_register(CC1101_PARTNUM);
    const uint8_t version = this->read_status_register(CC1101_VERSION);
    ESP_LOGCONFIG(TAG, "  CC1101 Part Number: 0x%02X", partnum);
    ESP_LOGCONFIG(TAG, "  CC1101 Version: 0x%02X", version);

    // Log key RF settings we're relying on for RTL/SDR bring-up.
    const uint8_t f2 = this->read_register(CC1101_FREQ2);
    const uint8_t f1 = this->read_register(CC1101_FREQ1);
    const uint8_t f0 = this->read_register(CC1101_FREQ0);
    const uint8_t mdm2 = this->read_register(CC1101_MDMCFG2);
    const uint8_t pkt0 = this->read_register(CC1101_PKTCTRL0);
    const uint8_t fr0 = this->read_register(CC1101_FREND0);
    ESP_LOGCONFIG(TAG, "  FREQ: 0x%02X%02X%02X", f2, f1, f0);
    ESP_LOGCONFIG(TAG, "  MDMCFG2: 0x%02X (modulation/sync)", mdm2);
    ESP_LOGCONFIG(TAG, "  PKTCTRL0: 0x%02X (packet mode)", pkt0);
    ESP_LOGCONFIG(TAG, "  FREND0: 0x%02X (PA_POWER)", fr0);
}

void ProFlame2Component::write_register(uint8_t reg, uint8_t value) {
    this->enable();
    this->write_byte(reg);
    this->write_byte(value);
    this->disable();
}

uint8_t ProFlame2Component::read_register(uint8_t reg) {
    this->enable();
    this->write_byte(reg | 0x80);  // Read flag
    uint8_t value = this->read_byte();
    this->disable();
    return value;
}

uint8_t ProFlame2Component::read_status_register(uint8_t reg) {
    // Status registers must be read with burst + read bits set (0xC0)
    this->enable();
    this->write_byte(reg | 0xC0);
    uint8_t value = this->read_byte();
    this->disable();
    return value;
}

void ProFlame2Component::send_strobe(uint8_t strobe) {
    this->enable();
    this->write_byte(strobe);
    this->disable();
}

void ProFlame2Component::reset_cc1101() {
    // CC1101 reset sequence (simplified):
    // 1) CSn high -> low -> high -> low with timing gaps
    // 2) SRES strobe while CSn is asserted
    // NOTE: The "proper" sequence includes waiting for SO(MISO) to go low.
    // In ESPHome we don't have direct access to SO level here, so we keep
    // timings generous and rely on subsequent register reads for sanity.

    this->disable();
    delayMicroseconds(5);

    this->enable();
    delayMicroseconds(10);
    this->disable();
    delayMicroseconds(40);

    this->enable();
    this->write_byte(CC1101_SRES);
    this->disable();
    delay(2);
}

void ProFlame2Component::configure_cc1101() {
    // Write configuration registers
    for (size_t i = 0; i < sizeof(CC1101_CONFIG) / sizeof(CC1101_CONFIG[0]); i++) {
        this->write_register(CC1101_CONFIG[i][0], CC1101_CONFIG[i][1]);
    }

    // Frequency: FREQ = f / (26MHz / 2^16).
    // 314.973 MHz (FCC filing / smartfire) -> 0x0C1D46. The real remote captured
    // with rtl_433 measured ~315.07 MHz; OOK receivers are wide, either works.
    const uint32_t freq_word =
        static_cast<uint32_t>(lroundf(this->frequency_mhz_ * 1e6f / (26000000.0f / 65536.0f)));
    this->write_register(CC1101_FREQ2, (freq_word >> 16) & 0xFF);
    this->write_register(CC1101_FREQ1, (freq_word >> 8) & 0xFF);
    this->write_register(CC1101_FREQ0, freq_word & 0xFF);

    // Set to IDLE state and flush FIFOs
    this->send_strobe(CC1101_SIDLE);
    this->send_strobe(CC1101_SFTX);
    this->send_strobe(CC1101_SFRX);

    // PA table for OOK: index 0 = '0' symbol (carrier OFF), index 1 = '1' symbol
    // (carrier ON, ~+10 dBm at 315MHz). FREND0.PA_POWER=1 selects index 1 for '1'.
    // Index 0 MUST stay 0x00 or the '0' symbol also transmits carrier.
    static const uint8_t PA_TABLE[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    this->enable();
    this->write_byte(CC1101_PATABLE | 0x40);  // burst write
    for (int i = 0; i < 8; i++) {
        this->write_byte(PA_TABLE[i]);
    }
    this->disable();

    // Force a calibration once at boot so we start from a known-good state.
    this->send_strobe(CC1101_SCAL);

    ESP_LOGD(TAG, "CC1101 configured for %.3f MHz OOK at 2400 baud (FREQ=0x%06X)",
             this->frequency_mhz_, static_cast<unsigned>(freq_word));
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

    // Build command bytes
    // Command 1: CPI | Light[3] | 0 0 | Thermostat | Power
    uint8_t cmd1 = (this->current_state_.pilot_cpi ? 0x80 : 0x00) |
                   ((this->current_state_.light_level & 0x07) << 4) |
                   (this->current_state_.thermostat ? 0x02 : 0x00) |
                   (this->current_state_.power ? 0x01 : 0x00);

    // Command 2: Front | Fan[3] | Aux | Flame[3]
    uint8_t cmd2 = (this->current_state_.front_flame ? 0x80 : 0x00) |
                   ((this->current_state_.fan_level & 0x07) << 4) |
                   (this->current_state_.aux_power ? 0x08 : 0x00) |
                   (this->current_state_.flame_level & 0x07);

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

    return total_bytes;
}

void ProFlame2Component::transmit_command() {
    // Always queue: the pending flag is consumed by try_start_pending_tx_() once the
    // radio is free and the rate limit has elapsed. Because the packet is built at
    // send time from current_state_, rapid changes coalesce into the latest state
    // instead of being dropped.
    this->tx_pending_ = true;
    this->try_start_pending_tx_();
}

void ProFlame2Component::try_start_pending_tx_() {
    if (!this->tx_pending_ || this->tx_state_ == TX_RUNNING) {
        return;
    }

    const uint32_t now = millis();
    if (now - this->last_transmission_ < MIN_TRANSMISSION_INTERVAL) {
        return;  // loop() retries shortly
    }
    this->tx_pending_ = false;

    ESP_LOGI(TAG, "TX state: Power=%d, Pilot=%s, Flame=%d, Fan=%d, Light=%d, Aux=%d, Front=%d, Thermo=%d",
             this->current_state_.power,
             this->current_state_.pilot_cpi ? "CPI" : "IPI",
             this->current_state_.flame_level,
             this->current_state_.fan_level,
             this->current_state_.light_level,
             this->current_state_.aux_power,
             this->current_state_.front_flame,
             this->current_state_.thermostat);

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
    uint8_t burst[220];
    const size_t burst_len = this->build_tx_burst_(encoded, 182, burst, sizeof(burst), 5, 12);
    if (burst_len == 0) {
        ESP_LOGE(TAG, "Failed to build TX burst");
        return;
    }

    ESP_LOGD(TAG, "Sending burst: 5 packets, 12 zero-bit separators, %u bytes",
             static_cast<unsigned>(burst_len));

    this->start_tx_(burst, burst_len);
    this->last_transmission_ = now;
}

void ProFlame2Component::start_tx_(const uint8_t *data, size_t len) {
    if (len == 0 || len > sizeof(this->tx_buf_) || len > 255) {
        ESP_LOGE(TAG, "TX buffer size invalid: %u", static_cast<unsigned>(len));
        return;
    }

    memcpy(this->tx_buf_, data, len);
    this->tx_len_ = len;
    this->tx_pos_ = 0;
    this->tx_start_ms_ = millis();
    this->tx_state_ = TX_RUNNING;

    // Program fixed packet length so the CC1101 terminates TX cleanly when done.
    this->write_register(CC1101_PKTLEN, static_cast<uint8_t>(len));

    // Known-good state and clean FIFO
    this->send_strobe(CC1101_SIDLE);
    this->send_strobe(CC1101_SFTX);

    // Manual calibration before TX (MCSM0 has auto-cal disabled)
    this->send_strobe(CC1101_SCAL);
    delay(5);  // Calibration takes ~720us; generous margin

    // Verify we're back in IDLE after calibration
    uint8_t marc_cal = this->read_status_register(CC1101_MARCSTATE) & 0x1F;
    if (marc_cal != 0x01) {
        ESP_LOGW(TAG, "Calibration may have failed, MARCSTATE=0x%02X", marc_cal);
        this->send_strobe(CC1101_SIDLE);
        delay(1);
    }

    // Prime the 64-byte TX FIFO as full as we can; deep priming reduces the chance
    // of an early underflow if the main loop is busy for a short burst.
    const size_t first = std::min<size_t>(64, this->tx_len_);
    this->enable();
    this->write_byte(CC1101_TXFIFO | 0x40);  // TX FIFO burst write
    for (size_t i = 0; i < first; i++) {
        this->write_byte(this->tx_buf_[i]);
    }
    this->disable();
    this->tx_pos_ = first;

    uint8_t st = this->read_status_register(CC1101_MARCSTATE) & 0x1F;
    ESP_LOGD(TAG, "TX start: primed=%u bytes, MARCSTATE=0x%02X", static_cast<unsigned>(first), st);

    // Start TX
    this->send_strobe(CC1101_STX);
}

void ProFlame2Component::service_tx_() {
    if (this->tx_state_ != TX_RUNNING) {
        return;
    }

    const uint32_t now = millis();

    const uint8_t marc = this->read_status_register(CC1101_MARCSTATE) & 0x1F;
    const uint8_t txbytes_raw = this->read_status_register(CC1101_TXBYTES);
    const bool underflow = (txbytes_raw & 0x80) != 0;
    const uint8_t txbytes = txbytes_raw & 0x7F;

    // TX FIFO underflow or hung radio -> recover.
    if (underflow || marc == 0x16) {
        ESP_LOGE(TAG, "TX error: MARCSTATE=0x%02X TXBYTES=0x%02X (underflow=%d)", marc, txbytes_raw, underflow);
        this->send_strobe(CC1101_SIDLE);
        this->send_strobe(CC1101_SFTX);
        this->tx_state_ = TX_ERROR;
        return;
    }

    // Timeout guard (a 120-byte burst is ~400ms at 2400 baud)
    if (now - this->tx_start_ms_ > 2000) {
        ESP_LOGE(TAG, "TX timeout: MARCSTATE=0x%02X TXBYTES=%u pos=%u/%u", marc, txbytes,
                 static_cast<unsigned>(this->tx_pos_), static_cast<unsigned>(this->tx_len_));
        this->send_strobe(CC1101_SIDLE);
        this->send_strobe(CC1101_SFTX);
        this->tx_state_ = TX_ERROR;
        return;
    }

    // Refill FIFO while TX is running. FIFO is 64 bytes; write whenever there is any
    // free space - tiny top-ups beat an underflow (which hard-stops the radio).
    if (this->tx_pos_ < this->tx_len_) {
        const size_t free = (txbytes >= 64) ? 0 : (64 - txbytes);
        if (free > 0) {
            const size_t remaining = this->tx_len_ - this->tx_pos_;
            const size_t chunk = std::min(free, remaining);

            this->enable();
            this->write_byte(CC1101_TXFIFO | 0x40);  // TX FIFO burst write
            for (size_t i = 0; i < chunk; i++) {
                this->write_byte(this->tx_buf_[this->tx_pos_ + i]);
            }
            this->disable();
            this->tx_pos_ += chunk;

            ESP_LOGV(TAG, "TX refill: wrote=%u free=%u txbytes=%u pos=%u/%u marc=0x%02X",
                     static_cast<unsigned>(chunk), static_cast<unsigned>(free), txbytes,
                     static_cast<unsigned>(this->tx_pos_), static_cast<unsigned>(this->tx_len_), marc);
        }
    }

    // Completion: in fixed-length mode the CC1101 returns to IDLE automatically when
    // PKTLEN bytes have been clocked out. We treat "done" as: all bytes queued by us,
    // TX FIFO drained, and MARCSTATE back in IDLE.
    if (this->tx_pos_ >= this->tx_len_ && txbytes == 0 && (marc == 0x01 || marc == 0x00)) {
        this->send_strobe(CC1101_SIDLE);
        this->send_strobe(CC1101_SFTX);  // clear any residual flags
        this->tx_state_ = TX_IDLE;
        ESP_LOGD(TAG, "TX complete");
        // If a newer state was queued while transmitting, loop() picks it up via
        // try_start_pending_tx_() once the rate limit allows.
    }
}

// Control method implementations
void ProFlame2Component::set_power(bool state) {
    if (this->current_state_.power != state) {
        this->current_state_.power = state;
        this->transmit_command();
        if (this->power_switch_) {
            this->power_switch_->publish_state(state);
        }
        ESP_LOGI(TAG, "Power set to %s", state ? "ON" : "OFF");
    }
}

void ProFlame2Component::set_pilot_mode(bool cpi_mode) {
    if (this->current_state_.pilot_cpi != cpi_mode) {
        this->current_state_.pilot_cpi = cpi_mode;
        this->transmit_command();
        if (this->pilot_switch_) {
            this->pilot_switch_->publish_state(cpi_mode);
        }
        ESP_LOGI(TAG, "Pilot mode set to %s", cpi_mode ? "CPI" : "IPI");
    }
}

void ProFlame2Component::set_flame_level(uint8_t level) {
    if (level > 6) level = 6;
    if (this->current_state_.flame_level != level) {
        this->current_state_.flame_level = level;
        this->transmit_command();
        if (this->flame_number_) {
            this->flame_number_->publish_state(level);
        }
        ESP_LOGI(TAG, "Flame level set to %d", level);
    }
}

void ProFlame2Component::set_fan_level(uint8_t level) {
    if (level > 6) level = 6;
    if (this->current_state_.fan_level != level) {
        this->current_state_.fan_level = level;
        this->transmit_command();
        if (this->fan_number_) {
            this->fan_number_->publish_state(level);
        }
        ESP_LOGI(TAG, "Fan level set to %d", level);
    }
}

void ProFlame2Component::set_light_level(uint8_t level) {
    if (level > 6) level = 6;
    if (this->current_state_.light_level != level) {
        this->current_state_.light_level = level;
        this->transmit_command();
        if (this->light_number_) {
            this->light_number_->publish_state(level);
        }
        ESP_LOGI(TAG, "Light level set to %d", level);
    }
}

void ProFlame2Component::set_aux_power(bool state) {
    if (this->current_state_.aux_power != state) {
        this->current_state_.aux_power = state;
        this->transmit_command();
        if (this->aux_switch_) {
            this->aux_switch_->publish_state(state);
        }
        ESP_LOGI(TAG, "Aux power set to %s", state ? "ON" : "OFF");
    }
}

void ProFlame2Component::set_front_flame(bool state) {
    if (this->current_state_.front_flame != state) {
        this->current_state_.front_flame = state;
        this->transmit_command();
        if (this->front_switch_) {
            this->front_switch_->publish_state(state);
        }
        ESP_LOGI(TAG, "Front flame set to %s", state ? "ON" : "OFF");
    }
}

void ProFlame2Component::set_thermostat(bool state) {
    if (this->current_state_.thermostat != state) {
        this->current_state_.thermostat = state;
        this->transmit_command();
        if (this->thermostat_switch_) {
            this->thermostat_switch_->publish_state(state);
        }
        ESP_LOGI(TAG, "Thermostat set to %s", state ? "ON" : "OFF");
    }
}

// DEBUG FUNCTIONS
void ProFlame2Component::debug_minimal_tx() {
    ESP_LOGI(TAG, "=== DEBUG: Sending minimal 23-byte test ===");

    if (this->tx_state_ == TX_RUNNING) {
        ESP_LOGW(TAG, "TX busy, try again");
        return;
    }

    // Alternating pattern that's easy to spot on an SDR
    uint8_t test_data[23];
    for (int i = 0; i < 23; i++) {
        test_data[i] = (i % 2) ? 0x55 : 0xAA;
    }

    this->start_tx_(test_data, 23);
    ESP_LOGI(TAG, "Sent 23 bytes = 184 bits; rtl_433 should show {184} bits");
}

void ProFlame2Component::debug_check_config() {
    ESP_LOGI(TAG, "=== CC1101 Configuration Check ===");

    uint8_t freq2 = this->read_register(CC1101_FREQ2);
    uint8_t freq1 = this->read_register(CC1101_FREQ1);
    uint8_t freq0 = this->read_register(CC1101_FREQ0);
    uint8_t mdm4 = this->read_register(CC1101_MDMCFG4);
    uint8_t mdm3 = this->read_register(CC1101_MDMCFG3);
    uint8_t frend0 = this->read_register(CC1101_FREND0);
    uint8_t pktlen = this->read_register(CC1101_PKTLEN);

    ESP_LOGI(TAG, "MDMCFG4: 0x%02X (should be 0xF6 for 2400 baud)", mdm4);
    ESP_LOGI(TAG, "MDMCFG3: 0x%02X (should be 0x83 for 2400 baud)", mdm3);
    ESP_LOGI(TAG, "FREND0: 0x%02X (should be 0x11)", frend0);
    ESP_LOGI(TAG, "PKTLEN: 0x%02X", pktlen);

    // Check PA table
    uint8_t pa_table[2];
    this->enable();
    this->write_byte(CC1101_PATABLE | 0xC0);
    pa_table[0] = this->read_byte();
    pa_table[1] = this->read_byte();
    this->disable();
    ESP_LOGI(TAG, "PA Table: OFF=0x%02X (should be 0x00), ON=0x%02X (should be 0xC0)",
             pa_table[0], pa_table[1]);

    // Calculate actual frequency
    uint32_t freq_reg = (static_cast<uint32_t>(freq2) << 16) |
                        (static_cast<uint32_t>(freq1) << 8) |
                        freq0;
    float frequency = (freq_reg * 26.0f) / 65536.0f;
    ESP_LOGI(TAG, "FREQ: 0x%06X = %.3f MHz (configured %.3f MHz)",
             static_cast<unsigned>(freq_reg), frequency, this->frequency_mhz_);

    if (mdm4 != 0xF6 || mdm3 != 0x83) {
        ESP_LOGE(TAG, "CRITICAL: Data rate is wrong! Must be 2400 baud (0xF6/0x83)");
    }
    if (pa_table[0] != 0x00) {
        ESP_LOGE(TAG, "CRITICAL: PATABLE[0] must be 0x00 for OOK");
    }
}

}  // namespace proflame2
}  // namespace esphome
