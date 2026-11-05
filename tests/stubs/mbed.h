#pragma once
#include "Arduino.h"
extern int raw_y, raw_z;
extern unsigned ctrl6_reads, axes_reads;
extern uint8_t fifo_flags;
extern bool mutate_on_ctrl6_read;
extern int ctrl6_readback_override, ctrl6_transfer_override;
namespace mbed {
struct SPI {
    SPI(int, int, int) {}
    void frequency(int) {}
    void format(int, int) {}
    int write(int value) {
        fake_time += spi_call_cost_us;
        return transfer(value);
    }
    int write(const char *tx, int tx_size, char *rx, int rx_size) {
        fake_time += spi_call_cost_us;
        const int length = std::max(tx_size, rx_size);
        for (int i = 0; i < length; ++i) {
            const int value = transfer(i < tx_size ? uint8_t(tx[i]) : 0xff);
            if (i < rx_size) rx[i] = char(value);
        }
        return reading && transaction_address == 0x25 && ctrl6_transfer_override >= 0
            ? ctrl6_transfer_override : length;
    }
    int transfer(int value) {
        ++fake_time;
        if (transaction_offset++ == 0) {
            transaction_address = unsigned(value) & 0x7f;
            reading = value & 0x80;
            return 0;
        }
        if (!reading) {
            registers[transaction_address] = uint8_t(value);
            if (transaction_address == 0x2e) {
                fifo_count = 0;
                fifo_overflow = false;
                next_sample = fake_time + sensor_period;
            }
            if (transaction_address == 0x20 && value) {
                sensor_period = 1000000 / (value == 0x94 ? 1600 : value == 0x84 ? 800 : value == 0x74 ? 400 : 200);
            }
            return 0;
        }
        if (transaction_address == 0x21 && !reset_stuck) registers[0x21] &= ~0x40;
        if (transaction_address == 0x25) {
            ++ctrl6_reads;
            if (mutate_on_ctrl6_read) {
                raw_axis = 1; raw_y = 2; raw_z = 3;
                fifo_count = 7;
                fake_time += 100000;
            }
            if (ctrl6_readback_override >= 0) return ctrl6_readback_override;
        }
        if (transaction_address == 0x2f) {
            while (produce && registers[0x20] && registers[0x2e] && next_sample <= fake_time) {
                if (fifo_count == 32) fifo_overflow = true;
                else ++fifo_count;
                next_sample += sensor_period;
            }
            return int(fifo_count | fifo_flags | (fifo_overflow ? 0x40 : 0));
        }
        if (transaction_address == 0x28) {
            unsigned byte = transaction_offset - 2;
            if (!byte) ++axes_reads;
            int16_t axis = byte < 2 ? int16_t(raw_axis) : byte >= 4 ? int16_t(raw_z) : int16_t(raw_y);
            return byte % 2 ? (uint16_t(axis) >> 8) & 255 : uint16_t(axis) & 255;
        }
        return registers[transaction_address];
    }
};
}
