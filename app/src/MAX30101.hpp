/*
    This is a modification of the SparkFun MAX3010x Pulse and Proximity Sensor
   Library to run on Zephyr, with some minor additions.
    https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library

    Modified by Anthony Wertz, 2023.
*/

/***************************************************
 This is a library written for the Maxim MAX30105 Optical Smoke Detector
 It should also work with the MAX30102. However, the MAX30102 does not have a
 Green LED.

 These sensors use I2C to communicate, as well as a single (optional)
 interrupt line that is not currently supported in this driver.

 Written by Peter Jansen and Nathan Seidle (SparkFun)
 BSD license, all text above must be included in any redistribution.
 *****************************************************/

#pragma once

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

#define MAX3010x_MAX_NUM_CHANNELS 3

enum max3010x_mode {
    MAX3010x_MODE_HEART_RATE = 2,
    MAX3010x_MODE_SPO2 = 3,
    MAX3010x_MODE_MULTI_LED = 7,
};

enum max3010x_slot {
    MAX3010x_SLOT_DISABLED = 0,
    MAX3010x_SLOT_RED_LED1_PA,
    MAX3010x_SLOT_IR_LED2_PA,
    MAX3010x_SLOT_GREEN_LED3_PA,
    MAX3010x_SLOT_RED_PILOT_PA,
    MAX3010x_SLOT_IR_PILOT_PA,
    MAX3010x_SLOT_GREEN_PILOT_PA,
};

struct max3010x_config {
    struct i2c_dt_spec i2c;
    uint8_t fifo;
    uint8_t spo2;
    uint8_t led_pa[MAX3010x_MAX_NUM_CHANNELS];
    enum max3010x_mode mode;
    enum max3010x_slot slot[4];
};

class MAX30101
{
public:
    MAX30101();

    bool begin(const struct device *dev);

    uint32_t getRed(void);                  // Returns immediate red value
    uint32_t getIR(void);                   // Returns immediate IR value
    uint32_t getGreen(void);                // Returns immediate green value
    bool safeCheck(uint8_t maxTimeToCheck); // Given a max amount of time,
                                            // check for new data

    // Configuration
    void softReset();
    void shutDown();
    void wakeUp();

    void setLEDMode(uint8_t mode);

    void setADCRange(uint8_t adcRange);
    void setSampleRate(uint8_t sampleRate);
    void setPulseWidth(uint8_t pulseWidth);

    void setPulseAmplitudeRed(uint8_t value);
    void setPulseAmplitudeIR(uint8_t value);
    void setPulseAmplitudeGreen(uint8_t value);
    void setPulseAmplitudeProximity(uint8_t value);

    void setProximityThreshold(uint8_t threshMSB);

    // Multi-led configuration mode (page 22)
    void enableSlot(
        uint8_t slotNumber,
        uint8_t device); // Given slot number, assign a device to slot
    void disableSlots(void);

    // Data Collection

    // Interrupts (page 13, 14)
    uint8_t getINT1(void);  // Returns the main interrupt group
    uint8_t getINT2(void);  // Returns the temp ready interrupt
    void enableAFULL(void); // Enable/disable individual interrupts
    void disableAFULL(void);
    void enableDATARDY(void);
    void disableDATARDY(void);
    void enableALCOVF(void);
    void disableALCOVF(void);
    void enablePROXINT(void);
    void disablePROXINT(void);
    void enableDIETEMPRDY(void);
    void disableDIETEMPRDY(void);

    // FIFO Configuration (page 18)
    void setFIFOAverage(uint8_t samples);
    void enableFIFORollover();
    void disableFIFORollover();
    void setFIFOAlmostFull(uint8_t samples);

    // FIFO Reading
    uint16_t check(void);    // Checks for new data and fills FIFO
    uint8_t available(void); // Tells caller how many new samples are
                             // available (head - tail)
    void nextSample(void);   // Advances the tail of the sense array
    uint32_t getFIFORed(
        void);                // Returns the FIFO sample pointed to by tail
    uint32_t getFIFOIR(void); // Returns the FIFO sample pointed to by tail
    uint32_t getFIFOGreen(
        void); // Returns the FIFO sample pointed to by tail

    uint8_t getWritePointer(void);
    uint8_t getReadPointer(void);
    void clearFIFO(void); // Sets the read/write pointers to zero

    // Proximity Mode Interrupt Threshold
    void setPROXINTTHRESH(uint8_t val);

    // Die Temperature
    float readTemperature();
    float readTemperatureF();

    // Detecting ID/Revision
    uint8_t getRevisionID();
    uint8_t readPartID();

    // Setup the IC with user selectable settings
    void setup(
        uint8_t powerLevelRed = 0x1F,
        uint8_t powerLevelIR = 0x1F,
        uint8_t powerLevelGreen = 0x1F,
        uint8_t sampleAverage = 4,
        uint8_t ledMode = 3,
        int sampleRate = 400,
        int pulseWidth = 411,
        int adcRange = 4096);
    void setupSpO2(
        uint8_t ir_power,
        uint8_t red_power,
        uint8_t sampleAverage,
        uint8_t sampleRate,
        uint8_t pulseWidth,
        uint8_t adcRange);

    // Get configuration registers.
    uint8_t getFIFOConfig();
    uint8_t getParticleConfig();

    uint8_t getPARed();
    uint8_t getPAIR();
    uint8_t getPAGreen();

    // Low-level I2C communication
    uint8_t readRegister8(uint8_t reg);
    void writeRegister8(uint8_t reg, uint8_t value);

    uint8_t burstRead(uint8_t reg, uint16_t size);
    uint8_t burstRead_next();

private:
    const struct device *dev = nullptr;

    static const uint16_t I2C_BUFFER_LENGTH = 288;
    uint8_t burst_read_buffer[I2C_BUFFER_LENGTH];
    uint16_t burst_read_buffer_i = 0;
    uint16_t burst_read_buffer_used = 0;

    // activeLEDs is the number of channels turned on, and can be 1 to 3. 2
    // is common for Red+IR.
    uint8_t activeLEDs; // Gets set during setup. Allows check() to
                        // calculate how many bytes to read from FIFO

    uint8_t revisionID;

    void readRevisionID();

    void bitMask(uint8_t reg, uint8_t mask, uint8_t thing);

    static const int STORAGE_SIZE = 32;
    typedef struct Record
    {
        uint32_t red[STORAGE_SIZE];
        uint32_t IR[STORAGE_SIZE];
        uint32_t green[STORAGE_SIZE];
        uint8_t head;
        uint8_t tail;
    } sense_struct; // This is our circular buffer of readings from the
                    // sensor

    sense_struct sense;
};