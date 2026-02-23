#pragma once
#include <ArduinoJson.h>
#include <Arduino.h>

struct MeasurementData {
  float temperature;
  float humidity;
  float pressure;
  float pm1;
  float pm25;
  float pm10;
  float co2;
  int32_t voc;
};

String prepareJSON(const char* deviceId, time_t t, MeasurementData data);
bool sendHTTP(String payload);
