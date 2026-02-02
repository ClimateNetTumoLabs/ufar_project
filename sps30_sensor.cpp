#include "sps30_sensor.h"
#include <Wire.h>
#include "config.h"

#define SPS30_ADDR 0x69

void initSPS30() {
  Wire.begin();
}

uint8_t calcCRC(uint8_t data1, uint8_t data2) {
  uint8_t crc = 0xFF;
  crc ^= data1;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  crc ^= data2;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  return crc;
}

void sendCommand(uint16_t cmd, uint16_t arg=0x0300, bool withArg=false) {
  Wire.beginTransmission(SPS30_ADDR);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  if (withArg) {
    uint8_t msb = arg >> 8;
    uint8_t lsb = arg & 0xFF;
    Wire.write(msb);
    Wire.write(lsb);
    Wire.write(calcCRC(msb, lsb));
  }
  Wire.endTransmission();
  delay(100);
}

void startSPS30() {
  sendCommand(0x0010, 0x0300, true);
}

void stopSPS30() {
  sendCommand(0x0104);
}

bool readSPS30(float* pm) {
  Wire.beginTransmission(SPS30_ADDR);
  Wire.write(0x03);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(100);

  Wire.requestFrom(SPS30_ADDR, 60);
  if (Wire.available() < 60) return false;

  for(int i=0;i<10;i++){
    uint8_t b[4];
    for(int j=0;j<2;j++){
      b[j*2] = Wire.read();
      b[j*2+1] = Wire.read();
      Wire.read();
    }
    uint32_t tmp = ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|((uint32_t)b[3]);
    memcpy(&pm[i], &tmp, sizeof(float));
  }
  return true;
}
