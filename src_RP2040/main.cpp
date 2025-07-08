#include <Arduino.h>
#include <SPI.h>
#include "mbed.h"

/* ---------------LIS2DW12 ---------------------------- */
#define WHO_AM_I_ADDR 0x0F
#define CTRL_REG1_ADDR 0x20
#define CTRL_REG2_ADDR 0x21
#define CTRL_REG6_ADDR 0x25
#define STATUS_REG_ADDR 0x27
#define OUT_XL_ADDR 0x28
#define OUT_XH_ADDR 0x29
#define OUT_YL_ADDR 0x2A
#define OUT_YH_ADDR 0x2B
#define OUT_ZL_ADDR 0x2C
#define OUT_ZH_ADDR 0x2D
#define REG_MOD_READ 0x80

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

String fw = "LIS2DW USB accelerometer FW v0.2.2 \r\nhttps://github.com/lis2dwusb\r\nby @navaismo";

// Variable to store the sample interval in microseconds
// If sample_us == 0, it means that we should not read the sensor
// If sample_us > 0, it means that we should read the sensor at this interval
// (e.g., 1000 means 1 ms, 50000 means 50 ms, etc.)
volatile uint32_t sample_us = 0;
uint32_t t0_us = 0;
uint32_t next_us = 0;
uint8_t fs_setting = 0; // Full-scale setting read from CTRL_REG6
float lsb_per_g = 16384.0f; // We always set the scale at ±2g, so this is the LSB per G value

// Struct to hold axis readings
// Each axis is represented by two bytes: LSB and MSB
// The value is stored as a signed 16-bit integer
union AxisReading
{
    struct
    {
        uint8_t low, high;
    };
    int16_t value;
};

static mbed::SPI spiPort(
    digitalPinToPinName(MOSI_PIN),
    digitalPinToPinName(MISO_PIN),
    digitalPinToPinName(SCK_PIN));
// SPI speed in Hz
// The LIS2DW12 supports up to 10 MHz, but we use 5 MHz for stability
// (as per the datasheet, it is recommended to use a lower speed for reliable communication)
constexpr uint32_t SPI_SPEED = 10'000'000;

// Buffer to accumulate command line
String cmd = "";

// Helpers SPI /CS
inline void csLow() { digitalWrite(CS_PIN, LOW); }
inline void csHigh() { digitalWrite(CS_PIN, HIGH); }

// Write a byte to the LIS2DW sensor
void lisWrite(uint8_t reg, uint8_t val)
{
    csLow();
    spiPort.write(reg & 0x3F);
    spiPort.write(val);
    csHigh();
}
// Read a byte from the LIS2DW sensor
uint8_t lisReadReg(uint8_t reg)
{
    csLow();
    spiPort.write(REG_MOD_READ | (reg & 0x3F));
    uint8_t v = spiPort.write(0);
    csHigh();
    return v;
}

