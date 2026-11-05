#include "Arduino.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <type_traits>
#include "lis2dw_protocol.h"

uint64_t fake_time = 100;
uint8_t registers[64] = {};
unsigned transaction_address = 0, transaction_offset = 0, fifo_count = 0;
bool reading = false, fifo_overflow = false, produce = true, reset_stuck = false;
int raw_axis = 4096;
int raw_y = 0, raw_z = 16384;
unsigned ctrl6_reads = 0, axes_reads = 0;
uint8_t fifo_flags = 0;
bool mutate_on_ctrl6_read = false;
int ctrl6_readback_override = -1, ctrl6_transfer_override = -1;
uint64_t next_sample = 0;
unsigned sensor_period = 625;
unsigned spi_call_cost_us = 0;
MockUSB _SerialUSB;
bool synthetic_workload = false;

template<typename... Args>
int nanoSnprintf(char *output, size_t capacity, const char *format, Args... args) {
    // Host libc accepts wide integers that the target's Newlib Nano rejects.
    static_assert(((!std::is_integral<Args>::value || sizeof(Args) <= sizeof(uint32_t)) && ...),
                  "Newlib Nano does not support 64-bit printf arguments");
    if (synthetic_workload) fake_time += sizeof...(Args) * 30;
    return std::snprintf(output, capacity, format, args...);
}
void *timedMemcpy(void *output, const void *input, size_t size) {
    if (synthetic_workload) fake_time += 30;
    return std::memcpy(output, input, size);
}
uint16_t timedCrc(uint16_t crc, const char *data, size_t size) {
    if (synthetic_workload) fake_time += 24;
    return lis_crc16_update(crc, data, size);
}
#define snprintf nanoSnprintf
#define memcpy timedMemcpy
#define lis_crc16_update timedCrc
#include "../src_RP2040/main.cpp"
#undef snprintf
#undef memcpy
#undef lis_crc16_update

