#include <Wire.h>
#include <esp_sleep.h>
#include "config.h"
#include "sensors.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "rtc_utils.h"
#include "json_utils.h"
#include "ota_updater.h"

// ===================== RTC Memory =====================
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
  float temperature = 0;
  float humidity    = 0;
  float pressure    = 0;
  float co2         = 0;
  int32_t vocIndex  = 0;
  float pm1         = 0;
  float pm25        = 0;
  float pm10        = 0;
  int validSamples  = 0;
};

// ===================== Power =====================
void enableI2CPower() {
#if I2C_POWER_PIN >= 0
  pinMode(I2C_POWER_PIN, OUTPUT);
  digitalWrite(I2C_POWER_PIN, HIGH);
#endif
  delay(300);
}

void disableI2CPower() {
#if I2C_POWER_PIN >= 0
  digitalWrite(I2C_POWER_PIN, LOW);
#endif
}

// ===================== Sensors =====================
bool initAllSensors() {
  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeout(1000);
  delay(200);

  bool ok = true;

  if (!bme280.init(0x76)) ok = false;
  if (!scd30.init())      ok = false;
  if (!sgp40.init())      ok = false;
  if (!sps30.init())      ok = false;

  return ok;
}

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

// ===================== Measurement =====================
bool takeSingleReading(SensorReadings &r) {
  float t,h,p;

  if (bme280.read(t,h,p)) {
    r.temperature += t;
    r.humidity    += h;
    r.pressure    += p;
  }

  float co2;
  if (scd30.read(co2)) r.co2 += co2;

  int32_t voc;
  if (sgp40.read(voc, t, h)) r.vocIndex += voc;

  float pm1, pm25, pm10;
  if (sps30.read(pm1, pm25, pm10)) {
    r.pm1  += pm1;
    r.pm25 += pm25;
    r.pm10 += pm10;
  }

  r.validSamples++;
  return true;
}

void performMeasurementCycle(MeasurementData &finalData) {
  SensorReadings acc;

  float t,h,p;
  bme280.read(t,h,p);

  startAllSensors(p);
  delay(SPS30_WARMUP_SEC * 1000);

  int samples = SAMPLE_DURATION_SEC / SAMPLE_INTERVAL_SEC;

  for (int i=0;i<samples;i++) {
    takeSingleReading(acc);
    if (i < samples-1)
      delay(SAMPLE_INTERVAL_SEC * 1000);
  }

  if (acc.validSamples > 0) {
    finalData.temperature = acc.temperature / acc.validSamples;
    finalData.humidity    = acc.humidity    / acc.validSamples;
    finalData.pressure    = acc.pressure    / acc.validSamples;
    finalData.co2         = acc.co2         / acc.validSamples;
    finalData.voc         = acc.vocIndex    / acc.validSamples;
    finalData.pm1         = acc.pm1         / acc.validSamples;
    finalData.pm25        = acc.pm25        / acc.validSamples;
    finalData.pm10        = acc.pm10        / acc.validSamples;
  }

  stopAllSensors();
}

// ===================== Send =====================
bool sendData(time_t ts, MeasurementData &data) {

  logDataToFile(ts, data.temperature, data.humidity, data.pressure,
                data.co2, data.voc, data.pm1, data.pm25, data.pm10);

  if (!connectWiFi()) {
    queueFailedData(ts, data);
    return false;
  }

  String payload = prepareJSON(DEVICE_ID, ts, data);
  bool success = sendHTTP(payload);

  if (!success)
    queueFailedData(ts, data);

  disconnectWiFi();
  return success;
}

// ===================== Deep Sleep =====================
void enterDeepSleep(uint64_t seconds) {

  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);

  disableI2CPower();
  shutdownSD();

  esp_deep_sleep_start();
}

// ===================== Setup =====================
void setup() {

  bootCount++;

  Serial.begin(115200);
  delay(1000);

  initSDCard();
  enableI2CPower();

  // ---- WiFi + Time Sync ----
  if (bootCount == 1 || !timeIsSynced) {
    if (connectWiFi() && syncTime()) {
      timeIsSynced = true;
    } else {
      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      esp_deep_sleep_start();
    }
  }

  configTime(ARMENIA_TZ_OFFSET, ARMENIA_DST_OFFSET,
             "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);

  if (bootCount == 1)
    lastMeasurementTime = now;

  uint32_t needed = SPS30_WARMUP_SEC + SAMPLE_DURATION_SEC;

  time_t nextSend  = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
  time_t startTime = nextSend - needed;

  bool measure = false;
  time_t timestamp = 0;

  if (bootCount == 1) {
    measure = true;
  }
  else if (now >= startTime - 30 && now < nextSend) {
    measure = true;
    timestamp = nextSend;
  }

  if (measure) {

    MeasurementData data;
    initAllSensors();
    performMeasurementCycle(data);

    if (timestamp == 0)
      timestamp = time(nullptr);

    sendData(timestamp, data);
    lastMeasurementTime = timestamp;
  }

  now = time(nullptr);
  nextSend  = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
  startTime = nextSend - needed;

  uint64_t sleepSec;

  if (startTime > now + 10)
    sleepSec = (startTime - now) - 10;
  else
    sleepSec = 10;

  enterDeepSleep(sleepSec);
}

void loop() {
}