// Reinitialize the sensor with a new frequency
// This function is called when the user sends a command to change the frequency
void reinitSensor(uint32_t f_hz)
{
     // Soft reset
    lisWrite(CTRL_REG2_ADDR, 0x40);
    delay(30);
    while (lisReadReg(CTRL_REG2_ADDR) & 0x40);

    // CTRL_REG2
    // BDU = 0 (block data continuos update), bit3 of CTRL_REG2, 
    // IF_ADDR_INC = 1 for auto-increment bit2 of CTRL_REG2
    lisWrite(CTRL_REG2_ADDR, 0x04);


    // CTRL_REG6
    // Bandwith (bit6,bit7) = 00 ODR/2 800 - 1600 Hz
    // Fullscale ±2g (bit5-bit4) = 00
    // Filtered data (bit3) = 0 - Low-pass filter enabled
    // Low_noise (bit2) = 0  - Disabled
    // bit1 = 0 (not used)
    // bit0 = 0 (not used) 
    lisWrite(CTRL_REG6_ADDR, 0x00);


    // Ctrl_reg1: We choose the full value according to frequency
    uint8_t ctrl1_val;
    String fq = "0"; // Frequency in Hz, used for Serial print
    if (f_hz >= 1500){ //1600 Hz
        // CTRL_REG1
        // ODR (bit7-bit4) = 1001  - 1600 Hz
        // MODE(bit3-bit2) = 01    - High-performance mode
        // LP_MODE(bit1-bit0) = 11 - Low-power mode 4, 14 bit resolution
        ctrl1_val = 0x97;
        fq = "1600"; 
    }else if (f_hz >= 700){ // 800 Hz
        // CTRL_REG1
        // ODR (bit7-bit4) = 1000  - 800 Hz
        // MODE(bit3-bit2) = 01    - High-performance mode
        // LP_MODE(bit1-bit0) = 11 - Low-power mode 4, 14 bit resolution
        ctrl1_val = 0x87;
        fq = "800"; 
    }else if (f_hz >= 350){ // 400 Hz
        // CTRL_REG1
        // ODR (bit7-bit4) = 0111  - 400 Hz
        // MODE(bit3-bit2) = 01    - High-performance mode
        // LP_MODE(bit1-bit0) = 11 - Low-power mode 4, 14 bit resolution
        ctrl1_val = 0x77;
        fq = "400"; 
    }else if (f_hz >= 150){ // 200 Hz
        // CTRL_REG1
        // ODR (bit7-bit4) = 0110  - 200 Hz
        // MODE(bit3-bit2) = 01    - High-performance mode
        // LP_MODE(bit1-bit0) = 11 - Low-power mode 4, 14 bit resolution
        ctrl1_val = 0x67;
        fq = "200"; 
    }else{
        ctrl1_val = 0x97; // 1600 Hz
        fq = "1600"; // Default to 1600 Hz if frequency is too low
    }
    // Set ODR and enable the sensor
    lisWrite(CTRL_REG1_ADDR, ctrl1_val);

    // Restart our t0_us
    t0_us = micros();
    next_us = t0_us;

    // Print the reinitialization information
    Serial.println("Rebooting to setup new Freq: " + fq + " Hz");
    Serial.println(fw);
    Serial.print("WHO_AM_I: 0x");
    Serial.println(lisReadReg(WHO_AM_I_ADDR), HEX);
    Serial.print("CTRL_REG1: 0x");
    Serial.println(lisReadReg(CTRL_REG1_ADDR), HEX);
    Serial.print("CTRL_REG2: 0x");
    Serial.println(lisReadReg(CTRL_REG2_ADDR), HEX);
    Serial.print("CTRL_REG6: 0x");
    fs_setting = lisReadReg(CTRL_REG6_ADDR);
    Serial.println(fs_setting, HEX);
}

// Setup LIS2DW sensor
void setupLIS2DW()
{
    pinMode(CS_PIN, OUTPUT);
    csHigh();
    spiPort.frequency(SPI_SPEED);
    spiPort.format(8, 0);

    // Soft reset
    lisWrite(CTRL_REG2_ADDR, 0x40);
    delay(30);
    while (lisReadReg(CTRL_REG2_ADDR) & 0x40);

    // BDU = 0 (block data continuos update), bit3 of CTRL_REG2, 
    // IF_ADDR_INC = 1 for auto-increment bit2 of CTRL_REG2
    lisWrite(CTRL_REG2_ADDR, 0x04);

    // CTRL_REG6
    // Bandwith (bit6,bit7) = 00 ODR/2 800 - 1600 Hz
    // Fullscale ±2g (bit5-bit4) = 00
    // Filtered data (bit3) = 0 - Low-pass filter enabled
    // Low_noise (bit2) = 0  - Disabled
    // bit1 = 0 (not used)
    // bit0 = 0 (not used) 
    lisWrite(CTRL_REG6_ADDR, 0x00);

    // CTRL_REG1
    // ODR (bit7-bit4) = 1001  - 1600 Hz
    // MODE(bit3-bit2) = 01    - High-performance mode
    // LP_MODE(bit1-bit0) = 11 - Low-power mode 4, 14 bit resolution
    lisWrite(CTRL_REG1_ADDR, 0x97);

    // Save times if you use them ...
    t0_us = micros();
    next_us = t0_us;
}


