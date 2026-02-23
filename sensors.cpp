#include "sensors.h"

// ===================== BME280 =====================
bool BME280Sensor::init(uint8_t address) {
    return bme.begin(address);
}

void BME280Sensor::start() {
    bme.setSampling(
        Adafruit_BME280::MODE_NORMAL,
        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::FILTER_OFF,
        Adafruit_BME280::STANDBY_MS_1000
    );
}

void BME280Sensor::stop() {
    bme.setSampling(Adafruit_BME280::MODE_SLEEP);
}

void BME280Sensor::sleep() {
    stop();
}

bool BME280Sensor::read(float &temperature, float &humidity, float &pressure) {
    temperature = bme.readTemperature();
    humidity    = bme.readHumidity();
    pressure    = bme.readPressure() / 100.0F; // hPa
    return true;
}

// ===================== SCD30 =====================
bool SCD30Sensor::init() {
    return scd30.begin();
}

void SCD30Sensor::start(uint16_t pressure_hPa) {
    if (pressure_hPa > 0) {
        scd30.startContinuousMeasurement(pressure_hPa);
    } else {
        scd30.startContinuousMeasurement();
    }
}

void SCD30Sensor::stop() {
    // No real stop command, best we can do is slow it down
    scd30.setMeasurementInterval(60);
}

void SCD30Sensor::sleep() {
    stop();
}

bool SCD30Sensor::read(float &co2) {
    if (!scd30.dataReady()) return false;
    if (!scd30.read()) return false;

    co2 = scd30.CO2;
    return true;
}

// ===================== SGP40 =====================
bool SGP40Sensor::init() {
    if (!sgp40.begin()) return false;
    return sgp40.selfTest();
}

void SGP40Sensor::start() {
    // Heater auto-starts on measurement
}

void SGP40Sensor::stop() {
    sgp40.heaterOff();
}

void SGP40Sensor::sleep() {
    stop();
}

bool SGP40Sensor::read(int32_t &vocIndex, float temperature, float humidity) {
    vocIndex = sgp40.measureVocIndex(temperature, humidity);
    return true;
}

// ===================== SPS30 =====================
bool SPS30Sensor::init() {
    Wire.begin();
    delay(100); // Give sensor time to boot
    
    // Wake up sensor with retry
    for (int attempt = 0; attempt < 3; attempt++) {
        if (wakeUp()) {
            delay(100); // Wait for sensor to fully wake
            return true;
        }
        delay(200);
    }
    
    // If wake-up failed, still return true and try to use sensor
    // (sensor might already be awake)
    delay(100);
    return true;
}

bool SPS30Sensor::start() {
    // Start measurement command: 0x0010
    // Argument: 0x0300 (big endian format, IEEE754 float)
    Wire.beginTransmission(SPS30_I2C_ADDR);
    Wire.write(0x00);
    Wire.write(0x10);
    Wire.write(0x03);
    Wire.write(0x00);
    Wire.write(calcCRC(0x03, 0x00));
    
    if (Wire.endTransmission() != 0) return false;
    
    delay(50); // Wait for fan to start spinning
    return true;
}

bool SPS30Sensor::stop() {
    return writeCommand(0x0104); // Stop measurement
}

bool SPS30Sensor::sleep() {
    writeCommand(0x1001); // Sleep command
    return true;
}

bool SPS30Sensor::wakeUp() {
    bool result = writeCommand(0x1103); // Wake-up command
    return result;
}

bool SPS30Sensor::read(float &pm1, float &pm25, float &pm10) {
    // Check if data is ready
    Wire.beginTransmission(SPS30_I2C_ADDR);
    Wire.write(0x02);
    Wire.write(0x02);
    if (Wire.endTransmission() != 0) return false;
    
    delay(20);
    
    Wire.requestFrom(SPS30_I2C_ADDR, 3);
    if (Wire.available() < 3) return false;
    
    uint8_t ready_h = Wire.read();
    uint8_t ready_l = Wire.read();
    uint8_t crc = Wire.read();
    
    if (crc != calcCRC(ready_h, ready_l)) return false;
    if (ready_l == 0x00) return false; // Not ready
    
    // Read measured values: 0x0300
    Wire.beginTransmission(SPS30_I2C_ADDR);
    Wire.write(0x03);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    
    delay(50);
    
    // Request 60 bytes (10 floats × 6 bytes each: 4 data + 2 CRC)
    Wire.requestFrom(SPS30_I2C_ADDR, 60);
    if (Wire.available() < 60) return false;

    uint8_t data[60];
    for (int i = 0; i < 60; i++) {
        data[i] = Wire.read();
    }
    
    // Extract PM values (each float is 4 bytes + 2 CRC bytes)
    pm1  = bytesToFloatWithCRC(&data[0]);   // PM1.0
    pm25 = bytesToFloatWithCRC(&data[6]);   // PM2.5
    pm10 = bytesToFloatWithCRC(&data[12]);  // PM10

    return true;
}

bool SPS30Sensor::writeCommand(uint16_t cmd) {
    Wire.beginTransmission(SPS30_I2C_ADDR);
    Wire.write(cmd >> 8);
    Wire.write(cmd & 0xFF);
    return (Wire.endTransmission() == 0);
}

uint8_t SPS30Sensor::calcCRC(uint8_t d1, uint8_t d2) {
    uint8_t crc = 0xFF;
    
    crc ^= d1;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x80) {
            crc = (crc << 1) ^ 0x31;
        } else {
            crc <<= 1;
        }
    }
    
    crc ^= d2;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x80) {
            crc = (crc << 1) ^ 0x31;
        } else {
            crc <<= 1;
        }
    }
    
    return crc;
}

float SPS30Sensor::bytesToFloatWithCRC(uint8_t *b) {
    // Validate CRCs
    if (b[2] != calcCRC(b[0], b[1])) return -1.0;
    if (b[5] != calcCRC(b[3], b[4])) return -1.0;
    
    // Convert 4 bytes to float (big-endian)
    uint32_t temp = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | 
                    ((uint32_t)b[3] << 8) | b[4];
    float f;
    memcpy(&f, &temp, sizeof(f));
    return f;
}