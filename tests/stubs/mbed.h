#pragma once
#include "Arduino.h"
namespace mbed {
struct SPI {
    SPI(int, int, int) {}
    void frequency(int) {}
    void format(int, int) {}
    int write(int value) {
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
        if (transaction_address == 0x2f) {
            while (produce && registers[0x20] && registers[0x2e] && next_sample <= fake_time) {
                if (fifo_count == 32) fifo_overflow = true;
                else ++fifo_count;
                next_sample += sensor_period;
            }
            return int(fifo_count | (fifo_overflow ? 0x40 : 0));
        }
        if (transaction_address == 0x28) {
            unsigned byte = transaction_offset - 2;
            int16_t axis = byte < 2 ? int16_t(raw_axis) : byte >= 4 ? int16_t(16384) : int16_t(0);
            return byte % 2 ? (uint16_t(axis) >> 8) & 255 : uint16_t(axis) & 255;
        }
        return registers[transaction_address];
    }
};
}
