#pragma once

/* ================= WIFI ================= */
#define WIFI_SSID ""
#define WIFI_PASS ""
#define WIFI_TIMEOUT_SEC 30

/* ================= DEVICE ================= */
#define DEVICE_ID "" // add only the number
#define DATA_URL "" // API that triggers fromEspToRDS lambda
#define TZ_SOURCE "wifi"   // or "sim"
/* ================= MEASUREMENT INTERVALS ================= */
// Send interval in minutes
#define MEASURE_INTERVAL_MIN 20

// SPS30 warm-up time in seconds
#define SPS30_WARMUP_SEC 30

// Measurement duration in seconds
#define SAMPLE_DURATION_SEC 120

// Interval between samples during measurement
#define SAMPLE_INTERVAL_SEC 2

/* ================= TIMEZONE ================= */
// Armenia UTC+4
#define ARMENIA_TZ_OFFSET  (4 * 3600)
#define ARMENIA_DST_OFFSET 0

/* ================= POWER MANAGEMENT ================= */
// LilyGO T-SIM7000G I2C Power Control Pin (if your board has one)
// Set to -1 if you don't have a dedicated I2C power control pin
#define I2C_POWER_PIN 25

// Modem power pins (disabled to save power, not using SIM)
// T-SIM7000G modem pins
#define MODEM_PWRKEY     4
#define MODEM_TX     27
#define MODEM_RX     26
#define SD_MISO     2
#define SD_MOSI     15
#define SD_SCLK     14
#define SD_CS       13

/* ================= OTA ================= */
// Raw URL to version.json in your GitHub repo, e.g.:
// https://raw.githubusercontent.com/YOUR_USER/YOUR_REPO/main/firmware/version.json
#define OTA_MANIFEST_URL ""
// Bump FIRMWARE_VERSION in ota_updater.h before each release

/* ================= SD CARD ================= */
#define SD_LOG_DIR      "/ufar_project"
// Single combined log file — all logs and data rows, appended forever
#define SD_LOG_FILE     "/ufar_project/device_" DEVICE_ID "_log.txt"
// Pending queue: one JSON payload per line, retried when connectivity returns
#define SD_QUEUE_FILE   "/ufar_project/pending_queue.txt"

