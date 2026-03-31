#include "sim_manager.h"
#include "sd_logger.h"
#include "config.h"
// ── TinyGSM setup ────────────────────────────────────────────────────────────
#define TINY_GSM_MODEM_SIM7000SSL
#include <TinyGsmClient.h>

// Optional: comment out StreamDebugger in production to save RAM
// #include <StreamDebugger.h>

#include <time.h>

// ── Pin / APN config (pulled from config.h) ──────────────────────────────────
// MODEM_PWRKEY, MODEM_TX, MODEM_RX are defined in config.h
#ifndef MODEM_APN
  #define MODEM_APN "internet.beeline.am"   // override in config.h if needed
#endif

// ── Internal objects ─────────────────────────────────────────────────────────
static HardwareSerial SerialAT(1);

// Swap the two lines below to enable/disable the AT-command debugger:
// static StreamDebugger _dbg(SerialAT, Serial);
// static TinyGsm        modem(_dbg);
static TinyGsm              modem(SerialAT);
static TinyGsmClientSecure  secureClient(modem);

// ── Helpers ──────────────────────────────────────────────────────────────────

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
// Format from SIM7000: "YY/MM/DD,HH:MM:SS±TZ"
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

  // Reject the SIMCom default epoch (1980-01-06)
  if (yy == 80 || yy == 0) return 0;

  // Quarter-hour offset encoded after the seconds field
  // e.g. "+16" means UTC+4h (16 * 15 min = 240 min)
  int tzQuarters = 0;
  if (raw.length() > 17) {
    char sign = raw.charAt(17);
    tzQuarters = raw.substring(18).toInt();
    if (sign == '-') tzQuarters = -tzQuarters;
  }

  struct tm t = {};
  t.tm_year = (2000 + yy) - 1900;
  t.tm_mon  = mo - 1;
  t.tm_mday = dd;
  t.tm_hour = hh;
  t.tm_min  = mi;
  t.tm_sec  = ss;

  // mktime treats tm as local; we want UTC, so subtract the TZ offset
  time_t epoch = mktime(&t) - (tzQuarters * 15 * 60);
  return epoch;
}

// ============================================================================
// Public API
// ============================================================================

void simPowerOn() {
  logToSD("[SIM] Powering on modem...");

  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);

  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(5000);   // modem boot

  // 🔥 IMPORTANT: wait until AT responds
  uint32_t start = millis();
  while (millis() - start < 10000) {
    SerialAT.println("AT");
    String r = readATResponse(1000);
    if (r.indexOf("OK") != -1) break;
    delay(500);
  }

  modem.init();

  modem.sendAT("+CNMP=13");
  modem.waitResponse(3000);

  // Enable CLTS (safe even if repeated)
  sendATCommand("AT+CLTS=1", 3000);
  sendATCommand("AT&W",      3000);

  logToSD("[SIM] Modem ready");
}

void simPowerOff() {
  logToSD("[SIM] Powering off modem...");
  modem.gprsDisconnect();
  delay(500);

  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1500);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(5000);

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

  uint32_t deadline = millis() + 90000;   // 90-second window
  while (millis() < deadline) {
    SerialAT.println("AT+CCLK?");
    String response = readATResponse(5000);
    time_t epoch = parseCCLK(response);

    if (epoch > 0) {
      // Apply Armenia UTC+4 offset defined in config.h
      struct timeval tv = { epoch, 0 };
      settimeofday(&tv, nullptr);

      setenv("TZ", "UTC-4", 1);
      tzset();
      logToSD("[SIM] Time synced: " + String(ctime(&epoch)));
      return true;
    }

    logToSD("[SIM] Time not ready yet, retrying...");
    delay(5000);
  }

  logToSD("[SIM] ERROR: Could not obtain valid network time");
  return false;
}

bool sendHTTPSIM(const String& payload) {
  const char* host = "";   // parsed from DATA_URL below
  int         port = 443;

  // ── Derive host and path from DATA_URL ───────────────────────────────────
  // DATA_URL format: "https://host/path"
  String url = String(DATA_URL);
  String scheme = "https://";
  String hostStr, path;

  if (url.startsWith(scheme)) {
    url = url.substring(scheme.length());
  }

  int slashPos = url.indexOf('/');
  if (slashPos == -1) {
    hostStr = url;
    path    = "/";
  } else {
    hostStr = url.substring(0, slashPos);
    path    = url.substring(slashPos);
  }

  logToSD("[SIM] HTTP POST -> " + hostStr + path);

  if (!secureClient.connect(hostStr.c_str(), port)) {
    logToSD("[SIM] ERROR: TLS connect failed");
    return false;
  }

  // Build raw HTTP/1.1 request (TinyGsmClientSecure is a plain Stream)
  String request;
  request.reserve(256 + payload.length());
  request  = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + hostStr + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(payload.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += payload;

  secureClient.print(request);

  // Read the status line to determine success
  String statusLine;
  uint32_t start = millis();
  while (secureClient.connected() && millis() - start < 15000) {
    if (secureClient.available()) {
      statusLine = secureClient.readStringUntil('\n');
      break;
    }
    delay(10);
  }

  // Drain the rest
  while (secureClient.connected() || secureClient.available()) {
    while (secureClient.available()) secureClient.read();
  }
  secureClient.stop();

  logToSD("[SIM] Response status: " + statusLine);

  // statusLine: "HTTP/1.1 200 OK\r"
  int code = 0;
  int spacePos = statusLine.indexOf(' ');
  if (spacePos != -1) code = statusLine.substring(spacePos + 1, spacePos + 4).toInt();

  return (code == 200 || code == 201);
}