#include "sd_logger.h"
#include "json_utils.h"
#include "rtc_utils.h"
#include "config.h"
#include <SD.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <WiFi.h>

// LilyGO T-SIM7000G SD card pins
#define SD_MISO     2
#define SD_MOSI     15
#define SD_SCLK     14
#define SD_CS       13

bool sdInitialized = false;
String logBuffer = "";
const int LOG_BUFFER_SIZE = 1024;

static void appendRaw(const String& line) {
  logBuffer += line + "\n";

  #if DEBUG
  Serial.println(line);
  #endif

  if (logBuffer.length() >= LOG_BUFFER_SIZE) {
    flushSDLog();
  }
}

// ===================== SD init =====================

bool initSDCard() {
  #if DEBUG
  Serial.println("[SD] Initializing SD card...");
  #endif

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    #if DEBUG
    Serial.println("[SD] Card mount failed!");
    #endif
    sdInitialized = false;
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    #if DEBUG
    Serial.println("[SD] No SD card attached!");
    #endif
    sdInitialized = false;
    return false;
  }

  #if DEBUG
  Serial.print("[SD] Card type: ");
  if      (cardType == CARD_MMC)  Serial.println("MMC");
  else if (cardType == CARD_SD)   Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else                            Serial.println("UNKNOWN");
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("[SD] Card size: %lluMB\n", cardSize);
  #endif

  sdInitialized = true;

  if (!SD.exists(SD_LOG_DIR)) {
    SD.mkdir(SD_LOG_DIR);
  }

  logToSD("[SYSTEM] ========== BOOT ==========");
  flushSDLog();

  return true;
}

// ===================== Logging =====================

void logToSD(String message) {
  if (!sdInitialized) {
    #if DEBUG
    Serial.println(message);
    #endif
    return;
  }

  time_t now = time(nullptr);
  appendRaw(timeToStr(now) + " | LOG  | " + message);
}

void flushSDLog() {
  if (!sdInitialized || logBuffer.length() == 0) return;

  File f = SD.open(SD_LOG_FILE, FILE_APPEND);
  if (!f) {
    #if DEBUG
    Serial.println("[SD] Failed to open log file");
    #endif
    return;
  }

  f.print(logBuffer);
  f.close();
  logBuffer = "";
}

// ===================== Combined log: data row =====================

// Writes a human-readable DATA line into the same daily log file.
// Always called regardless of transmission success.
void logDataToFile(time_t timestamp, float temp, float hum, float press,
                   float co2, int32_t voc, float pm1, float pm25, float pm10) {
  if (!sdInitialized) return;

  char row[160];
  snprintf(row, sizeof(row),
    "%s | DATA | temp=%.2f hum=%.2f press=%.2f co2=%.0f voc=%d pm1=%.2f pm2.5=%.2f pm10=%.2f",
    timeToStr(timestamp).c_str(),
    temp, hum, press, co2, voc, pm1, pm25, pm10);

  appendRaw(String(row));
  flushSDLog(); // flush immediately so data line is never lost
}

// ===================== S3 log mirror =====================

// Streams the full log file from SD directly to S3 without loading it into RAM.
// Each upload overwrites the previous key — S3 always holds the latest complete log.
bool uploadLogToS3() {
  if (!sdInitialized) return false;

  flushSDLog(); // ensure buffer is written to disk before reading

  if (!SD.exists(SD_LOG_FILE)) {
    logToSD("[S3-LOG] No log file to upload");
    return false;
  }

  File f = SD.open(SD_LOG_FILE, FILE_READ);
  if (!f) {
    logToSD("[S3-LOG] ERROR: Cannot open log file for reading");
    return false;
  }

  size_t fileSize = f.size();
  if (fileSize == 0) {
    f.close();
    return false;
  }

  String objectKey = "logs/device_" + String(DEVICE_ID) + "_log.txt";
  String url = "https://" + String(S3_BUCKET) + ".s3." + String(S3_REGION) +
               ".amazonaws.com/" + objectKey;

  logToSD("[S3-LOG] Streaming " + String(fileSize) + " bytes to: " + url);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "text/plain");
  http.setTimeout(60000); // 60s — large files need more time

  // Stream directly from SD card — no RAM buffer needed
  int status = http.sendRequest("PUT", &f, fileSize);
  f.close();

  logToSD("[S3-LOG] Response code: " + String(status));

  if (status > 0 && status < 300) {
    logToSD("[S3-LOG] Log uploaded successfully");
    http.end();
    return true;
  } else {
    String errBody = http.getString();
    if (errBody.length() > 0 && errBody.length() < 300) {
      logToSD("[S3-LOG] Error: " + errBody);
    }
    http.end();
    return false;
  }
}

// ===================== Offline retry queue =====================
// Format: one JSON payload per line in SD_QUEUE_FILE.
// Each line is a complete JSON payload ready to POST/PUT as-is.
// On reconnect, flushPendingQueue() replays each line and removes it if sent.

void queueFailedData(time_t timestamp, MeasurementData data) {
  if (!sdInitialized) {
    logToSD("[QUEUE] ERROR: SD not available, measurement lost");
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

  logToSD("[QUEUE] Entry saved (" + String(payload.length()) + " bytes) → " + SD_QUEUE_FILE);
}

bool hasPendingQueue() {
  if (!sdInitialized) return false;
  if (!SD.exists(SD_QUEUE_FILE)) return false;

  File f = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!f) return false;

  bool hasData = (f.size() > 0);
  f.close();
  return hasData;
}

// Reads every line from the queue, tries to send each one (API + S3).
// Entries that succeed are removed; failures are kept for the next attempt.
// Returns true if the queue is now empty.
bool flushPendingQueue() {
  if (!sdInitialized || !SD.exists(SD_QUEUE_FILE)) return true;

  File f = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!f) {
    logToSD("[QUEUE] ERROR: Cannot open queue for reading");
    return false;
  }

  // Read all lines into memory (queue should stay small — one entry per failed cycle)
  std::vector<String> pending;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      pending.push_back(line);
    }
  }
  f.close();

  if (pending.empty()) {
    SD.remove(SD_QUEUE_FILE);
    return true;
  }

  logToSD("[QUEUE] Replaying " + String(pending.size()) + " queued entry/entries...");

  std::vector<String> failed;
  for (auto& payload : pending) {
    bool ok = sendHTTP(payload);

    if (ok) {
      logToSD("[QUEUE] Entry sent successfully");
    } else {
      logToSD("[QUEUE] Entry still failing, keeping for next attempt");
      failed.push_back(payload);
    }
  }

  // Rewrite queue file with only the still-failed entries
  SD.remove(SD_QUEUE_FILE);

  if (!failed.empty()) {
    File fw = SD.open(SD_QUEUE_FILE, FILE_WRITE);
    if (fw) {
      for (auto& line : failed) {
        fw.println(line);
      }
      fw.close();
    }
    logToSD("[QUEUE] " + String(failed.size()) + " entry/entries remain in queue");
    return false;
  }

  logToSD("[QUEUE] All queued entries sent, queue cleared");
  return true;
}


