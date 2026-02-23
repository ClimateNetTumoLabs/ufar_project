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

// ===================== Power Management =====================
void enableI2CPower() {
  #if I2C_POWER_PIN >= 0
  pinMode(I2C_POWER_PIN, OUTPUT);
  digitalWrite(I2C_POWER_PIN, HIGH);
  delay(300); // Longer delay for voltage to stabilize and sensors to boot
  logToSD("[POWER] I2C power enabled via pin " + String(I2C_POWER_PIN));
  #else
  logToSD("[POWER] No I2C power control pin configured");
  delay(300); // Still wait for sensors
  #endif
}

void disableI2CPower() {
  #if I2C_POWER_PIN >= 0
  digitalWrite(I2C_POWER_PIN, LOW);
  logToSD("[POWER] I2C power disabled");
  #endif
}

void disableModem() {
  // Turn off modem to save power
  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  digitalWrite(MODEM_POWER_ON, LOW);
  logToSD("[POWER] Modem disabled");
}

// ===================== I2C Utilities =====================
void scanI2CBus() {
  logToSD("[I2C] Scanning bus for devices...");
  
  int devicesFound = 0;
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    
    if (error == 0) {
      String msg = "[I2C] Device found at 0x" + String(address, HEX);
      logToSD(msg);
      devicesFound++;
    }
  }
  
  if (devicesFound == 0) {
    logToSD("[I2C] WARNING: No devices found on bus!");
  } else {
    logToSD("[I2C] Found " + String(devicesFound) + " device(s)");
  }
}

// ===================== Sensor Initialization =====================
bool initAllSensors() {
  logToSD("[SENSORS] Initializing all sensors...");
  
  // Initialize I2C with longer timeout and error recovery
  Wire.begin();
  Wire.setClock(100000); // 100kHz for stability
  Wire.setTimeout(1000); // 1 second timeout
  delay(200); // Longer delay for sensors to stabilize
  
  bool allOk = true;
  
  // BME280
  delay(50);
  if (bme280.init(0x76)) {
    logToSD("[BME280] Initialized successfully");
  } else {
    logToSD("[BME280] ERROR: Initialization failed");
    // Try once more after delay
    delay(200);
    if (bme280.init(0x76)) {
      logToSD("[BME280] Initialized on retry");
    } else {
      logToSD("[BME280] FAILED after retry");
      allOk = false;
    }
  }
  
  // SCD30
  delay(50);
  if (scd30.init()) {
    logToSD("[SCD30] Initialized successfully");
  } else {
    logToSD("[SCD30] ERROR: Initialization failed");
    delay(200);
    if (scd30.init()) {
      logToSD("[SCD30] Initialized on retry");
    } else {
      logToSD("[SCD30] FAILED after retry");
      allOk = false;
    }
  }
  
  // SGP40
  delay(50);
  if (sgp40.init()) {
    logToSD("[SGP40] Initialized successfully");
  } else {
    logToSD("[SGP40] ERROR: Initialization failed");
    delay(200);
    if (sgp40.init()) {
      logToSD("[SGP40] Initialized on retry");
    } else {
      logToSD("[SGP40] FAILED after retry");
      allOk = false;
    }
  }
  
  // SPS30
  delay(50);
  if (sps30.init()) {
    logToSD("[SPS30] Initialized successfully");
  } else {
    logToSD("[SPS30] ERROR: Initialization failed");
    delay(200);
    if (sps30.init()) {
      logToSD("[SPS30] Initialized on retry");
    } else {
      logToSD("[SPS30] FAILED after retry");
      allOk = false;
    }
  }
  
  if (!allOk) {
    logToSD("[SENSORS] WARNING: Some sensors failed - continuing with available sensors");
  }
  
  return allOk;
}

// ===================== Sensor Start/Stop =====================
void startAllSensors(float pressure_hPa) {
  logToSD("[SENSORS] Starting all sensors...");
  
  bme280.start();
  logToSD("[BME280] Started");
  
  scd30.start((uint16_t)pressure_hPa);
  logToSD("[SCD30] Started with pressure compensation: " + String((int)pressure_hPa) + " hPa");
  
  sgp40.start();
  logToSD("[SGP40] Started");
  
  // SPS30 requires warm-up time
  logToSD("[SPS30] Starting fan...");
  if (sps30.start()) {
    logToSD("[SPS30] Fan started, warming up for " + String(SPS30_WARMUP_SEC) + " seconds");
  } else {
    logToSD("[SPS30] ERROR: Failed to start");
  }
}

