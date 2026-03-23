#include "wifi_manager.h"
#include "sd_logger.h"
#include "config.h"
#include <esp_wifi.h>
#include <Arduino.h>

bool connectWiFi() {
  logToSD("[WIFI] Connecting to WiFi...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startAttempt = millis();
  unsigned long lastCheck = 0;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < WIFI_TIMEOUT_SEC * 1000) {

    if (millis() - lastCheck >= 500) {
      lastCheck = millis();
      // optional debug or watchdog feed
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    logToSD("[WIFI] Connected! IP: " + WiFi.localIP().toString());
    return true;
  } else {
    logToSD("[WIFI] ERROR: Connection timeout");
    return false;
  }
}

void disconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_OFF) {
    logToSD("[WIFI] Disconnecting...");
    esp_wifi_deinit();
  }
}

bool syncTime() {
  logToSD("[TIME] Syncing NTP (Armenia UTC+4)...");

  configTime(ARMENIA_TZ_OFFSET, ARMENIA_DST_OFFSET,
             "pool.ntp.org", "time.nist.gov", "time.google.com");

  unsigned long startAttempt = millis();
  unsigned long lastPrint = 0;

  while (time(nullptr) < 100000 && millis() - startAttempt < 30000) {

    if (millis() - lastPrint >= 500) {
      lastPrint = millis();
    }
  }

  time_t now = time(nullptr);

  if (now < 100000) {
    logToSD("[TIME] ERROR: NTP sync failed after 30s");
    return false;
  }

  logToSD("[TIME] Synced: " + timeToStr(now));
  return true;
}