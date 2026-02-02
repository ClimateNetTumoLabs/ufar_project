#include "json_utils.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include "rtc_utils.h"
#include "config.h"

#if DEBUG
  #define DBG(x) Serial.println(x)
  #define DBG2(x,y) Serial.print(x); Serial.println(y)
#else
  #define DBG(x)
  #define DBG2(x,y) Serial.print(x); Serial.println(y)
#endif

String prepareJSON(const char* deviceId, time_t t, MeasurementData data){
  StaticJsonDocument<512> doc;
  doc["device"] = deviceId;
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
  serializeJsonPretty(doc, payload);
  return payload;
}

void sendHTTP(String payload){
  HTTPClient http;
  http.begin(POST_URL);
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(payload);
  DBG2("[HTTP] Status: ", status);
  http.end();
}
