#pragma once
#include <time.h>
#include <Arduino.h>

time_t calculateNextSend(time_t now, time_t lastSent, int intervalMin);
String timeToStr(time_t t);