void stopAllSensors() {
  logToSD("[SENSORS] Stopping all sensors...");
  
  bme280.sleep();
  scd30.sleep();
  sgp40.sleep();
  sps30.sleep();
  
  logToSD("[SENSORS] All sensors in sleep mode");
}

// ===================== Single Reading =====================
bool takeSingleReading(SensorReadings &reading) {
  float temp, hum, press;
  
  // BME280 - always read first for T/H compensation
  if (bme280.read(temp, hum, press)) {
    reading.temperature += temp;
    reading.humidity += hum;
    reading.pressure += press;
  } else {
    logToSD("[BME280] ERROR: Read failed");
  }
  
  // SCD30
  float co2;
  if (scd30.read(co2)) {
    reading.co2 += co2;
  } else {
    logToSD("[SCD30] WARNING: Data not ready");
  }
  
  // SGP40
  int32_t voc;
  if (sgp40.read(voc, temp, hum)) {
    reading.vocIndex += voc;
  } else {
    logToSD("[SGP40] ERROR: Read failed");
  }
  
  // SPS30
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
  logToSD("[MEASURE] ========== Starting Measurement Cycle ==========");
  
  SensorReadings accumulated;
  
  // Get initial pressure for SCD30 compensation
  float temp, hum, press;
  bme280.read(temp, hum, press);
  
  // Start all sensors
  startAllSensors(press);
  
  // SPS30 warm-up
  logToSD("[MEASURE] SPS30 warming up...");
  delay(SPS30_WARMUP_SEC * 1000);
  
  // Calculate number of samples
  int numSamples = SAMPLE_DURATION_SEC / SAMPLE_INTERVAL_SEC;
  logToSD("[MEASURE] Taking " + String(numSamples) + " samples over " + String(SAMPLE_DURATION_SEC) + " seconds");
  
  // Take multiple samples
  for (int i = 0; i < numSamples; i++) {    
    takeSingleReading(accumulated);
    
    if (i < numSamples - 1) {
      delay(SAMPLE_INTERVAL_SEC * 1000);
    }
    
    // Flush logs periodically
    if ((i + 1) % 10 == 0) {
      flushSDLog();
    }
  }
  
  // Average the readings
  if (accumulated.validSamples > 0) {
    finalData.temperature = accumulated.temperature / accumulated.validSamples;
    finalData.humidity = accumulated.humidity / accumulated.validSamples;
    finalData.pressure = accumulated.pressure / accumulated.validSamples;
    finalData.co2 = accumulated.co2 / accumulated.validSamples;
    finalData.voc = accumulated.vocIndex / accumulated.validSamples;
    finalData.pm1 = accumulated.pm1 / accumulated.validSamples;
    finalData.pm25 = accumulated.pm25 / accumulated.validSamples;
    finalData.pm10 = accumulated.pm10 / accumulated.validSamples;
    
    logToSD("[MEASURE] Averaged data from " + String(accumulated.validSamples) + " samples");
    logToSD("[MEASURE] Final: T=" + String(finalData.temperature, 1) + "°C, H=" + String(finalData.humidity, 1) + 
            "%, P=" + String(finalData.pressure, 1) + "hPa, CO2=" + String((int)finalData.co2) + 
            "ppm, VOC=" + String(finalData.voc) + ", PM2.5=" + String(finalData.pm25, 2) + "µg/m³");
  } else {
    logToSD("[MEASURE] ERROR: No valid samples collected");
  }
  
  // Stop all sensors
  stopAllSensors();
  
  logToSD("[MEASURE] ========== Measurement Cycle Complete ==========");
}

