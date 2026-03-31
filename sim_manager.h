#pragma once

#include <Arduino.h>
#include "config.h"

// ===================== Lifecycle =====================

/** Power on the SIM7000 modem and initialize TinyGSM. */
void simPowerOn();

/** Power off the SIM7000 modem cleanly. */
void simPowerOff();

// ===================== Connectivity =====================

/**
 * Connect to the cellular network and bring up GPRS/LTE-M.
 * Mirrors connectWiFi() — returns true on success.
 * Call this instead of connectWiFi() everywhere in main.cpp.
 */
bool connectSIM();

/**
 * Disconnect GPRS gracefully.
 * Call before deep sleep instead of WiFi disconnect.
 */
void disconnectSIM();

// ===================== Time =====================

/**
 * Synchronise system time from the network operator clock (AT+CCLK).
 * Mirrors syncTime() from rtc_utils — returns true when a valid
 * timestamp was obtained and settimeofday() was called.
 */
bool syncTimeSIM();

// ===================== HTTP =====================

/**
 * POST a JSON payload to DATA_URL over HTTPS (port 443).
 * Mirrors sendHTTP() from json_utils — returns true on HTTP 200/201.
 */
bool sendHTTPSIM(const String& payload);