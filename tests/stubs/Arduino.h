#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
extern uint64_t fake_time;
extern uint8_t registers[64];
extern unsigned transaction_address, transaction_offset, fifo_count;
extern bool reading, fifo_overflow, produce, reset_stuck;
extern int raw_axis;
extern uint64_t next_sample;
extern unsigned sensor_period;
inline int digitalPinToPinName(int pin) { return pin; }
inline void pinMode(int, int) {}
inline uint32_t micros() { return uint32_t(fake_time); }
inline void delay(unsigned ms) { fake_time += ms * 1000; }
inline void digitalWrite(int, int value) {
    if (value == LOW) transaction_offset = 0;
    else if (reading && transaction_address == 0x28 && transaction_offset == 7 && fifo_count) --fifo_count;
}
struct MockUSB {
    bool online = true, writable = true;
    unsigned write_limit = 64;
    std::deque<char> input;
    std::string output;
    void begin(unsigned) {}
    bool connected() { return online; }
    int available() { return int(input.size()); }
    int read() { char c = input.front(); input.pop_front(); return c; }
    void send_nb(uint8_t *data, uint32_t size, uint32_t *sent) {
        *sent = writable ? std::min(size, write_limit) : 0;
        output.append(reinterpret_cast<char *>(data), *sent);
    }
};
extern MockUSB _SerialUSB;
