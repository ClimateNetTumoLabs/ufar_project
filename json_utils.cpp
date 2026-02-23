#include "json_utils.h"
#include "sd_logger.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include "rtc_utils.h"
#include "config.h"

// ===================== JSON / HTTP =====================

String prepareJSON(const char* deviceId, time_t t, MeasurementData data) {
  StaticJsonDocument<512> doc;
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "device%s", deviceId);
  doc["device"] = buffer;
  JsonObject d = doc.createNestedArray("data").createNestedObject();
  d["time"] = timeToStr(t);
  d["temperature"] = data.temperature;
  d["humidity"] = data.humidity;
  d["pressure"] = data.pressure;
  d["pm1"] = data.pm1;
  d["pm2_5"] = data.pm25;
  d["pm10"] = data.pm10;
  d["co2"] = data.co2;
  d["voc"] = data.voc;

  String payload;
  serializeJson(doc, payload);

  logToSD("[JSON] Payload prepared (" + String(payload.length()) + " bytes)");

  return payload;
}

bool sendHTTP(String payload) {
  logToSD("[HTTP] Sending to: " + String(POST_URL));

  HTTPClient http;
  http.begin(POST_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int status = http.POST(payload);

  logToSD("[HTTP] Response code: " + String(status));

  if (status > 0) {
    String response = http.getString();
    if (response.length() > 0 && response.length() < 200) {
      logToSD("[HTTP] Response: " + response);
    }
  } else {
    logToSD("[HTTP] ERROR: " + http.errorToString(status));
  }

  http.end();

  return (status == 200 || status == 201);
}
