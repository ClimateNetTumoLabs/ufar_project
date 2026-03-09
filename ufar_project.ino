#include <Wire.h>
#include <esp_sleep.h>
#include "config.h"
#include "sensors.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "rtc_utils.h"
#include "json_utils.h"
#include "ota_updater.h"

// ===================== RTC Memory (persists through deep sleep) =====================
RTC_DATA_ATTR time_t lastMeasurementTime = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR bool timeIsSynced = false;

// ===================== Sensor Objects =====================
BME280Sensor bme280;
SCD30Sensor  scd30;
SGP40Sensor  sgp40;
SPS30Sensor  sps30;

// ===================== Measurement Data =====================
struct SensorReadings {
  float temperature = 0.0;
  float humidity = 0.0;
  float pressure = 0.0;
  float co2 = 0.0;
  int32_t vocIndex = 0;
  float pm1 = 0.0;
  float pm25 = 0.0;
  float pm10 = 0.0;
  int validSamples = 0;
};

// ===================== Sensor Initialization =====================
bool initAllSensors() {
  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeout(1000);

  bool allOk = true;

    if (!bme280.init()) { logToSD("[BME280] ERROR: Init failed"); allOk = false; }
    if (!scd30.init()) { logToSD("[SCD30] ERROR: Init failed"); allOk = false; }
    if (!sgp40.init()) { logToSD("[SGP40] ERROR: Init failed"); allOk = false; }
    if (!sps30.init()) { logToSD("[SPS30] ERROR: Init failed"); allOk = false; }

  if (!allOk)
    logToSD("[SENSORS] WARNING: Some sensors failed init");

  return allOk;
}

// ===================== Sensor Start/Stop =====================
void startAllSensors(float pressure_hPa) {
  bme280.start();
  scd30.start((uint16_t)pressure_hPa);
  sgp40.start();
  if (!sps30.start())
    logToSD("[SPS30] ERROR: Failed to start");
}

void stopAllSensors() {
  bme280.stop();
  scd30.stop();
  sgp40.stop();
  sps30.stop();
}

// ===================== Single Reading =====================
bool takeSingleReading(SensorReadings &reading) {
  float temp, hum, press;

  if (bme280.read(temp, hum, press)) {
    reading.temperature += temp;
    reading.humidity += hum;
    reading.pressure += press;
  } else {
    logToSD("[BME280] ERROR: Read failed");
  }

  float co2;
  if (scd30.read(co2)) {
    reading.co2 += co2;
  } else {
    logToSD("[SCD30] WARNING: Data not ready");
  }

  int32_t voc;
  if (sgp40.read(voc, temp, hum)) {
    reading.vocIndex += voc;
  } else {
    logToSD("[SGP40] ERROR: Read failed");
  }

  float pm1, pm25, pm10;
  if (sps30.read(pm1, pm25, pm10)) {
    reading.pm1 += pm1;
    reading.pm25 += pm25;
    reading.pm10 += pm10;
  } else {
    logToSD("[SPS30] WARNING: Data not ready");
  }

  reading.validSamples++;
  return true;
}

// ===================== Measurement Cycle =====================
void performMeasurementCycle(MeasurementData &finalData) {
  SensorReadings accumulated;

  float temp, hum, press;
  bme280.read(temp, hum, press);

  startAllSensors(press);
  delay(SPS30_WARMUP_SEC * 1000);
  logToSD("[MEASURE] Starting measurement");
  int numSamples = SAMPLE_DURATION_SEC / SAMPLE_INTERVAL_SEC;

  for (int i = 0; i < numSamples; i++) {
    takeSingleReading(accumulated);
    if (i < numSamples - 1) delay(SAMPLE_INTERVAL_SEC * 1000);
  }

  if (accumulated.validSamples > 0) {
    finalData.temperature = accumulated.temperature / accumulated.validSamples;
    finalData.humidity    = accumulated.humidity    / accumulated.validSamples;
    finalData.pressure    = accumulated.pressure    / accumulated.validSamples;
    finalData.co2         = accumulated.co2         / accumulated.validSamples;
    finalData.voc         = accumulated.vocIndex    / accumulated.validSamples;
    finalData.pm1         = accumulated.pm1         / accumulated.validSamples;
    finalData.pm25        = accumulated.pm25        / accumulated.validSamples;
    finalData.pm10        = accumulated.pm10        / accumulated.validSamples;

    logToSD("[MEASURE] T=" + String(finalData.temperature, 1) +
            "C H=" + String(finalData.humidity, 1) +
            "% P=" + String(finalData.pressure, 1) +
            "hPa CO2=" + String((int)finalData.co2) +
            "ppm VOC=" + String(finalData.voc) +
            " PM2.5=" + String(finalData.pm25, 2) + "ug/m3");
  } else {
    logToSD("[MEASURE] ERROR: No valid samples collected");
  }

  stopAllSensors();
}

