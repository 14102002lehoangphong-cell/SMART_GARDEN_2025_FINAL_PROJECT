#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

extern Adafruit_BME280 bme;
extern BH1750 lightMeter;

extern int g_tempC;
extern int g_hum;
extern int g_pres;
extern int g_lux;
extern int g_moist;
extern float g_ph;
extern bool g_rain;

extern float g_tempC_raw;   // °C chưa lọc
extern float g_hum_raw;     // % chưa lọc
extern float g_lux_raw;     // lux chưa lọc
extern int   g_moist_raw;   // % chưa lọc
extern float g_ph_raw;      // pH chưa lọc


extern const int SOIL_PIN;
extern const int SOIL_RAW_DRY;
extern const int SOIL_RAW_WET;
extern const int PH_PIN;
extern const int RAIN_D0_PIN;

void Sensors_Init();
void TaskSensors(void *pvParameters);

#endif
