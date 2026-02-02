#pragma once
#include <ArduinoJson.h>
#include "bme_scd_sgp.h"

String prepareJSON(const char* deviceId, time_t t, MeasurementData data);
void sendHTTP(String payload);
