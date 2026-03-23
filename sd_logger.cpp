#include "sd_logger.h"
#include "json_utils.h"
#include "rtc_utils.h"
#include "config.h"

#include <SD.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <WiFi.h>

bool sdInitialized = false;

/* ===================== SD INIT ===================== */

bool initSDCard() {

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    sdInitialized = false;
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    sdInitialized = false;
    return false;
  }

  sdInitialized = true;

  if (!SD.exists(SD_LOG_DIR)) {
    SD.mkdir(SD_LOG_DIR);
  }

  logToSD("[SYSTEM] ========== BOOT ==========");

  return true;
}

/* ===================== SINGLE LOG METHOD ===================== */

void logToSD(String message) {

  time_t now = time(nullptr);

  String line = timeToStr(now) + " | LOG  | " + message;

  Serial.println(line);

  if (!sdInitialized) return;

  File f = SD.open(SD_LOG_FILE, FILE_APPEND);

  if (!f) return;

  f.println(line);
  f.close();
}

/* ===================== DATA ROW ===================== */

void logDataToFile(time_t timestamp,
                   float temp,
                   float hum,
                   float press,
                   float co2,
                   int32_t voc,
                   float pm1,
                   float pm25,
                   float pm10) {

  char row[160];

  snprintf(row, sizeof(row),
    "%s | DATA | temp=%.2f hum=%.2f press=%.2f co2=%.0f voc=%d pm1=%.2f pm2.5=%.2f pm10=%.2f",
    timeToStr(timestamp).c_str(),
    temp, hum, press, co2, voc, pm1, pm25, pm10
  );

  logToSD(String(row));
}

/* ===================== OFFLINE QUEUE ===================== */

void queueFailedData(time_t timestamp, MeasurementData data) {

  if (!sdInitialized) {
    logToSD("[QUEUE] ERROR: SD not available");
    return;
  }

  String payload = prepareJSON(DEVICE_ID, timestamp, data);

  File f = SD.open(SD_QUEUE_FILE, FILE_APPEND);

  if (!f) {
    logToSD("[QUEUE] ERROR: Cannot open queue file");
    return;
  }

  f.println(payload);
  f.close();

  logToSD("[QUEUE] Entry saved (" + String(payload.length()) + " bytes)");
}

/* ===================== CHECK QUEUE ===================== */

bool hasPendingQueue() {

  if (!sdInitialized) return false;
  if (!SD.exists(SD_QUEUE_FILE)) return false;

  File f = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!f) return false;

  bool hasData = f.size() > 0;

  f.close();

  return hasData;
}

/* ===================== FLUSH QUEUE ===================== */

bool flushPendingQueue() {

  if (!sdInitialized || !SD.exists(SD_QUEUE_FILE))
    return true;

  File f = SD.open(SD_QUEUE_FILE, FILE_READ);

  if (!f) {
    logToSD("[QUEUE] ERROR: Cannot open queue");
    return false;
  }

  std::vector<String> pending;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() > 0)
      pending.push_back(line);
  }

  f.close();

  if (pending.empty()) {
    SD.remove(SD_QUEUE_FILE);
    return true;
  }

  logToSD("[QUEUE] Replaying " + String(pending.size()) + " entries");

  std::vector<String> failed;

  for (auto& payload : pending) {

    bool ok = sendHTTP(payload);

    if (!ok) {
      failed.push_back(payload);
      logToSD("[QUEUE] Entry failed again");
    } else {
      logToSD("[QUEUE] Entry sent successfully");
    }
  }

  SD.remove(SD_QUEUE_FILE);

  if (!failed.empty()) {

    File fw = SD.open(SD_QUEUE_FILE, FILE_WRITE);

    if (fw) {
      for (auto& line : failed)
        fw.println(line);

      fw.close();
    }

    logToSD("[QUEUE] " + String(failed.size()) + " entries remain");

    return false;
  }

  logToSD("[QUEUE] Queue cleared");

  return true;
}