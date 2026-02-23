#pragma once
#include <Arduino.h>

// Current firmware version — bump this before every release
#define FIRMWARE_VERSION "1.0.0"

// Check GitHub for a newer version and apply OTA update if available.
// Returns true if an update was downloaded and the device is about to reboot.
// Returns false if already up to date or if check/download failed.
bool checkAndApplyOTA();