// ===================== Data Transmission =====================
// Returns true if the API accepted the data.
// NOTE: does NOT disconnect WiFi — caller must do that after uploadLogToS3().
bool sendData(time_t timestamp, MeasurementData &data) {
  logToSD("[SEND] ========== Starting Data Transmission ==========");

  // Always log the data reading to the log file, regardless of outcome
  logDataToFile(timestamp, data.temperature, data.humidity, data.pressure,
                data.co2, data.voc, data.pm1, data.pm25, data.pm10);

  // Connect WiFi
  if (!connectWiFi()) {
    logToSD("[SEND] ERROR: WiFi connection failed - queuing data for retry");
    queueFailedData(timestamp, data);
    return false;
  }

  // Prepare and send JSON to API
  String payload = prepareJSON(DEVICE_ID, timestamp, data);

  #if DEBUG
  logToSD("[SEND] JSON: " + payload);
  #endif

  bool success = sendHTTP(payload);

  if (success) {
    logToSD("[SEND] API transmission successful");
  } else {
    logToSD("[SEND] WARNING: API transmission failed - queuing data for retry");
    queueFailedData(timestamp, data);
  }

  logToSD("[SEND] ========== Transmission Complete ==========");
  return success;
}


// ===================== Deep Sleep =====================
void enterDeepSleep(uint64_t sleepTimeSeconds) {
  logToSD("[SLEEP] Entering deep sleep for " + String((uint32_t)sleepTimeSeconds) + " seconds");
  logToSD("[SLEEP] Next wake: " + timeToStr(time(nullptr) + sleepTimeSeconds));
  
  flushSDLog(); // Make sure all logs are written
  
  // Configure wake-up
  esp_sleep_enable_timer_wakeup(sleepTimeSeconds * 1000000ULL);
  
  // Power down peripherals
  disableI2CPower();
  
  // Enter deep sleep
  esp_deep_sleep_start();
}