void runFor(uint64_t duration) {
    uint64_t end = fake_time + duration;
    while (fake_time < end) {
        loop();
        fake_time += 20;
    }
}
void drain() {
    const uint64_t deadline = fake_time + 2000000;
    while ((queued || finishing || control[0] || tx_state != TxState::IDLE || usb_size || tx_queued) && fake_time < deadline) {
        transmit();
        serviceUSB();
        fake_time += 20;
    }
    assert(!queued && !finishing && !control[0] && tx_state == TxState::IDLE && !usb_size && !tx_queued);
}
void feed(const char *payload) {
    char encoded[LIS_LINE_SIZE];
    assert(lis_frame(encoded, sizeof(encoded), payload) > 0);
    for (char *p = encoded; *p; ++p) _SerialUSB.input.push_back(*p);
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    const std::string scenario = argv[1];
    if (scenario == "wrap") fake_time = UINT32_MAX - 1000000ULL;
    registers[0x0f] = scenario == "identity" ? 0 : 0x44;
    if (scenario == "reset") reset_stuck = true;
    setup();
    assert(lis_crc16("123456789", 9) == 0x29b1);
    if (scenario == "range_session") {
        assert(argc >= 4);
        const unsigned requested_range = unsigned(std::strtoul(argv[3], nullptr, 10));
        const std::string fault = argc > 4 ? argv[4] : "normal";
        feed("HELLO,1234abcd"); runFor(1000);
        if (fault == "readback") ctrl6_readback_override = 0xFF;
        if (fault == "filter_readback") ctrl6_readback_override = 0x34;
        if (fault == "short_read") ctrl6_transfer_override = 1;
        if (fault == "failed_read") ctrl6_transfer_override = 0;
        if (lis_range_valid(requested_range)) {
            raw_axis = int(lis_counts_per_g(requested_range) / 4);
            raw_z = int(lis_counts_per_g(requested_range));
        }
        char start[128];
        snprintf(start, sizeof(start), "START,800,1234abcd,%s", argv[3]);
        feed(start); runFor(50000);
        if (active) {
            assert(fault == "normal" || fault == "clipping" || fault == "legacy_reset");
            assert(registers[CTRL1] == 0x84 && registers[CTRL2] == 0x0C && registers[FIFO_CTRL] == 0xC0);
            const unsigned ctrl6 = requested_range == 16 ? 0x30 : requested_range == 8 ? 0x20 : requested_range == 4 ? 0x10 : 0;
            assert(registers[CTRL6] == ctrl6 && verified_ctrl6 == ctrl6 && configured_ctrl6 == ctrl6);
            if (fault == "clipping") {
                raw_axis = 32764; raw_y = -32768; raw_z = 32760;
                runFor(3000);
                assert(!active);
            } else {
                runFor(15000000);
                feed("STOP,1234abcd"); runFor(20000);
                assert(sequence >= 800 * 15 && !active);
            }
            if (fault == "legacy_reset") {
                drain();
                feed("HELLO,1234abcd"); runFor(1000);
                raw_axis = 4096; raw_y = 0; raw_z = 16384;
                feed("START,800,1234abcd"); runFor(50000);
                assert(active && configured_ctrl6 == 0 && verified_ctrl6 == 0 && registers[CTRL6] == 0);
                feed("STOP,1234abcd"); runFor(20000);
            }
        }
        assert(!active && registers[CTRL1] == 0 && registers[FIFO_CTRL] == 0);
        drain();
        std::cout << _SerialUSB.output;
        return 0;
    }
    if (scenario == "clipping_format" || scenario == "clipping_candidate") {
        assert(argc >= 6);
        const int16_t axes[] = {int16_t(std::stoi(argv[3])), int16_t(std::stoi(argv[4])),
                                int16_t(std::stoi(argv[5]))};
        strcpy(token, "1234abcd");
        const unsigned range = scenario == "clipping_format" && argc > 7 ? unsigned(std::stoul(argv[7])) : 2;
        assert(configure(800, range));
        assert(ctrl6_reads == 1 && verified_ctrl6 == configured_ctrl6 && discard == 2);
        registers[CTRL6] = 0x30;
        mutate_on_ctrl6_read = true;
        bool diagnostic_expected = true;
        if (scenario == "clipping_format") {
            sequence = UINT32_MAX - 1;
            registers[CTRL6] = 0xFF;
            failClipping(axes, 0xFF, UINT64_MAX);
            assert(axes_reads == 0);
            const size_t padding = argc > 6 ? size_t(std::stoul(argv[6])) : 0;
            assert(padding < USB_PACKET_SIZE);
            memset(tx_bytes, '\r', padding);
            tx_head = tx_queued = padding;
            tx_tail = 0;
            _SerialUSB.write_limit = 5;
        } else {
            raw_axis = axes[0]; raw_y = axes[1]; raw_z = axes[2];
            sequence = argc > 6 ? unsigned(std::stoul(argv[6])) : 339;
            const uint32_t candidate = sequence;
            rate = 800; active = true; discard = 0; produce = false;
            fifo_count = 1; fifo_flags = 0x80;
            epoch = nowUs() - 100; last_empty = nowUs() - 10;
            const uint64_t expected_time = 96;
            acquire();
            assert(axes_reads == 1);
            const bool clipped = axes[0] >= 32760 || axes[0] <= -32760
                || axes[1] >= 32760 || axes[1] <= -32760 || axes[2] >= 32760 || axes[2] <= -32760;
            diagnostic_expected = clipped;
            assert(active != clipped);
            if (clipped) {
                assert(sequence == candidate && queued == 0 && !finishing);
                assert(strstr(control, ":t_us=96:") != nullptr);
            } else {
                assert(sequence == candidate + 1 && queued == 1);
                assert(samples[tail].time == expected_time);
                powerDown();
            }
        }
        assert(ctrl6_reads == (diagnostic_expected ? 2U : 1U));
        assert(!active && registers[CTRL1] == 0 && registers[FIFO_CTRL] == 0);
        drain();
        if (scenario == "clipping_format") assert(_SerialUSB.last_write_size < USB_PACKET_SIZE);
        std::cout << _SerialUSB.output;
        return 0;
    }
    if (scenario == "control_boundary") {
        for (size_t padding = 0; padding < USB_PACKET_SIZE; ++padding) {
            // CR padding is ignored by the line reader and selects each alignment.
            memset(tx_bytes, '\r', padding);
            tx_head = tx_queued = padding;
            tx_tail = 0;
            usb_started = nowUs();
            strcpy(control, "LIS2DWUSB,2,1234abcd");
            drain();
            assert(_SerialUSB.last_write_size < USB_PACKET_SIZE);
        }
        for (char c : _SerialUSB.output) if (c != '\r') std::cout << c;
        return 0;
    }
    if (scenario == "format") {
        const uint64_t times[] = {0, 1, UINT32_MAX, uint64_t(UINT32_MAX) + 1, UINT64_MAX};
        _SerialUSB.write_limit = 5;
        for (uint64_t timestamp : times) {
            Sample &sample = samples[head++];
            sample = {sequence++, timestamp, {-32756, 0, 16384}, 20};
            ++queued;
        }
        drain();
        std::cout << _SerialUSB.output;
        return 0;
    }
    if (scenario == "abort_fields" || scenario == "abort_crc" || scenario == "abort_send"
            || scenario == "abort_usb" || scenario == "abort_packet") {
        samples[0] = {0, 625, {4096, 0, 16384}, 20};
        if (scenario == "abort_packet")
            samples[0] = {UINT32_MAX, UINT64_MAX, {-32768, -32768, -32768}, UINT32_MAX};
        head = queued = 1;
        _SerialUSB.write_limit = 5;
        const TxState target = scenario == "abort_fields" ? TxState::FIELDS :
                               scenario == "abort_crc" ? TxState::CHECKSUM : TxState::SEND;
        do { transmit(); } while (tx_state != target);
        if (scenario == "abort_send" || scenario == "abort_usb" || scenario == "abort_packet") transmit();
        if (scenario == "abort_usb" || scenario == "abort_packet") {
            tx_flush = tx_queued;
            serviceUSB();
            serviceUSB();
            assert(usb_offset == 5);
        }
        fail("TEST_ABORT");
        drain();
        std::cout << _SerialUSB.output;
        return 0;
    }
    synthetic_workload = scenario == "workload" || scenario == "bulk_latency";
    if (scenario == "throughput" || scenario == "bulk_latency") {
        spi_call_cost_us = 15;
        // A synthetic host delays short packets more than full bulk packets.
        _SerialUSB.packet_delay_us = scenario == "bulk_latency" ? 1000 : 100;
        _SerialUSB.short_packet_delay_us = 1000;
    }
    feed("HELLO,1234abcd");
    runFor(1000);
    unsigned requested = argc > 2 ? unsigned(std::stoul(argv[2])) : 1600;
    char start[64];
    snprintf(start, sizeof(start), "START,%u,1234abcd", requested);
    feed(start);
    runFor(50000);
    if (scenario == "backlog") { fake_time += 3000; runFor(100); }
    else if (scenario == "overflow") { fake_time += 30000; runFor(100); }
    else if (scenario == "uncertain") { fake_time = next_sample + sensor_period / 3; runFor(100); }
    else if (scenario == "clipping") { raw_axis = 32760; runFor(1000); }
    else if (scenario == "sensor_timeout") { produce = false; runFor(600000); }
    else if (scenario == "buffer") {
        _SerialUSB.writable = false;
        runFor(250000);
        assert(!active);
        _SerialUSB.writable = true;
    } else if (scenario == "disconnect") {
        _SerialUSB.online = false;
        runFor(1000);
        assert(!active && registers[0x20] == 0);
    } else if (scenario == "normal" || scenario == "wrap" || scenario == "workload" || scenario == "throughput" || scenario == "bulk_latency") {
        _SerialUSB.write_limit = synthetic_workload || scenario == "throughput" || scenario == "bulk_latency" ? 64 : 5;
        runFor(scenario == "throughput" || scenario == "bulk_latency" ? 15000000 : 2600000);
        feed("STOP,1234abcd");
    }
    runFor(20000);
    assert(!active);
    if (scenario == "throughput" || scenario == "bulk_latency") {
        assert(sequence >= requested * 14);
        if (requested == 1600) {
            assert(_SerialUSB.full_packets > _SerialUSB.short_packets);
            if (scenario == "bulk_latency") assert(_SerialUSB.busy_writes > 0);
        }
    }
    drain();
    assert(ctrl6_reads == (scenario == "clipping" ? 2U : scenario == "identity" || scenario == "reset" ? 0U : 1U));
    std::cout << _SerialUSB.output;
}
