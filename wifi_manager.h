#pragma once
#include <WiFi.h>
#include <time.h>
#include "rtc_utils.h"

bool connectWiFi();
bool syncTime();
void disconnectWiFi();