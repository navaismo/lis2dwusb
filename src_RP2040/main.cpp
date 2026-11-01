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

uint64_t nowUs() {
    const uint32_t tick = micros();
    clock_us += uint32_t(tick - last_tick);
    last_tick = tick;
    return clock_us;
}
void writeReg(uint8_t address, uint8_t value) {
    digitalWrite(CS_PIN, LOW);
    spi.write(address & 0x7F);
    spi.write(value);
    digitalWrite(CS_PIN, HIGH);
}
uint8_t readReg(uint8_t address) {
    digitalWrite(CS_PIN, LOW);
    spi.write(address | 0x80);
    const uint8_t value = spi.write(0);
    digitalWrite(CS_PIN, HIGH);
    return value;
}
void readAxes(int16_t *axes) {
    digitalWrite(CS_PIN, LOW);
    spi.write(OUT_X_L | 0x80);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const uint8_t low = spi.write(0), high = spi.write(0);
        axes[axis] = int16_t(uint16_t(low) | (uint16_t(high) << 8));
    }
    digitalWrite(CS_PIN, HIGH);
}
void powerDown() {
    active = false;
    writeReg(CTRL1, 0);
    writeReg(FIFO_CTRL, 0);
}
void fail(const char *reason) {
    powerDown();
    finishing = false;
    queued = head = tail = 0;
    snprintf(control, sizeof(control), "ERROR,%s,%s", token, reason);
}
bool configure(unsigned requested) {
    if (readReg(WHO_AM_I) != 0x44) return false;
    writeReg(CTRL2, 0x40);
    const uint64_t deadline = nowUs() + 100000;
    while (readReg(CTRL2) & 0x40) {
        if (nowUs() >= deadline) return false;
        delay(1);
    }
    // ST DS11811: BDU + address increment, high-performance mode, +/-2 g.
    writeReg(CTRL2, 0x0C);
    writeReg(CTRL6, 0x00);
    const uint8_t odr = requested == 1600 ? 0x94 : requested == 800 ? 0x84 : requested == 400 ? 0x74 : 0x64;
    writeReg(CTRL1, odr);
    if (readReg(CTRL1) != odr || readReg(CTRL2) != 0x0C || readReg(CTRL6) != 0) return false;
    delay(30);
    writeReg(FIFO_CTRL, 0);
    writeReg(FIFO_CTRL, 0xC0);
    if (readReg(FIFO_CTRL) != 0xC0) return false;
    discard = 2;  // Covers ODR stabilization and the FIFO-switch discard.
    return true;
}
void command(char *line) {
    if (!lis_unframe(line)) { fail("BAD_COMMAND_CRC"); return; }
    char *save = nullptr;
    char *verb = strtok_r(line, ",", &save);
    char *argument = strtok_r(nullptr, ",", &save);
    char *third = strtok_r(nullptr, ",", &save);
    if (verb && !strcmp(verb, "HELLO") && argument && lis_token_valid(argument) && !third) {
        powerDown();
        queued = head = tail = 0;
        finishing = false;
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
        if (!configure(unsigned(requested))) { fail("SENSOR_CONFIG"); return; }
        rate = unsigned(requested);
        sequence = 0;
        epoch = last_empty = last_sample = last_tx = nowUs();
        active = true;
        snprintf(control, sizeof(control), "READY,2,%s,%u,16384,POLL", token, unsigned(rate));
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
    for (unsigned i = 0; i < 32 && _SerialUSB.available(); ++i) {
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
    if (window > 1000000UL / rate / 4) { fail("TIMING_UNCERTAIN"); return; }
    if (queued == QUEUE_SIZE || sequence == UINT32_MAX) { fail("BUFFER_OVERFLOW"); return; }
    for (unsigned axis = 0; axis < 3; ++axis)
        if (axes[axis] >= 32760 || axes[axis] <= -32760) { fail("CLIPPING"); return; }
    Sample &sample = samples[head];
    sample.sequence = sequence++;
    sample.time = last_empty + window / 2 - epoch;
    memcpy(sample.axes, axes, sizeof(axes));
    sample.window = uint32_t(window);
    head = (head + 1) % QUEUE_SIZE;
    ++queued;
}
void transmit() {
    if (pending_offset == pending_size) {
        pending_offset = pending_size = 0;
        char payload[LIS_LINE_SIZE] = {};
        if (control[0]) {
            strcpy(payload, control);
            control[0] = 0;
        } else if (queued) {
            const Sample &s = samples[tail];
            snprintf(payload, sizeof(payload), "S,%" PRIu32 ",%" PRIu64 ",%d,%d,%d,%" PRIu32,
                     s.sequence, s.time, int(s.axes[0]), int(s.axes[1]), int(s.axes[2]), s.window);
            tail = (tail + 1) % QUEUE_SIZE;
            --queued;
        } else if (finishing) {
            snprintf(payload, sizeof(payload), "DONE,%s,%" PRIu32, token, sequence);
            finishing = false;
        }
        if (!payload[0]) return;
        const int size = lis_frame(pending, sizeof(pending), payload);
        if (size < 0) { fail("FRAME_SIZE"); return; }
        pending_size = size_t(size);
    }
    uint32_t sent = 0;
    // Ordinary Serial.write blocks; the pinned mbed CDC API does not.
    _SerialUSB.send_nb(reinterpret_cast<uint8_t *>(pending + pending_offset),
                   uint32_t(pending_size - pending_offset), &sent);
    pending_offset += sent;
    if (sent) last_tx = nowUs();
    if (active && nowUs() - last_tx > 2000000) fail("USB_TIMEOUT");
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
        control[0] = 0;
        return;
    }
    acquire();
    receiveCommands();
    transmit();
}
