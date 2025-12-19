#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <RTClib.h>

// ---- Externs từ main / các lib khác ----
extern RTC_DS3231 rtc;
extern void RTC_BuildStrings(DateTime now, String &dateStr, String &timeStr, String &isoStr);

extern int  g_tempC;
extern int  g_hum;
extern int  g_pres;
extern int  g_lux;
extern int  g_moist;
extern float g_ph;

// NEW: bộ RAW + mưa
extern float g_tempC_raw;
extern float g_hum_raw;
extern float g_lux_raw;
extern int   g_moist_raw;
extern float g_ph_raw;
extern bool  g_rain; 

extern bool r1, r2, r3, r4;
extern bool btn_open_state;  
extern bool btn_close_state;

extern const char* SHEETS_URL;
extern const char* SHEETS_SECRET;

extern bool connectWiFiWithTimeout(uint32_t timeout_ms);
extern void powerOffWiFi();

extern const uint32_t LOG_PERIOD_MS;

bool Logger_PostSnapshot();
bool Logger_PostEvent(const String& event, const String& relay, const String& state);
void Logger_InitQueue();
bool Logger_EnqueueEvent(const char* event, const char* relay, const char* state);

void TaskLogger(void *pvParameters);

#endif // LOGGER_H
