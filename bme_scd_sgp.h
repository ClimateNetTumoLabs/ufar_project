#pragma once
#include <Adafruit_BME280.h>
#include "Adafruit_SCD30.h"
#include "Adafruit_SGP40.h"

struct MeasurementData {
  float temperature;
  float humidity;
  float pressure;
  float pm1;
  float pm25;
  float pm10;
  float co2;
  float voc;
};

void initOtherSensors();
MeasurementData sampleSensors(int durationSec, int intervalSec);