// ===================== Setup =====================
void setup() {
  bootCount++;
  
  #if DEBUG
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n========== BOOT " + String(bootCount) + " ==========");
  #endif
  
  // Initialize SD card first
  if (!initSDCard()) {
    #if DEBUG
    Serial.println("SD card init failed - continuing anyway");
    #endif
  }
  
  logToSD("[SYSTEM] ========== BOOT #" + String(bootCount) + " ==========");
  logToSD("[SYSTEM] Wake-up reason: " + String(esp_sleep_get_wakeup_cause()));
  
  // Disable modem to save power (not using SIM card)
  disableModem();
  
  // Enable I2C power
  enableI2CPower();
  
  // ALWAYS connect WiFi first (needed for time sync and/or data transmission)
  logToSD("[SYSTEM] Connecting to WiFi...");

  // On first boot (or after losing time), we MUST get valid NTP before proceeding.
  // Retry up to 3 times: reconnect WiFi + re-request NTP each attempt.
  if (bootCount == 1 || !timeIsSynced) {
    bool synced = false;
    for (int attempt = 1; attempt <= 3 && !synced; attempt++) {
      logToSD("[SYSTEM] NTP sync attempt " + String(attempt) + "/3...");

      if (WiFi.status() != WL_CONNECTED) {
        if (!connectWiFi()) {
          logToSD("[SYSTEM] WiFi failed on attempt " + String(attempt));
          delay(2000);
          continue;
        }
      }

      if (syncTime()) {
        synced = true;
        timeIsSynced = true;
      } else {
        logToSD("[SYSTEM] NTP failed, reconnecting WiFi and retrying...");
        disconnectWiFi();
        delay(3000);
      }
    }

    if (!synced) {
      logToSD("[SYSTEM] CRITICAL: NTP sync failed after 3 attempts - sleeping 60s and rebooting");
      flushSDLog();
      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      esp_deep_sleep_start();
      // Never reaches here
    }
  } else {
    // Time already synced from a previous boot - just connect WiFi
    if (!connectWiFi()) {
      logToSD("[SYSTEM] ERROR: WiFi connection failed on boot");
    }
  }
  
  // CRITICAL: timezone must be re-applied on every boot (doesn't persist through deep sleep).
  // If we just synced above, configTime was already called inside syncTime().
  // Call it again here to guarantee timezone is set even on non-first boots.
  configTime(ARMENIA_TZ_OFFSET, ARMENIA_DST_OFFSET,
             "pool.ntp.org", "time.nist.gov", "time.google.com");
  delay(100);

  // Get current time (guaranteed valid — we hard-rebooted above if NTP failed)
  time_t now = time(nullptr);

  // On first boot, anchor lastMeasurementTime so scheduling starts from now
  if (bootCount == 1) {
    lastMeasurementTime = now;
    logToSD("[SYSTEM] First boot - time anchored: " + timeToStr(now));
  } else {
    logToSD("[SYSTEM] Current time: " + timeToStr(now));
  }

  // Now that time is valid, flush any queued measurements from previous failures.
  // WiFi is still connected at this point.
  if (WiFi.status() == WL_CONNECTED && hasPendingQueue()) {
    logToSD("[SYSTEM] Pending queue found - flushing offline data...");
    flushPendingQueue();
  }

  // Check for OTA firmware update while WiFi is up.
  // If an update is applied the device reboots automatically inside checkAndApplyOTA().
  if (WiFi.status() == WL_CONNECTED) {
    checkAndApplyOTA();
  }
  
  // Calculate next SEND time (on the interval boundary)
  time_t nextSendTime = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
  
  // Calculate when to START measurement (must finish before send time)
  // Total time needed = SPS30_WARMUP + SAMPLE_DURATION
  uint32_t measurementTimeNeeded = SPS30_WARMUP_SEC + SAMPLE_DURATION_SEC;
  time_t startMeasurementTime = nextSendTime - measurementTimeNeeded;
  
  logToSD("[SYSTEM] Next send time: " + timeToStr(nextSendTime));
  logToSD("[SYSTEM] Should start measuring at: " + timeToStr(startMeasurementTime));

  // ---- Decide what to do this boot ----

  bool shouldMeasure = false;
  time_t measurementTimestamp;

  if (bootCount == 1) {
    // First boot: measure immediately, timestamp = now rounded to interval
    // This gives a clean first reading without waiting up to one full interval
    shouldMeasure = true;
    measurementTimestamp = nextSendTime; // use next interval boundary as timestamp
    logToSD("[SYSTEM] First boot - measuring immediately");
  } else if (now >= startMeasurementTime - 30 && now < nextSendTime) {
    // Normal wake: inside the measurement window for the upcoming send slot
    shouldMeasure = true;
    measurementTimestamp = nextSendTime;
    logToSD("[SYSTEM] Time to start measurement sequence");
  } else if (now >= nextSendTime) {
    // Woke up too late — missed the window, skip and reschedule
    logToSD("[SYSTEM] WARNING: Woke up too late, skipping this measurement cycle");
    disconnectWiFi();
    lastMeasurementTime = now;
    nextSendTime = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
    startMeasurementTime = nextSendTime - measurementTimeNeeded;
  } else {
    logToSD("[SYSTEM] Not time to measure yet (next start: " + timeToStr(startMeasurementTime) + ")");
    disconnectWiFi();
  }

  if (shouldMeasure) {
    // Disconnect WiFi during measurement to save power
    disconnectWiFi();

    #if DEBUG
    scanI2CBus();
    #endif

    if (!initAllSensors()) {
      logToSD("[SYSTEM] WARNING: Some sensors failed to initialize");
    }

    MeasurementData data;
    performMeasurementCycle(data);

    logToSD("[SYSTEM] Using scheduled timestamp: " + timeToStr(measurementTimestamp));

    // sendData reconnects WiFi internally; do NOT disconnect after it returns
    // so that uploadLogToS3 can reuse the same connection
    bool sent = sendData(measurementTimestamp, data);

    if (sent) {
      lastMeasurementTime = measurementTimestamp;
      logToSD("[SYSTEM] Measurement cycle complete and data sent");
      // Upload the full daily log (logs + data) to S3 while WiFi is still up
      uploadLogToS3();
    } else {
      logToSD("[SYSTEM] Transmission failed - data queued for retry on next boot");
      lastMeasurementTime = measurementTimestamp;
    }

    // Disconnect WiFi now that all uploads (including log) are done
    disconnectWiFi();

    // Recalculate for sleep
    now = time(nullptr);
    nextSendTime = calculateNextSend(now, lastMeasurementTime, MEASURE_INTERVAL_MIN);
    startMeasurementTime = nextSendTime - measurementTimeNeeded;
  }
  
  // Calculate sleep duration until next measurement START time
  now = time(nullptr);
  uint64_t sleepSeconds;

  if (startMeasurementTime > now + 10) {
    sleepSeconds = (startMeasurementTime - now) - 10; // wake 10s early for boot overhead
  } else {
    // startMeasurementTime is in the past or imminent — wake immediately
    // (next boot will fall into the measurement window check)
    sleepSeconds = 10;
  }
  
  enterDeepSleep(sleepSeconds);
}

// ===================== Loop =====================
void loop() {
  // Never reached - ESP is always in deep sleep between measurements
}
