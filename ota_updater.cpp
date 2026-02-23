#include "ota_updater.h"
#include "sd_logger.h"
#include "config.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ===================== Version comparison =====================
// Compares two semantic version strings "X.Y.Z".
// Returns true if `remote` is strictly newer than `local`.
static bool isNewerVersion(const char* local, const char* remote) {
  int lMaj = 0, lMin = 0, lPat = 0;
  int rMaj = 0, rMin = 0, rPat = 0;
  sscanf(local,  "%d.%d.%d", &lMaj, &lMin, &lPat);
  sscanf(remote, "%d.%d.%d", &rMaj, &rMin, &rPat);

  if (rMaj != lMaj) return rMaj > lMaj;
  if (rMin != lMin) return rMin > lMin;
  return rPat > lPat;
}

// ===================== OTA progress callback =====================
static void otaProgressCallback(int current, int total) {
  static int lastPct = -1;
  int pct = (total > 0) ? (current * 100 / total) : 0;
  if (pct != lastPct && pct % 10 == 0) {
    logToSD("[OTA] Progress: " + String(pct) + "% (" + String(current) + "/" + String(total) + " bytes)");
    lastPct = pct;
  }
}

// ===================== Main OTA function =====================
bool checkAndApplyOTA() {
  logToSD("[OTA] Current firmware: v" + String(FIRMWARE_VERSION));
  logToSD("[OTA] Checking for update at: " + String(OTA_MANIFEST_URL));

  // ---- Step 1: Fetch version manifest ----
  WiFiClientSecure manifestClient;
  manifestClient.setInsecure(); // GitHub uses HTTPS; skip cert validation on ESP32
  manifestClient.setTimeout(10);

  HTTPClient http;
  http.begin(manifestClient, OTA_MANIFEST_URL);
  http.setTimeout(10000);
  // Follow GitHub raw redirects
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  logToSD("[OTA] Manifest response code: " + String(code));

  if (code != 200) {
    logToSD("[OTA] Failed to fetch manifest, skipping update");
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  // ---- Step 2: Parse manifest JSON ----
  // Expected format:
  // { "version": "1.2.0", "url": "https://github.com/.../firmware.bin" }
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    logToSD("[OTA] ERROR: Failed to parse manifest JSON: " + String(err.c_str()));
    return false;
  }

  const char* remoteVersion = doc["version"] | "";
  const char* binUrl        = doc["url"]     | "";

  if (strlen(remoteVersion) == 0 || strlen(binUrl) == 0) {
    logToSD("[OTA] ERROR: Manifest missing version or url field");
    return false;
  }

  logToSD("[OTA] Remote version: v" + String(remoteVersion));

  // ---- Step 3: Compare versions ----
  if (!isNewerVersion(FIRMWARE_VERSION, remoteVersion)) {
    logToSD("[OTA] Already up to date (v" + String(FIRMWARE_VERSION) + ")");
    return false;
  }

  logToSD("[OTA] New version available: v" + String(remoteVersion) + " — starting download");
  logToSD("[OTA] Binary URL: " + String(binUrl));

  // ---- Step 4: Download and flash ----
  WiFiClientSecure binClient;
  binClient.setInsecure();
  binClient.setTimeout(60); // 60s socket timeout for large binary

  httpUpdate.onProgress(otaProgressCallback);
  httpUpdate.rebootOnUpdate(false); // We'll reboot manually after logging

  t_httpUpdate_return result = httpUpdate.update(binClient, binUrl);

  switch (result) {
    case HTTP_UPDATE_OK:
      logToSD("[OTA] Update successful! Rebooting to v" + String(remoteVersion) + "...");
      flushSDLog();
      delay(500);
      ESP.restart();
      return true; // never reached but satisfies compiler

    case HTTP_UPDATE_NO_UPDATES:
      logToSD("[OTA] Server reports no update available");
      return false;

    case HTTP_UPDATE_FAILED:
      logToSD("[OTA] ERROR: Update failed — " + httpUpdate.getLastErrorString());
      return false;

    default:
      logToSD("[OTA] ERROR: Unknown update result: " + String(result));
      return false;
  }
}