void setup()
{
    Serial.begin(2000000);
    while (!Serial)
    {
    }

    setupLIS2DW();

    // Print banner, but we don't print data or header yet:
    Serial.println(fw);
    Serial.print("WHO_AM_I: 0x");
    Serial.println(lisReadReg(WHO_AM_I_ADDR), HEX);
    Serial.print("CTRL_REG1: 0x");
    Serial.println(lisReadReg(CTRL_REG1_ADDR), HEX);
    Serial.print("CTRL_REG2: 0x");
    Serial.println(lisReadReg(CTRL_REG2_ADDR), HEX);
    Serial.print("CTRL_REG6: 0x");
    fs_setting = lisReadReg(CTRL_REG6_ADDR);
    Serial.println(fs_setting, HEX);
}

// Check if new data is ready
bool dataReady()
{
    // If DRDY bit (bit0) of STATUS_REG is set, it means that new data is ready
    return (lisReadReg(STATUS_REG_ADDR) & 0x01) != 0;
}

// Read axes X, Y, Z in burst mode
// This function reads all three axes in one burst read operation
// It is more efficient than reading each axis separately
void burstReadAxes(AxisReading &x, AxisReading &y, AxisReading &z)
{
    // Wait for fresh data
    while (!dataReady());

    uint8_t buf[6];
    csLow();
    // Send the read command for OUT_XL_ADDR
    spiPort.write(0x80 | OUT_XL_ADDR); // READ  | OUT_X_L
    // Read 6 bytes(REG ADDRs) in burst mode
    for (int i = 0; i < 6; i++)
        buf[i] = spiPort.write(0);
    

    csHigh();
    // Assign the values to the AxisReading structures
    x.low = buf[0];
    x.high = buf[1];
    y.low = buf[2];
    y.high = buf[3];
    z.low = buf[4];
    z.high = buf[5];
}

uint32_t samples = 0;
// Main loop
void loop()
{
    // Process serial bytes (to assemble CMD complete)
    while (Serial.available())
    {
        char c = Serial.read();
        if (c == '\r')
            continue;
        if (c == '\n')
        {
            // End of line arrived → We process CMD:
            cmd.trim();
            cmd.toUpperCase();
            if (cmd.startsWith("F="))
            {
                // It is "F = <n>"
                uint32_t f = cmd.substring(2).toInt();
                if (f < 200) // Minimum frequency is 200 Hz
                    f = 200;
                if (f > 2000) // Maximum frequency is 2000 Hz
                    f = 2000;

                // Calculate interval in µs and reconfigure ODR
                sample_us = 1'000'000UL / f;
                reinitSensor(f);

                // Send the header to confirm:
                Serial.println("time,x,y,z");
            }
            else if (cmd == "Q")
            {
                // If "Q" arrives, we stop the transmission completely
                sample_us = 0; // Indicates "do not send" to another f =
                // (If I want, I can print something like "Stopped \ n")
                // Serial.println ("stopped");
            }
            cmd = "";
        }
        else
        {
            cmd += c;
        }
    }

    // If sample_us == 0, it means that we should not read the sensor or the Register DRDY has not data ready
    if (sample_us == 0 || !dataReady()) return;
    
        // Read Axes X, Y, Z.
        // AxisReading x, y, z;

        // x.low  = lisReadReg(OUT_XL_ADDR);
        // x.high = lisReadReg(OUT_XH_ADDR);
        // y.low  = lisReadReg(OUT_YL_ADDR);
        // y.high = lisReadReg(OUT_YH_ADDR);
        // z.low  = lisReadReg(OUT_ZL_ADDR);
        // z.high = lisReadReg(OUT_ZH_ADDR);

        AxisReading x, y, z;
        burstReadAxes(x, y, z);

        // Convert raw values to mg (milligravity)
        int16_t xi = (int16_t)(x.value * 1000 / lsb_per_g);
        int16_t yi = (int16_t)(y.value * 1000 / lsb_per_g);
        int16_t zi = (int16_t)(z.value * 1000 / lsb_per_g);


        char buf[48];
        // Send the data in CSV format
        snprintf(buf, sizeof(buf), "%d,%d,%d", xi, yi, zi);
        Serial.print(buf);
        Serial.print("\r\n");

        // samples++;
        // if(millis() % 1000 == 0) {
        //    Serial.print("Samples: ");
        //    Serial.println(samples);
        // }


    //}
}
