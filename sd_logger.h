#pragma once
#include <Arduino.h>
#include "json_utils.h"

bool initSDCard();
void logToSD(String message);
void flushSDLog();

// Combined log file (logs + data lines, always written)
void logDataToFile(time_t timestamp, float temp, float hum, float press,
                   float co2, int32_t voc, float pm1, float pm25, float pm10);

// Upload today's log file to S3 (called after a successful send cycle)
bool uploadLogToS3();

// Offline retry queue (only written on send failure, flushed when back online)
void queueFailedData(time_t timestamp, MeasurementData data);
bool hasPendingQueue();
bool flushPendingQueue();  // returns true if all entries sent successfully
