#include <Wire.h>
#include <esp_sleep.h>
#include "config.h"
#include "sensors.h"
#include "sd_logger.h"
#include "sim_manager.h"
#include "rtc_utils.h"
#include "json_utils.h"

// ===================== RTC Memory =====================
RTC_DATA_ATTR time_t lastMeasurementTime = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR bool timeIsSynced = false;

// ===================== Sensors =====================
BME280Sensor bme280;
SCD30Sensor  scd30;
SGP40Sensor  sgp40;
SPS30Sensor  sps30;

// ===================== Data Struct =====================
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

// ===================== Sensor Init =====================
bool initAllSensors() {
  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeout(1000);

  bool allOk = true;

  if (!bme280.init()) { logToSD("[BME280] ERROR"); allOk = false; }
  if (!scd30.init()) { logToSD("[SCD30] ERROR"); allOk = false; }
  if (!sgp40.init()) { logToSD("[SGP40] ERROR"); allOk = false; }
  if (!sps30.init()) { logToSD("[SPS30] ERROR"); allOk = false; }

  return allOk;
}

// ===================== Sensor Control =====================
void startAllSensors(float pressure_hPa) {
  bme280.start();
  scd30.start((uint16_t)pressure_hPa);
  sgp40.start();
  sps30.start();
}

void stopAllSensors() {
  bme280.stop();
  scd30.stop();
  sgp40.stop();
  sps30.stop();
}

// ===================== Read =====================
bool takeSingleReading(SensorReadings &r) {
  float t, h, p;

  if (bme280.read(t, h, p)) {
    r.temperature += t;
    r.humidity += h;
    r.pressure += p;
  }

  float co2;
  if (scd30.read(co2)) r.co2 += co2;

  int32_t voc;
  if (sgp40.read(voc, t, h)) r.vocIndex += voc;

  float pm1, pm25, pm10;
  if (sps30.read(pm1, pm25, pm10)) {
    r.pm1 += pm1;
    r.pm25 += pm25;
    r.pm10 += pm10;
  }

  r.validSamples++;
  return true;
}

// ===================== Measurement =====================
void performMeasurementCycle(MeasurementData &finalData) {
  SensorReadings acc;

  float t, h, p;
  bme280.read(t, h, p);

  startAllSensors(p);
  delay(SPS30_WARMUP_SEC * 1000);

  int samples = SAMPLE_DURATION_SEC / SAMPLE_INTERVAL_SEC;

  for (int i = 0; i < samples; i++) {
    takeSingleReading(acc);
    if (i < samples - 1) delay(SAMPLE_INTERVAL_SEC * 1000);
  }

  if (acc.validSamples > 0) {
    finalData.temperature = acc.temperature / acc.validSamples;
    finalData.humidity    = acc.humidity / acc.validSamples;
    finalData.pressure    = acc.pressure / acc.validSamples;
    finalData.co2         = acc.co2 / acc.validSamples;
    finalData.voc         = acc.vocIndex / acc.validSamples;
    finalData.pm1         = acc.pm1 / acc.validSamples;
    finalData.pm25        = acc.pm25 / acc.validSamples;
    finalData.pm10        = acc.pm10 / acc.validSamples;
  }

  stopAllSensors();
}

// ===================== SEND =====================
bool sendData(time_t ts, MeasurementData &data) {
  logDataToFile(ts, data.temperature, data.humidity, data.pressure,
                data.co2, data.voc, data.pm1, data.pm25, data.pm10);

  delay(100);
  simPowerOn();

  if (!connectSIM()) {
    simPowerOff();
    delay(500);
    return false;
  }

  String payload = prepareJSON(DEVICE_ID, ts, data);
  bool ok = sendHTTPSIM(payload);

  disconnectSIM();
  simPowerOff();
  delay(500);

  return ok;
}

// ===================== Sleep =====================
void enterDeepSleep(uint64_t sec) {

  logToSD("[SLEEP] Preparing for deep sleep");

  disconnectSIM();
  delay(200);

  simPowerOff();   // 🔥 CRITICAL
  delay(500);

  esp_sleep_enable_timer_wakeup(sec * 1000000ULL);

#if DEBUG
  Serial.flush();
  delay(100);
#endif

  esp_deep_sleep_start();
}
// ===================== SETUP =====================
void setup() {
  bootCount++;

#if DEBUG
  Serial.begin(115200);
#endif

  initSDCard();
  logToSD("\n===== BOOT #" + String(bootCount) + " =====");

  time_t now = time(nullptr);

  // ===================== STEP 1: TIME SYNC (ONLY IF NEEDED) =====================
  if (!timeIsSynced || now < 100000) {  // invalid time check

    logToSD("[TIME] Sync required");

    simPowerOn();

    if (!connectSIM()) {
      logToSD("[SIM] Failed to connect for time sync");
      simPowerOff();
      enterDeepSleep(60);
    }

    if (!syncTimeSIM()) {
      logToSD("[TIME] Sync failed");
      disconnectSIM();
      simPowerOff();
      enterDeepSleep(60);
    }

    now = time(nullptr);

    timeIsSynced = true;
    lastMeasurementTime = now;   // 🔥 IMPORTANT

    disconnectSIM();
    simPowerOff();
    delay(500);
  }

  // ===================== STEP 2: CALCULATE NEXT MEASUREMENT =====================
  now = time(nullptr);

  uint32_t needed = SPS30_WARMUP_SEC + SAMPLE_DURATION_SEC;

  time_t nextSend = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
  time_t startMeas = nextSend - needed;

  bool shouldMeasure = false;

  if (bootCount == 1) {
    // First boot → measure immediately after sync
    shouldMeasure = true;
    nextSend = now;
  } else {
    shouldMeasure = (now >= startMeas - 30 && now < nextSend);
  }

  // ===================== STEP 3: MEASUREMENT =====================
  if (shouldMeasure) {

    logToSD("[MEASURE] Starting");

    initAllSensors();

    static MeasurementData data;
    performMeasurementCycle(data);

    time_t ts = (bootCount == 1) ? time(nullptr) : nextSend;

    // ===================== STEP 4: SEND =====================
    logToSD("[SEND] Starting");

    simPowerOn();

    bool sent = false;

    if (connectSIM()) {
      sent = sendHTTPSIM(prepareJSON(DEVICE_ID, ts, data));
      disconnectSIM();
    } else {
      logToSD("[SIM] Connect failed (send)");
    }

    if (sent) {
      logToSD("[SEND] Success");
      lastMeasurementTime = ts;
    } else {
      logToSD("[SEND] Failed (data lost)");
      lastMeasurementTime = ts; // still move forward
    }
  }

  // ===================== STEP 5: SLEEP =====================
  now = time(nullptr);

  nextSend = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
  startMeas = nextSend - needed;

  uint64_t sleepSec = (startMeas > now + 10)
                      ? (startMeas - now - 10)
                      : 10;

  logToSD("[SLEEP] " + String((uint32_t)sleepSec) + " sec");

  enterDeepSleep(sleepSec);
}

// ===================== LOOP =====================
void loop() {}