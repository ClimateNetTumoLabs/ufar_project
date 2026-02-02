#pragma once
#include <Wire.h>

void initSPS30();
void startSPS30();
void stopSPS30();
bool readSPS30(float* pm);
