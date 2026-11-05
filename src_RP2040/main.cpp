#include <Arduino.h>
#include "mbed.h"
#include <USB/PluggableUSBSerial.h>
#include <inttypes.h>
#include "lis2dw_protocol.h"

#ifndef CS_PIN
#define CS_PIN 9
#endif
#ifndef SCK_PIN
#define SCK_PIN 10
#endif
#ifndef MOSI_PIN
#define MOSI_PIN 11
#endif
#ifndef MISO_PIN
#define MISO_PIN 8
#endif

namespace {
constexpr uint8_t WHO_AM_I = 0x0F, CTRL1 = 0x20, CTRL2 = 0x21, CTRL6 = 0x25;
constexpr uint8_t OUT_X_L = 0x28, FIFO_CTRL = 0x2E, FIFO_SAMPLES = 0x2F;
uint8_t configured_ctrl6 = 0;
uint8_t verified_ctrl6 = 0;
constexpr size_t QUEUE_SIZE = 256;
mbed::SPI spi(digitalPinToPinName(MOSI_PIN), digitalPinToPinName(MISO_PIN), digitalPinToPinName(SCK_PIN));
struct Sample {
    uint32_t sequence;
    uint64_t time;
    int16_t axes[3];
    uint32_t window;
};
Sample samples[QUEUE_SIZE];
size_t head = 0, tail = 0, queued = 0;
uint32_t sequence = 0, rate = 0, last_tick = 0;
uint64_t clock_us = 0, epoch = 0, last_empty = 0, last_sample = 0, last_tx = 0;
bool active = false, finishing = false;
unsigned discard = 0;
char token[9] = "00000000";
char input[LIS_LINE_SIZE], pending[LIS_LINE_SIZE], control[LIS_LINE_SIZE];
size_t input_size = 0, pending_size = 0, pending_offset = 0;
enum class TxState { IDLE, FIELDS, CHECKSUM, SEND };
TxState tx_state = TxState::IDLE;
Sample transmitting;
size_t payload_size = 0, checksum_offset = 0;
unsigned tx_field = 0;
uint16_t checksum = 0xFFFF;
const char *last_work = "START";
constexpr size_t USB_PACKET_SIZE = 64;
constexpr uint64_t USB_FLUSH_US = 2000;
constexpr size_t TX_BUFFER_SIZE = 512;
uint8_t tx_bytes[TX_BUFFER_SIZE];
size_t tx_head = 0, tx_tail = 0, tx_queued = 0, tx_flush = 0;
uint8_t usb_packet[USB_PACKET_SIZE];
size_t usb_size = 0, usb_offset = 0;
uint64_t usb_started = 0, usb_retry = 0;
bool pending_control = false;
uint32_t formatted_samples = 0, usb_writes = 0, usb_busy = 0;

uint64_t nowUs() {
    const uint32_t tick = micros();
    clock_us += uint32_t(tick - last_tick);
    last_tick = tick;
    return clock_us;
}
size_t decimalTime(char *output, uint64_t value) {
    // The pinned Newlib Nano printf cannot format 64-bit integers.
    char reversed[20];
    size_t count = 0;
    while (value > UINT32_MAX) {
        reversed[count++] = char('0' + value % 10);
        value /= 10;
    }
    uint32_t small = uint32_t(value);
    do {
        reversed[count++] = char('0' + small % 10);
        small /= 10;
    } while (small);
    for (size_t i = 0; i < count; ++i) output[i] = reversed[count - i - 1];
    output[count] = 0;
    return count;
}
void writeReg(uint8_t address, uint8_t value) {
    digitalWrite(CS_PIN, LOW);
    spi.write(address & 0x7F);
    spi.write(value);
    digitalWrite(CS_PIN, HIGH);
}
uint8_t readReg(uint8_t address) {
    const char tx[2] = {char(address | 0x80), 0};
    char rx[2] = {};
    digitalWrite(CS_PIN, LOW);
    spi.write(tx, sizeof(tx), rx, sizeof(rx));
    digitalWrite(CS_PIN, HIGH);
    return uint8_t(rx[1]);
}
bool readConfigCtrl6(uint8_t &value) {
    const char tx[2] = {char(CTRL6 | 0x80), 0};
    char rx[2] = {};
    digitalWrite(CS_PIN, LOW);
    const int transferred = spi.write(tx, sizeof(tx), rx, sizeof(rx));
    digitalWrite(CS_PIN, HIGH);
    if (transferred != int(sizeof(tx))) return false;
    value = uint8_t(rx[1]);
    return true;
}
void readAxes(int16_t *axes) {
    const char tx[7] = {char(OUT_X_L | 0x80), 0, 0, 0, 0, 0, 0};
    char rx[7] = {};
    digitalWrite(CS_PIN, LOW);
    spi.write(tx, sizeof(tx), rx, sizeof(rx));
    digitalWrite(CS_PIN, HIGH);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const uint8_t low = uint8_t(rx[1 + 2 * axis]), high = uint8_t(rx[2 + 2 * axis]);
        axes[axis] = int16_t(uint16_t(low) | (uint16_t(high) << 8));
    }
}
void powerDown() {
    active = false;
    writeReg(CTRL1, 0);
    writeReg(FIFO_CTRL, 0);
}
void discardUnsent() {
    // Finish any partly transmitted line before emitting a control response.
    if (tx_state != TxState::SEND || !pending_offset) {
        tx_state = TxState::IDLE;
        pending_size = pending_offset = 0;
    }
}
void fail(const char *reason) {
    powerDown();
    finishing = false;
    queued = head = tail = 0;
    discardUnsent();
    snprintf(control, sizeof(control), "ERROR,%s,%s", token, reason);
}
// Keep diagnostic formatting and its stack frame off the successful acquisition path.
__attribute__((noinline)) void failClipping(const int16_t *axes, uint8_t fifo_status, uint64_t timestamp) {
    // Preserve the original candidate before the failure-only register read.
    const uint32_t candidate = sequence;
    const int x = axes[0], y = axes[1], z = axes[2];
    unsigned hit = 0;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (axes[axis] >= 32760 || axes[axis] <= -32760) hit |= 1U << axis;
    const uint8_t startup_ctrl6 = verified_ctrl6;
    const uint8_t fault_ctrl6 = readReg(CTRL6);
    char time[21];
    decimalTime(time, timestamp);
    // Include the largest field values, CRC, optional CR and NUL in the bound.
    constexpr size_t reason_size = sizeof("CLIPPING:why=raw_limit:seq=4294967295:t_us=18446744073709551615"
        ":x=-32768:y=-32768:z=-32768:hit=7:fifo=FF:fs=16:c6=FF:c6v=FF:c6f=FF");
    static_assert(sizeof("ERROR,ffffffff,") - 1 + reason_size + sizeof("*FFFF\r\n") - 1 <= LIS_LINE_SIZE,
                  "Clipping diagnostic must fit a complete protocol frame");
    char reason[reason_size];
    const int size = snprintf(reason, sizeof(reason), "CLIPPING:why=raw_limit:seq=%" PRIu32
        ":t_us=%s:x=%d:y=%d:z=%d:hit=%u:fifo=%02X:fs=%u:c6=%02X:c6v=%02X:c6f=%02X",
        candidate, time, x, y, z, hit, unsigned(fifo_status),
        2U << ((configured_ctrl6 >> 4) & 3), unsigned(configured_ctrl6),
        unsigned(startup_ctrl6), unsigned(fault_ctrl6));
    fail(size >= 0 && size_t(size) < sizeof(reason) ? reason : "CLIPPING:why=diagnostic_format");
}
bool configure(unsigned requested, unsigned range = 2) {
    if (!lis_range_valid(range)) return false;
    if (readReg(WHO_AM_I) != 0x44) return false;
    writeReg(CTRL2, 0x40);
    const uint64_t deadline = nowUs() + 100000;
    while (readReg(CTRL2) & 0x40) {
        if (nowUs() >= deadline) return false;
        delay(1);
    }
    // ST DS11811: FS is CTRL6[5:4]; keep BW_FILT, FDS and LOW_NOISE unchanged.
    writeReg(CTRL2, 0x0C);
    configured_ctrl6 = range == 16 ? 0x30 : range == 8 ? 0x20 : range == 4 ? 0x10 : 0x00;
    writeReg(CTRL6, configured_ctrl6);
    const uint8_t odr = requested == 1600 ? 0x94 : requested == 800 ? 0x84 : requested == 400 ? 0x74 : 0x64;
    writeReg(CTRL1, odr);
    if (readReg(CTRL1) != odr || readReg(CTRL2) != 0x0C) return false;
    uint8_t ctrl6;
    if (!readConfigCtrl6(ctrl6) || ctrl6 != configured_ctrl6) return false;
    verified_ctrl6 = ctrl6;
    delay(30);
    writeReg(FIFO_CTRL, 0);
    writeReg(FIFO_CTRL, 0xC0);
    if (readReg(FIFO_CTRL) != 0xC0) return false;
    discard = 2;  // Covers ODR stabilization and the FIFO-switch discard.
    return true;
}
void command(char *line) {
    if (!lis_unframe(line)) { fail("BAD_COMMAND_CRC"); return; }
    if (!line[0] || line[strlen(line) - 1] == ',' || strstr(line, ",,")) {
        fail("BAD_COMMAND"); return;
    }
    char *save = nullptr;
    char *verb = strtok_r(line, ",", &save);
    char *argument = strtok_r(nullptr, ",", &save);
    char *third = strtok_r(nullptr, ",", &save);
    char *fourth = strtok_r(nullptr, ",", &save);
    if (verb && !strcmp(verb, "HELLO") && argument && lis_token_valid(argument) && !third) {
        powerDown();
        queued = head = tail = 0;
        finishing = false;
        discardUnsent();
        strcpy(token, argument);
        snprintf(control, sizeof(control), "LIS2DWUSB,2,%s", token);
        return;
    }
    if (verb && !strcmp(verb, "START") && argument && third && !strcmp(third, token)
            && !strtok_r(nullptr, ",", &save) && !active && !finishing && !queued && !control[0]) {
        char *end = nullptr;
        const unsigned long requested = strtoul(argument, &end, 10);
        if (!argument[0] || *end || requested > 1600 || !lis_rate_valid(unsigned(requested))) {
            fail("UNSUPPORTED_RATE"); return;
        }
        unsigned range = 2;
        if (fourth) {
            const unsigned long selected = strtoul(fourth, &end, 10);
            bool digits = fourth[0] != 0;
            for (const char *p = fourth; *p; ++p) if (*p < '0' || *p > '9') digits = false;
            if (!digits || *end || selected > 16 || !lis_range_valid(unsigned(selected))) {
                fail("UNSUPPORTED_RANGE"); return;
            }
            range = unsigned(selected);
        }
        if (!configure(unsigned(requested), range)) { fail("SENSOR_CONFIG"); return; }
        rate = unsigned(requested);
        sequence = 0;
        formatted_samples = usb_writes = usb_busy = 0;
        epoch = last_empty = last_sample = last_tx = nowUs();
        active = true;
        const unsigned actual_range = 2U << ((verified_ctrl6 >> 4) & 3);
        if (fourth)
            snprintf(control, sizeof(control), "READY,2,%s,%u,%u,POLL,%u", token,
                     unsigned(rate), lis_counts_per_g(actual_range), actual_range);
        else
            snprintf(control, sizeof(control), "READY,2,%s,%u,%u,POLL", token,
                     unsigned(rate), lis_counts_per_g(actual_range));
        return;
    }
    if (verb && !strcmp(verb, "STOP") && argument && !strcmp(argument, token) && !third && active) {
        powerDown();
        finishing = true;
        return;
    }
    fail("BAD_COMMAND");
}
void receiveCommands() {
    // Bound command work so a noisy host cannot starve acquisition.
    const unsigned budget = active ? 4 : 32;
    for (unsigned i = 0; i < budget && _SerialUSB.available(); ++i) {
        last_work = "COMMAND";
        const int value = _SerialUSB.read();
        if (value == '\r') continue;
        if (value == '\n') {
            input[input_size] = 0;
            command(input);
            input_size = 0;
        } else if (value < 32 || value > 126 || input_size == sizeof(input) - 1) {
            input_size = 0;
            fail("BAD_COMMAND_LENGTH");
        } else input[input_size++] = char(value);
    }
}
void acquire() {
    if (!active) return;
    const uint64_t before = nowUs();
    const uint8_t status = readReg(FIFO_SAMPLES);
    const uint64_t observed = nowUs();
    if (status & 0x40) { fail("FIFO_OVERFLOW"); return; }
    const unsigned count = status & 0x3F;
    if (!count) {
        last_empty = before;
        if (observed - last_sample > 500000) fail("SENSOR_TIMEOUT");
        return;
    }
    // A backlog has no individually observed edges: never invent its times.
    if (count != 1) { fail("TIMING_BACKLOG"); return; }
    int16_t axes[3];
    readAxes(axes);
    last_sample = observed;
    if (discard) { --discard; last_empty = nowUs(); return; }
    const uint64_t window = observed - last_empty;
    const unsigned limit = 1000000UL / rate / 4;
    if (window > limit) {
        char width[21], reason[112];
        decimalTime(width, window);
        snprintf(reason, sizeof(reason), "TIMING_UNCERTAIN:window_us=%s:limit_us=%u:work=%s",
                 width, limit, last_work);
        fail(reason);
        return;
    }
    if (queued == QUEUE_SIZE || sequence == UINT32_MAX) {
        char reason[112];
        snprintf(reason, sizeof(reason), "BUFFER_OVERFLOW:acq=%" PRIu32 ":fmt=%" PRIu32
                 ":usb_ok=%" PRIu32 ":usb_busy=%" PRIu32,
                 sequence, formatted_samples, usb_writes, usb_busy);
        fail(reason);
        return;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        if (axes[axis] >= 32760 || axes[axis] <= -32760) {
            failClipping(axes, status, last_empty + window / 2 - epoch);
            return;
        }
    Sample &sample = samples[head];
    sample.sequence = sequence++;
    sample.time = last_empty + window / 2 - epoch;
    memcpy(sample.axes, axes, sizeof(axes));
    sample.window = uint32_t(window);
    head = (head + 1) % QUEUE_SIZE;
    ++queued;
}
void transmit() {
    if (active && nowUs() - last_tx > 2000000) { fail("USB_TIMEOUT"); return; }
    if (tx_state == TxState::IDLE) {
        pending_offset = pending_size = 0;
        if (control[0]) {
            pending_control = true;
            strcpy(pending, control);
            control[0] = 0;
        } else if (queued) {
            pending_control = false;
            transmitting = samples[tail];
            tail = (tail + 1) % QUEUE_SIZE;
            --queued;
            pending[0] = 'S';
            payload_size = 1;
            tx_field = 0;
            tx_state = TxState::FIELDS;
            return;
        } else if (finishing) {
            pending_control = true;
            snprintf(pending, sizeof(pending), "DONE,%s,%" PRIu32, token, sequence);
            finishing = false;
        } else return;
        payload_size = strlen(pending);
        checksum_offset = 0;
        checksum = 0xFFFF;
        tx_state = TxState::CHECKSUM;
        return;
    }
    if (tx_state == TxState::FIELDS) {
        last_work = "FORMAT";
        uint64_t value;
        bool negative = false;
        if (tx_field == 0) value = transmitting.sequence;
        else if (tx_field == 1) value = transmitting.time;
        else if (tx_field < 5) {
            const int axis = transmitting.axes[tx_field - 2];
            negative = axis < 0;
            value = unsigned(negative ? -axis : axis);
        } else value = transmitting.window;
        // Reserve the largest uint64 field and suffix before formatting directly
        // into the frame. Avoid a temporary field, strlen and a second copy.
        if (payload_size + 1 + size_t(negative) + 20 + 6 > sizeof(pending)) {
            fail("FRAME_SIZE"); return;
        }
        pending[payload_size++] = ',';
        if (negative) pending[payload_size++] = '-';
        payload_size += decimalTime(pending + payload_size, value);
        if (++tx_field == 6) {
            checksum_offset = 0;
            checksum = 0xFFFF;
            tx_state = TxState::CHECKSUM;
        }
        return;
    }
    if (tx_state == TxState::CHECKSUM) {
        last_work = "CRC";
        const size_t remaining = payload_size - checksum_offset;
        const size_t count = remaining < 32 ? remaining : 32;
        checksum = lis_crc16_update(checksum, pending + checksum_offset, count);
        checksum_offset += count;
        if (checksum_offset == payload_size) {
            static const char hex[] = "0123456789ABCDEF";
            pending[payload_size] = '*';
            for (unsigned i = 0; i < 4; ++i)
                pending[payload_size + 1 + i] = hex[(checksum >> (12 - 4 * i)) & 15];
            pending[payload_size + 5] = '\n';
            pending_size = payload_size + 6;
            tx_state = TxState::SEND;
        }
        return;
    }
    last_work = "PACKET";
    if (pending_control && !pending_offset && (tx_queued + pending_size) % USB_PACKET_SIZE == 0) {
        // A control must finish in a short packet so the host can deliver its tail.
        if (pending_size == sizeof(pending)) { fail("FRAME_SIZE"); return; }
        pending[pending_size - 1] = '\r';
        pending[pending_size++] = '\n';
    }
    const size_t remaining = pending_size - pending_offset;
    const size_t free = TX_BUFFER_SIZE - tx_queued;
    const size_t contiguous = TX_BUFFER_SIZE - tx_head;
    const size_t available = free < contiguous ? free : contiguous;
    const size_t count = remaining < available ? remaining : available;
    if (!count) return;
    if (!tx_queued) usb_started = nowUs();
    memcpy(tx_bytes + tx_head, pending + pending_offset, count);
    tx_head = (tx_head + count) % TX_BUFFER_SIZE;
    tx_queued += count;
    pending_offset += count;
    if (pending_offset == pending_size) {
        if (pending_control) tx_flush = tx_queued;
        else ++formatted_samples;
        tx_state = TxState::IDLE;
    }
}
void serviceUSB() {
    // Keep serialization independent of endpoint availability. A bounded byte
    // queue allows the next packets to be prepared while the current one waits.
    if (!usb_size) {
        if (!tx_queued || (tx_queued < USB_PACKET_SIZE && !tx_flush &&
                nowUs() - usb_started < USB_FLUSH_US)) return;
        size_t count = tx_queued < USB_PACKET_SIZE ? tx_queued : USB_PACKET_SIZE;
        if (tx_flush && tx_flush < count) count = tx_flush;
        const size_t contiguous = TX_BUFFER_SIZE - tx_tail;
        const size_t first = count < contiguous ? count : contiguous;
        last_work = "PACKET";
        memcpy(usb_packet, tx_bytes + tx_tail, first);
        if (first < count) memcpy(usb_packet + first, tx_bytes, count - first);
        tx_tail = (tx_tail + count) % TX_BUFFER_SIZE;
        tx_queued -= count;
        if (tx_flush) tx_flush -= count;
        usb_size = count;
        usb_offset = 0;
        usb_started = nowUs();
        return;
    }
    if (nowUs() < usb_retry) return;
    last_work = "USB";
    uint32_t sent = 0;
    _SerialUSB.send_nb(usb_packet + usb_offset, uint32_t(usb_size - usb_offset), &sent);
    usb_offset += sent;
    if (sent) {
        last_tx = nowUs();
        if (usb_writes != UINT32_MAX) ++usb_writes;
    } else {
        if (usb_busy != UINT32_MAX) ++usb_busy;
        usb_retry = nowUs() + 50;
    }
    if (usb_offset == usb_size) usb_size = usb_offset = 0;
}
}  // namespace

void setup() {
    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);
    spi.frequency(5000000);
    spi.format(8, 0);
    _SerialUSB.begin(2000000);
    last_tick = micros();
}
void loop() {
    nowUs();
    if (!_SerialUSB.connected()) {
        if (active || finishing) powerDown();
        finishing = false;
        queued = head = tail = 0;
        pending_size = pending_offset = input_size = 0;
        tx_state = TxState::IDLE;
        usb_size = usb_offset = 0;
        tx_head = tx_tail = tx_queued = tx_flush = 0;
        usb_retry = 0;
        control[0] = 0;
        return;
    }
    acquire();
    receiveCommands();
    // Amortize command/loop overhead while polling between every bounded TX step.
    for (unsigned i = 0; i < 8; ++i) {
        acquire();
        transmit();
        if (i % 2 == 1) {
            acquire();
            serviceUSB();
        }
    }
}
