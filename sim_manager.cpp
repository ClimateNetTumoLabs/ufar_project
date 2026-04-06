#include "sim_manager.h"
#include "sd_logger.h"
#include "config.h"

// ── TinyGSM setup ────────────────────────────────────────────────────────────
#define TINY_GSM_MODEM_SIM7000SSL
#include <TinyGsmClient.h>
#include <time.h>

#ifndef MODEM_APN
  #define MODEM_APN "internet.beeline.am"
#endif

// ── Internal objects ─────────────────────────────────────────────────────────
static HardwareSerial       SerialAT(1);
static TinyGsm              modem(SerialAT);
static TinyGsmClientSecure  secureClient(modem);

// ── AT helpers ───────────────────────────────────────────────────────────────

static String readATResponse(uint32_t timeout = 5000) {
  uint32_t start = millis();
  String response;
  while (millis() - start < timeout) {
    while (SerialAT.available()) response += (char)SerialAT.read();
    if (response.indexOf("OK")    != -1) break;
    if (response.indexOf("ERROR") != -1) break;
    delay(10);
  }
  return response;
}

static bool sendATCommand(const String& cmd, uint32_t timeout = 5000) {
  SerialAT.println(cmd);
  String r = readATResponse(timeout);
  logToSD("[SIM] AT: " + cmd + " -> " + r.substring(0, min((int)r.length(), 40)));
  return r.indexOf("OK") != -1;
}

// ── Parse AT+CCLK? response into a time_t ────────────────────────────────────
// SIM7000 format: "+CCLK: \"YY/MM/DD,HH:MM:SS±TZ\""
static time_t parseCCLK(const String& response) {
  int idx = response.indexOf("+CCLK: \"");
  if (idx == -1) return 0;
  idx += 8;
  int end = response.indexOf("\"", idx);
  if (end == -1) return 0;

  String raw = response.substring(idx, end);  // e.g. "26/03/27,14:35:12+16"
  if (raw.length() < 17) return 0;

  int yy = raw.substring(0,  2).toInt();
  int mo = raw.substring(3,  5).toInt();
  int dd = raw.substring(6,  8).toInt();
  int hh = raw.substring(9,  11).toInt();
  int mi = raw.substring(12, 14).toInt();
  int ss = raw.substring(15, 17).toInt();

  // Reject SIMCom default epoch (1980-01-06) and uninitialized zero
  if (yy == 80 || yy == 0) return 0;

  // Quarter-hour TZ offset: e.g. "+16" → UTC+4h (16 × 15 min)
  int tzQuarters = 0;
  if (raw.length() > 17) {
    char sign   = raw.charAt(17);
    tzQuarters  = raw.substring(18).toInt();
    if (sign == '-') tzQuarters = -tzQuarters;
  }

  struct tm t = {};
  t.tm_year = (2000 + yy) - 1900;
  t.tm_mon  = mo - 1;
  t.tm_mday = dd;
  t.tm_hour = hh;
  t.tm_min  = mi;
  t.tm_sec  = ss;

  // mktime treats tm as local; subtract TZ offset to get UTC
  time_t epoch = mktime(&t) - (tzQuarters * 15 * 60);
  return epoch;
}

// ============================================================================
// Public API
// ============================================================================

void simPowerOn() {
  logToSD("[SIM] Powering on modem...");

  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);

  // PWRKEY pulse: HIGH for 1s then LOW, then wait for boot
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(5000);  // SIM7000 boot time

  // Wait until the modem responds to AT (up to 10s)
  uint32_t start = millis();
  bool atOk = false;
  while (millis() - start < 10000) {
    SerialAT.println("AT");
    String r = readATResponse(1000);
    if (r.indexOf("OK") != -1) { atOk = true; break; }
    delay(500);
  }

  if (!atOk) {
    logToSD("[SIM] WARNING: Modem did not respond to AT after power on");
  }

  modem.init();

  // Lock to LTE Cat-M / NB-IoT only (13 = LTE only on SIM7000)
  modem.sendAT("+CNMP=13");
  modem.waitResponse(3000);

  // Enable network time sync and persist it
  sendATCommand("AT+CLTS=1", 3000);
  sendATCommand("AT&W",      3000);

  logToSD("[SIM] Modem ready");
}

void simPowerOff() {
  logToSD("[SIM] Powering off modem...");

  // Cleanly drop GPRS before killing power
  modem.gprsDisconnect();
  delay(500);

  // PWRKEY pulse: HIGH for 2s (SIM7000 datasheet: min 1.2s, use 2s for margin)
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(2000);
  digitalWrite(MODEM_PWRKEY, LOW);

  // Wait for module to fully shut down before the caller does anything else
  delay(3000);

  logToSD("[SIM] Modem off");
}

