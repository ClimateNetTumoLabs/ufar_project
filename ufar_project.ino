#include <Wire.h>
#include <esp_sleep.h>
#include "config.h"
#include "sensors.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "rtc_utils.h"
#include "json_utils.h"
#include "ota_updater.h"
#include <esp_wifi.h>
#include <esp_bt.h>
#include <driver/adc.h>
#include <driver/gpio.h>
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

// ===================== Power Management =====================
void enableI2CPower() {
  #if I2C_POWER_PIN >= 0
  pinMode(I2C_POWER_PIN, OUTPUT);
  digitalWrite(I2C_POWER_PIN, HIGH);
  delay(300);
  #else
  delay(300);
  #endif
}
void disableI2CPower() {
  #if I2C_POWER_PIN >= 0
  digitalWrite(I2C_POWER_PIN, LOW);
  Wire.end();
  pinMode(SDA, INPUT_PULLDOWN);
  pinMode(SCL, INPUT_PULLDOWN);
  #endif
}

void disableModem() {
    // Power on the modem first so it can receive AT commands
    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);
    delay(100);

    // TX=26, RX=27 — matching the board's wiring
    Serial1.begin(115200, SERIAL_8N1, 27, 26); // RX=27, TX=26
    delay(100);

    // Try AT first to check if modem is alive
    Serial1.println("AT");
    delay(500);

    // Clean software power-down
    Serial1.println("AT+CPOWD=1");
    delay(3000);

    Serial1.end();

    // Cut power rail — this kills the status LED too
    digitalWrite(MODEM_POWER_ON, LOW);

    // Pull all interface pins low — floating pins leak current
    // through the modem's internal pull-ups
    for (int pin : {4, 5, 25, 26, 27}) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
}

// ===================== Sensor Initialization =====================
bool initAllSensors() {
  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeout(1000);
  delay(200);

  bool allOk = true;

  delay(50);
  if (!bme280.init()) {
    delay(200);
    if (!bme280.init()) { logToSD("[BME280] ERROR: Init failed"); allOk = false; }
  }

  delay(50);
  if (!scd30.init()) {
    delay(200);
    if (!scd30.init()) { logToSD("[SCD30] ERROR: Init failed"); allOk = false; }
  }

  delay(50);
  if (!sgp40.init()) {
    delay(200);
    if (!sgp40.init()) { logToSD("[SGP40] ERROR: Init failed"); allOk = false; }
  }

  delay(50);
  if (!sps30.init()) {
    delay(200);
    if (!sps30.init()) { logToSD("[SPS30] ERROR: Init failed"); allOk = false; }
  }

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
    if ((i + 1) % 10 == 0) flushSDLog();
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

    // ── 1. Radio off ──────────────────────────────
    // disconnectWiFi() only disconnects — these fully cut the radio hardware
    esp_wifi_stop();
    esp_bt_controller_disable();

    // ── 3. I2C power + SD ─────────────────────────
    disableI2CPower();
    shutdownSD();

    // ── 4. Hold all output pins LOW through sleep ──
    // Without this, non-RTC GPIOs float and leak current into
    // connected peripherals
    const gpio_num_t outputPins[] = {
        GPIO_NUM_4,  // MODEM_PWRKEY
        GPIO_NUM_5,  // MODEM_RST
        GPIO_NUM_12, // netlight LED
        GPIO_NUM_13, // SD MOSI
        GPIO_NUM_14, // SD CLK
        GPIO_NUM_15, // SD CS
        GPIO_NUM_23, // MODEM_POWER_ON
        GPIO_NUM_25, // MODEM_DTR / I2C_POWER
        GPIO_NUM_26, // MODEM_TX
        GPIO_NUM_27, // MODEM_RX
    };
    for (gpio_num_t pin : outputPins) {
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
        gpio_hold_en(pin);
    }
    gpio_deep_sleep_hold_en(); // holds all of the above through sleep

    esp_sleep_enable_timer_wakeup(sleepTimeSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

// ===================== Setup =====================
void setup() {
  gpio_deep_sleep_hold_dis();
  for (int pin : {4, 5, 12, 13, 14, 15, 23, 25, 26, 27})
      gpio_hold_dis((gpio_num_t)pin);

  bootCount++;

  #if DEBUG
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n========== BOOT " + String(bootCount) + " ==========");
  #endif

  // ── 1. SD Card ────────────────────────────────
  if (!initSDCard()) {
    #if DEBUG
    Serial.println("SD card init failed - continuing anyway");
    #endif
  }
  logToSD("[SYSTEM] Boot #" + String(bootCount));

  // Modem is never used - keep it permanently off
  disableModem();

  // ── 2. WiFi ───────────────────────────────────
  enableI2CPower();

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
    flushSDLog();
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
      disconnectWiFi();
      flushSDLog();
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

  // ── 5. WiFi off before sensors ────────────────
  disconnectWiFi();

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
        uploadLogToS3();
      } else {
        lastMeasurementTime = measurementTimestamp;
      }
    } else {
      logToSD("[SEND] No WiFi - queuing data");
      queueFailedData(measurementTimestamp, data);
      lastMeasurementTime = measurementTimestamp;
    }

    disconnectWiFi();

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