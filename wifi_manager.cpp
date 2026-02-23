#include "wifi_manager.h"
#include "sd_logger.h"
#include "config.h"
#include <Arduino.h>

bool connectWiFi() {
  logToSD("[WIFI] Connecting to WiFi...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && 
         millis() - startAttempt < WIFI_TIMEOUT_SEC * 1000) {
    delay(500);
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
  logToSD("[WIFI] Disconnecting...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool syncTime() {
  logToSD("[TIME] Syncing NTP (Armenia UTC+4)...");

  // Use multiple servers for reliability
  configTime(ARMENIA_TZ_OFFSET, ARMENIA_DST_OFFSET,
             "pool.ntp.org", "time.nist.gov", "time.google.com");

  // Wait up to 30 seconds — NTP can be slow on first connect
  unsigned long startAttempt = millis();
  while (time(nullptr) < 100000 && millis() - startAttempt < 30000) {
    delay(500);
    #if DEBUG
    Serial.print(".");
    #endif
  }
  #if DEBUG
  Serial.println();
  #endif

  time_t now = time(nullptr);
  if (now < 100000) {
    logToSD("[TIME] ERROR: NTP sync failed after 30s");
    return false;
  }

  logToSD("[TIME] Synced: " + timeToStr(now));
  return true;
}
