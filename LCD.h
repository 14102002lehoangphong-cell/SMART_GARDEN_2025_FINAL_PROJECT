#ifndef LCD_H
#define LCD_H

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

extern LiquidCrystal_I2C lcd;

extern char g_date[11], g_time[9];

// Page LCD: 0 = main, 1 = sensor, 2 = auto config
extern uint8_t g_lcdPage;

// Các giá trị sensor cho LCD
extern int   g_moist;   // %
extern int   g_tempC;   // °C
extern int   g_pres;    // hPa
extern int   g_hum;     // %
extern int   g_lux;     // lux
extern float g_ph;

void LCD_ForceRefresh();
void LCD_Init();
void TaskLCD(void *pvParameters);
void printLinePadded(uint8_t row, const char *text);

#endif // LCD_H
