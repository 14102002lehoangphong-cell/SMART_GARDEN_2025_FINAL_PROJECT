#include "LCD.h"
#include "Relay_Button.h"
#include <string.h>

// ==== extern từ .ino / Sensors / Relay_Button ====
// Thời gian
extern char g_date[11], g_time[9];

// Sensor
extern int   g_moist;
extern int   g_tempC;
extern int   g_hum;
extern int   g_lux;
extern float g_ph;
extern bool  g_rain;
// Relay + mode
extern volatile bool g_autoMode;
extern bool  r1, r2, r3, r4;

// Page LCD: 0 = Main, 1 = Sensor, 2 = Auto config
extern uint8_t g_lcdPage;

// Ngưỡng cố định 24/7
extern int Temp_High, Temp_Low;
extern int Hum_High, Hum_Low;
extern int Lux_High,  Lux_Low;
extern int Soil_High, Soil_Low;

// pH range giống AUTO
static const float PH_MIN = 4.0f;
static const float PH_MAX = 7.0f;

// cache nội dung để tránh nháy màn hình
static char last0[21] = "", last1[21] = "", last2[21] = "", last3[21] = "";

// CỜ ép refresh an toàn giữa các task
static volatile bool s_lcdForceRefresh = false;

void LCD_Init() {
  lcd.init();
  lcd.backlight();
}

void printLinePadded(uint8_t row, const char *text) {
  char buffer[21];
  size_t len = text ? strlen(text) : 0;
  if (len > 20) len = 20;
  if (len) memcpy(buffer, text, len);
  for (size_t i = len; i < 20; i++) buffer[i] = ' ';
  buffer[20] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buffer);
}

// Ép TaskLCD refresh toàn bộ ở vòng lặp kế tiếp
void LCD_ForceRefresh() {
  s_lcdForceRefresh = true;
}

void TaskLCD(void *pvParameters) {
  (void)pvParameters;
  for(;;) {

    // Nếu có yêu cầu refresh từ task khác, xóa màn và reset cache
    if (s_lcdForceRefresh) {
      s_lcdForceRefresh = false;
      lcd.clear();
      last0[0] = last1[0] = last2[0] = last3[0] = '\0';
    }

    if (g_lcdPage == 0) {
      // ===== Page 0: Màn hình chính =====
      char l0[21];
      snprintf(l0, sizeof(l0), "%s  %s", g_date, g_time);
      const char *l1 = "  DO AN TOT NGHIEP";
      const char *l2 = "MONITORING & CONTROL";
      const char *l3 = "  OF MELON GROWING";

      if (strcmp(l0,last0)!=0){ printLinePadded(0,l0); strcpy(last0,l0); }
      if (strcmp(l1,last1)!=0){ printLinePadded(1,l1); strcpy(last1,l1); }
      if (strcmp(l2,last2)!=0){ printLinePadded(2,l2); strcpy(last2,l2); }
      if (strcmp(l3,last3)!=0){ printLinePadded(3,l3); strcpy(last3,l3); }

    } else if (g_lcdPage == 1) {
      // ===== Page 1: Sensor + MODE =====
      char l0[21]; snprintf(l0, sizeof(l0), " M:%3d%% R:%s  %s", g_moist, g_rain ? "YES" : "NO", g_autoMode ? "AUTO" : "MAN");
      char l1[21]; snprintf(l1, sizeof(l1), " T:%2d%cC  PH:%4.1f", g_tempC, (char)223, g_ph);
      char l2[21]; snprintf(l2,sizeof(l2)," H:%2d%%   L:%4dlux", g_hum, g_lux);
      char l3[21]; snprintf(l3,sizeof(l3)," F:%d L:%d P1:%d P2:%d", r1?1:0, r2?1:0, r3?1:0, r4?1:0);

      if (strcmp(l0,last0)!=0){ printLinePadded(0,l0); strcpy(last0,l0); }
      if (strcmp(l1,last1)!=0){ printLinePadded(1,l1); strcpy(last1,l1); }
      if (strcmp(l2,last2)!=0){ printLinePadded(2,l2); strcpy(last2,l2); }
      if (strcmp(l3,last3)!=0){ printLinePadded(3,l3); strcpy(last3,l3); }

    } else {
      // ===== Page 2: Auto thresholds (T/H/L/S) =====
      char l0[21]; snprintf(l0, sizeof(l0), "TH=%2d     TL=%2d", Temp_High, Temp_Low);
      char l1[21]; snprintf(l1, sizeof(l1), "HH=%2d     HL=%2d", Hum_High,  Hum_Low);
      char l2[21]; snprintf(l2, sizeof(l2), "LH=%3d    LL=%3d", Lux_High,  Lux_Low);
      char l3[21]; snprintf(l3, sizeof(l3), "SH=%2d     SL=%2d", Soil_High, Soil_Low);
      // ===== Page 2: Auto thresholds =====

      if (strcmp(l0,last0)!=0){ printLinePadded(0,l0); strcpy(last0,l0); }
      if (strcmp(l1,last1)!=0){ printLinePadded(1,l1); strcpy(last1,l1); }
      if (strcmp(l2,last2)!=0){ printLinePadded(2,l2); strcpy(last2,l2); }
      if (strcmp(l3,last3)!=0){ printLinePadded(3,l3); strcpy(last3,l3); }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