// ===================== Data Transmission =====================
bool sendData(time_t timestamp, MeasurementData &data) {
  logDataToFile(timestamp, data.temperature, data.humidity, data.pressure,
                data.co2, data.voc, data.pm1, data.pm25, data.pm10);

  if (!connectWiFi()) {
    logToSD("[SEND] ERROR: WiFi failed - data queued");
    queueFailedData(timestamp, data);
    return false;
  }

  String payload = prepareJSON(DEVICE_ID, timestamp, data);

  #if DEBUG
  logToSD("[SEND] JSON: " + payload);
  #endif

  bool success = sendHTTP(payload);

  if (success) {
    logToSD("[SEND] OK");
  } else {
    logToSD("[SEND] ERROR: API failed - data queued");
    queueFailedData(timestamp, data);
  }

  return success;
}

void enterDeepSleep(uint64_t sleepTimeSeconds) {
    logToSD("[SLEEP] " + String((uint32_t)sleepTimeSeconds) +
            "s, wake: " + timeToStr(time(nullptr) + sleepTimeSeconds));
    esp_sleep_enable_timer_wakeup(sleepTimeSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

// ===================== Setup =====================
void setup(){
  bootCount++;

  #if DEBUG
  Serial.begin(115200);
  Serial.println("\n\n========== BOOT " + String(bootCount) + " ==========");
  #endif

  // ── 1. SD Card ────────────────────────────────
  if (!initSDCard()) {
    #if DEBUG
    Serial.println("SD card init failed - continuing anyway");
    #endif
  }
  logToSD("[SYSTEM] Boot #" + String(bootCount));

  bool wifiOk = false;
  for (int attempt = 1; attempt <= 3 && !wifiOk; attempt++) {
    if (connectWiFi()) {
      wifiOk = true;
    } else {
      logToSD("[WIFI] Failed (attempt " + String(attempt) + "/3)");
      delay(2000);
    }
  }

  if (!wifiOk) {
    logToSD("[SYSTEM] CRITICAL: No WiFi - rebooting in 60s");
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }

  // ── 3. Time sync ──────────────────────────────
  if (!timeIsSynced) {
    bool synced = false;
    for (int attempt = 1; attempt <= 3 && !synced; attempt++) {
      if (syncTime()) {
        synced = true;
        timeIsSynced = true;
      } else {
        logToSD("[NTP] Failed (attempt " + String(attempt) + "/3)");
        delay(3000);
      }
    }

    if (!synced) {
      logToSD("[SYSTEM] CRITICAL: NTP failed - rebooting in 60s");
      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      esp_deep_sleep_start();
    }
  }

  configTime(ARMENIA_TZ_OFFSET, ARMENIA_DST_OFFSET,
             "pool.ntp.org", "time.nist.gov", "time.google.com");
  delay(100);

  time_t now = time(nullptr);

  if (bootCount == 1) {
    lastMeasurementTime = now;
    logToSD("[SYSTEM] First boot: " + timeToStr(now));
  }

  // ── 4. OTA + flush offline queue ──────────────
  if (hasPendingQueue()) {
    logToSD("[SYSTEM] Flushing offline queue...");
    flushPendingQueue();
  }
  checkAndApplyOTA();

  // ── 6. Measurement window calculation ─────────
  uint32_t measurementTimeNeeded = SPS30_WARMUP_SEC + SAMPLE_DURATION_SEC;
  time_t nextSendTime       = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
  time_t startMeasurementTime = nextSendTime - measurementTimeNeeded;

  bool shouldMeasure       = (bootCount == 1) ||
                             (now >= startMeasurementTime - 30 && now < nextSendTime);
  time_t measurementTimestamp = nextSendTime;

  if (now >= nextSendTime) {
    logToSD("[SYSTEM] WARNING: Missed window, rescheduling");
    lastMeasurementTime     = now;
    nextSendTime            = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
    startMeasurementTime    = nextSendTime - measurementTimeNeeded;
    measurementTimestamp    = nextSendTime;
    shouldMeasure           = false;
  }

  // ── 6. Measure ────────────────────────────────
  if (shouldMeasure) {
    if (!initAllSensors())
      logToSD("[SYSTEM] WARNING: Some sensors failed init");

    MeasurementData data;
    performMeasurementCycle(data);

    if (bootCount == 1)
      measurementTimestamp = time(nullptr);

    logToSD("[SYSTEM] Timestamp: " + timeToStr(measurementTimestamp));

    // ── 7. Send ───────────────────────────────────
    bool wifiReady = false;
    for (int attempt = 1; attempt <= 3 && !wifiReady; attempt++) {
      if (connectWiFi()) { wifiReady = true; break; }
      logToSD("[WIFI] Reconnect failed (attempt " + String(attempt) + "/3)");
      delay(2000);
    }

    if (wifiReady) {
      bool sent = sendData(measurementTimestamp, data);
      if (sent) {
        lastMeasurementTime = measurementTimestamp;
      }
    } else {
      logToSD("[SEND] No WiFi - queuing data");
      queueFailedData(measurementTimestamp, data);
      lastMeasurementTime = measurementTimestamp;
    }

    now                  = time(nullptr);
    nextSendTime         = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
    startMeasurementTime = nextSendTime - measurementTimeNeeded;
  }

  // ── 8. Deep sleep ─────────────────────────────
  now = time(nullptr);
  uint64_t sleepSeconds = (startMeasurementTime > now + 10)
                          ? (startMeasurementTime - now) - 10
                          : 10;

  enterDeepSleep(sleepSeconds);
}

// ===================== Loop =====================
void loop() {
  // Never reached - ESP is always in deep sleep between measurements
}