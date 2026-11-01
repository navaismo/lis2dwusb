#include "Arduino.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

uint64_t fake_time = 100;
uint8_t registers[64] = {};
unsigned transaction_address = 0, transaction_offset = 0, fifo_count = 0;
bool reading = false, fifo_overflow = false, produce = true, reset_stuck = false;
int raw_axis = 4096;
uint64_t next_sample = 0;
unsigned sensor_period = 625;
MockUSB _SerialUSB;

#include "../src_RP2040/main.cpp"

void runFor(uint64_t duration) {
    uint64_t end = fake_time + duration;
    while (fake_time < end) { loop(); fake_time += 20; }
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
    } else if (scenario == "normal" || scenario == "wrap") {
        _SerialUSB.write_limit = 5;
        runFor(2600000);
        feed("STOP,1234abcd");
    }
    runFor(20000);
    assert(!active);
    std::cout << _SerialUSB.output;
}
