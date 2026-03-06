#pragma once
#include <WiFi.h>
#include <time.h>
#include "rtc_utils.h"
#include "esp_pm.h"

bool connectWiFi();
bool syncTime();
void disconnectWiFi();
void setCPUSpeed80();
void setCPUSpeed240();