bool connectSIM() {
  logToSD("[SIM] Waiting for network...");
  if (!modem.waitForNetwork(60000)) {
    logToSD("[SIM] ERROR: Network registration failed");
    return false;
  }

  logToSD("[SIM] Connecting GPRS (APN: " + String(MODEM_APN) + ")...");
  if (!modem.gprsConnect(MODEM_APN, "", "")) {
    logToSD("[SIM] ERROR: GPRS connect failed");
    return false;
  }

  logToSD("[SIM] Connected. IP: " + modem.localIP().toString());
  return true;
}

void disconnectSIM() {
  modem.gprsDisconnect();
  logToSD("[SIM] GPRS disconnected");
}

bool syncTimeSIM() {
  logToSD("[SIM] Syncing time via AT+CCLK...");

  uint32_t deadline = millis() + 90000;
  while (millis() < deadline) {
    SerialAT.println("AT+CCLK?");
    String response = readATResponse(5000);
    time_t epoch = parseCCLK(response);

    if (epoch > 0) {
      struct timeval tv = { epoch, 0 };
      settimeofday(&tv, nullptr);
      logToSD("[TIME] Synced: " + String(ctime(&epoch)));
      return true;
    }

    logToSD("[SIM] Time not ready, retrying...");
    delay(5000);
  }

  logToSD("[SIM] ERROR: Could not obtain valid network time");
  return false;
}

bool sendHTTPSIM(const String& payload) {
  // ── Parse host and path from DATA_URL ────────────────────────────────────
  // Expected format: "https://host/path"
  String url    = String(DATA_URL);
  String scheme = "https://";
  int    port   = 443;

  if (url.startsWith(scheme)) url = url.substring(scheme.length());

  String hostStr, path;
  int slashPos = url.indexOf('/');
  if (slashPos == -1) {
    hostStr = url;
    path    = "/";
  } else {
    hostStr = url.substring(0, slashPos);
    path    = url.substring(slashPos);
  }

  logToSD("[SIM] HTTP POST -> " + hostStr + path);

  // ── TLS connect with explicit timeout ────────────────────────────────────
  // AWS API Gateway TLS handshake can take 5-10s; without a timeout this
  // call can block indefinitely if the network drops mid-handshake.
  secureClient.setTimeout(15000);  // 15s for the underlying stream ops

  uint32_t connectStart = millis();
  bool connected = false;
  while (millis() - connectStart < 20000) {  // 20s total TLS connect budget
    if (secureClient.connect(hostStr.c_str(), port)) {
      connected = true;
      break;
    }
    logToSD("[SIM] TLS connect attempt failed, retrying...");
    delay(2000);
  }

  if (!connected) {
    logToSD("[SIM] ERROR: TLS connect failed after retries");
    return false;
  }

  // ── Build and send HTTP/1.1 request ──────────────────────────────────────
  String request;
  request.reserve(300 + payload.length());
  request  = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + hostStr + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(payload.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += payload;

  secureClient.print(request);
  logToSD("[SIM] Request sent, waiting for response...");

  // ── Read status line with hard 20s timeout ───────────────────────────────
  // This was the original hang point: no timeout meant the ESP could wait
  // indefinitely for a response that never came (e.g. API Gateway cold start).
  String   statusLine;
  uint32_t readStart = millis();
  bool     gotStatus = false;

  while (millis() - readStart < 20000) {
    if (secureClient.available()) {
      statusLine = secureClient.readStringUntil('\n');
      gotStatus  = true;
      break;
    }
    delay(10);
  }

  // Drain response body regardless of outcome
  uint32_t drainStart = millis();
  while ((secureClient.connected() || secureClient.available())
         && millis() - drainStart < 10000) {
    while (secureClient.available()) secureClient.read();
    delay(5);
  }
  secureClient.stop();

  if (!gotStatus) {
    logToSD("[SIM] ERROR: Timed out waiting for HTTP response");
    return false;
  }

  logToSD("[SIM] Response: " + statusLine.substring(0, 40));

  // Parse HTTP status code from "HTTP/1.1 200 OK\r"
  int code     = 0;
  int spacePos = statusLine.indexOf(' ');
  if (spacePos != -1) {
    code = statusLine.substring(spacePos + 1, spacePos + 4).toInt();
  }

  if (code == 200 || code == 201) {
    return true;
  }

  logToSD("[SIM] ERROR: HTTP " + String(code));
  return false;
}
