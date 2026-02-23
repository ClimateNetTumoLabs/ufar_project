#ifndef SENSORS_H
#define SENSORS_H

#include <Wire.h>
#include <Adafruit_BME280.h>
#include "Adafruit_SCD30.h"
#include "Adafruit_SGP40.h"

// ===================== BME280 =====================
class BME280Sensor {
public:
    bool init(uint8_t address = 0x76);
    void start();
    void stop();     // not really supported, kept for interface consistency
    void sleep();    // same as stop for BME280

    bool read(float &temperature, float &humidity, float &pressure);

private:
    Adafruit_BME280 bme;
};

// ===================== SCD30 =====================
class SCD30Sensor {
public:
    bool init();
    void start(uint16_t pressure_hPa = 0); // optional pressure compensation
    void stop();                           // no true stop, but interval can be increased
    void sleep();                          // alias for stop

    bool read(float &co2);

private:
    Adafruit_SCD30 scd30;
};

// ===================== SGP40 =====================
class SGP40Sensor {
public:
    bool init();
    void start();
    void stop();   // heater off
    void sleep();  // heater off

    bool read(int32_t &vocIndex, float temperature = 25.0, float humidity = 50.0);

private:
    Adafruit_SGP40 sgp40;
};

// ===================== SPS30 =====================
#define SPS30_I2C_ADDR 0x69

class SPS30Sensor {
public:
    bool init();
    bool start();
    bool stop();
    bool sleep();
    bool wakeUp();
    bool read(float &pm1, float &pm25, float &pm10);

private:
    bool writeCommand(uint16_t cmd);
    uint8_t calcCRC(uint8_t d1, uint8_t d2);
    float bytesToFloatWithCRC(uint8_t *b);
};


#endif
