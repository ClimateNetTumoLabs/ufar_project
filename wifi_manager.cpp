#include "wifi_manager.h"
#include "config.h"

#if DEBUG
  #define DBG(x) Serial.println(x)
  #define DBG2(x,y) Serial.print(x); Serial.println(y)
#else
  #define DBG(x)
  #define DBG2(x,y)
#endif

void connectWiFi() {
  DBG("[WIFI] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  DBG2("[WIFI] IP: ", WiFi.localIP());
}

void syncTime() {
  DBG("[TIME] Syncing NTP (Armenia time)");
  configTime(ARMENIA_TZ_OFFSET, ARMENIA_DST_OFFSET, "pool.ntp.org", "time.nist.gov");
  while (time(nullptr) < 100000) delay(500);
}
