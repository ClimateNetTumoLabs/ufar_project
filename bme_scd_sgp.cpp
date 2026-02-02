#include "bme_scd_sgp.h"
#include "sps30_sensor.h"
#include "VOCGasIndexAlgorithm.h"
#include "config.h"
#include <Arduino.h>

Adafruit_BME280 bme;
Adafruit_SCD30 scd30;
Adafruit_SGP40 sgp40;
VOCGasIndexAlgorithm vocAlgo;

void initOtherSensors(){
  bme.begin(0x76);
  scd30.begin();
  sgp40.begin();
}

MeasurementData sampleSensors(int durationSec, int intervalSec){
  MeasurementData data = {};
  int samples = durationSec / intervalSec;

  float tSum=0,hSum=0,pSum=0;
  float pm1Sum=0,pm25Sum=0,pm10Sum=0;
  float co2Sum=0,vocSum=0;

  float pmValues[10];

  for(int i=0;i<samples;i++){
    if(readSPS30(pmValues)){
      pm1Sum+=pmValues[0];
      pm25Sum+=pmValues[1];
      pm10Sum+=pmValues[3];
    }

    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure()/100.0;

    if(scd30.dataReady()) scd30.read();
    co2Sum += scd30.CO2;

    uint16_t rawVoc = sgp40.measureRaw(h*65535/100, (t+45)*65535/175);
    vocSum += vocAlgo.process(rawVoc);

    tSum+=t; hSum+=h; pSum+=p;

    delay(intervalSec*1000);
  }

  data.temperature = tSum/samples;
  data.humidity = hSum/samples;
  data.pressure = pSum/samples;
  data.pm1 = pm1Sum/samples;
  data.pm25 = pm25Sum/samples;
  data.pm10 = pm10Sum/samples;
  data.co2 = co2Sum/samples;
  data.voc = vocSum/samples;

  return data;
}
