#include <Wire.h>
#include "config.h"
#include "wifi_manager.h"
#include "sps30_sensor.h"
#include "bme_scd_sgp.h"
#include "rtc_utils.h"
#include "json_utils.h"

/* ================= RTC MEMORY ================= */
RTC_DATA_ATTR time_t lastSent = 0;   // store last successful send

#if DEBUG
  #define DBG(x) Serial.println(x)
  #define DBG2(x,y) Serial.print(x); Serial.println(y)
#else
  #define DBG(x)
  #define DBG2(x,y) Serial.print(x); Serial.println(y)
#endif
void setup() {
  Serial.begin(115200);
  delay(2000);
  DBG("[BOOT] ESP32 started");

  Wire.begin();

  connectWiFi();
  syncTime();

  initSPS30();
  initOtherSensors();

  time_t now = time(nullptr);
  time_t nextSend = calculateNextSend(now, lastSent, MEASURE_INTERVAL_MIN);
  DBG2("[TIME] Next send aligned: ", timeToStr(nextSend));
}

void loop() {
  time_t now = time(nullptr);
  time_t nextSend = calculateNextSend(now, lastSent, MEASURE_INTERVAL_MIN);

  // Calculate sleep until measurement start (before warm-up)
  int64_t sleepSec = difftime(nextSend, now) - (SPS30_WARMUP_SEC + SAMPLE_DURATION_SEC);

  if(sleepSec > 0){
    DBG2("[SLEEP] Sleeping until measurement start in seconds: ", sleepSec);
    esp_sleep_enable_timer_wakeup(sleepSec * 1000000ULL);
    esp_deep_sleep_start();
  }

  // Start measurement
  startSPS30();
  DBG("[SPS30] Warm-up before measurement");
  delay(SPS30_WARMUP_SEC * 1000);

  // Sample sensors
  MeasurementData data = sampleSensors(SAMPLE_DURATION_SEC, SAMPLE_INTERVAL_SEC);

  // Send JSON
  String payload = prepareJSON(DEVICE_ID, nextSend, data);
  DBG("[JSON]"); DBG(payload);
  sendHTTP(payload);

  // Stop SPS30
  stopSPS30();

  // Update last sent
  lastSent = nextSend;

  // Sleep until next interval
  now = time(nullptr);
  nextSend = calculateNextSend(now, lastSent, MEASURE_INTERVAL_MIN);
  sleepSec = difftime(nextSend, now) - (SPS30_WARMUP_SEC + SAMPLE_DURATION_SEC);
  if(sleepSec < 0) sleepSec = 0; // ensure positive
  DBG2("[SLEEP] Deep sleeping until next measurement: ", sleepSec);
  esp_sleep_enable_timer_wakeup(sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}
