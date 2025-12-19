#include "Logger.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ====== externs (đã có ở .ino / các module khác) ======
extern RTC_DS3231 rtc;
extern const char* SHEETS_URL;
extern const char* SHEETS_SECRET;
extern const uint32_t LOG_PERIOD_MS;

extern volatile bool g_autoMode;
extern int  g_tempC;
extern int  g_hum;
extern int  g_pres;
extern int  g_lux;
extern int  g_moist;
extern float g_ph;

// bộ RAW + mưa
extern float g_tempC_raw;
extern float g_hum_raw;
extern float g_lux_raw;
extern int   g_moist_raw;
extern float g_ph_raw;
extern bool  g_rain; 

extern bool r1, r2, r3, r4;
extern bool btn_open_state;  
extern bool btn_close_state;

void RTC_BuildStrings(DateTime now, String &dateStr, String &timeStr, String &isoStr);

// ====== Hàng đợi sự kiện nhẹ ======
// Giữ nguyên chữ ký hàm nhưng chỉ sử dụng trường 'relay'
typedef struct {
  char event[1];            // không dùng
  char relay[48];           // <--- tăng kích thước để chứa chuỗi dài
  char state[1];            // không dùng
} log_evt_t;

static QueueHandle_t s_logq = NULL;

// ====== Backoff chống spam khi mạng lỗi ======
static uint32_t s_backoffMs     = 0;       // 0 = không backoff
static uint32_t s_backoffUntil  = 0;       // millis() đến khi được gửi lại
static uint32_t s_nextSnapshot  = 0;       // lịch gửi snapshot kế tiếp

// ======================================================
// Public API
// ======================================================
void Logger_InitQueue() {
  if (!s_logq) s_logq = xQueueCreate(8, sizeof(log_evt_t));
}

// Chữ ký giữ nguyên để khỏi sửa file .h/.cpp khác
bool Logger_EnqueueEvent(const char* ev, const char* re, const char* st) {
  if (!s_logq) return false;
  log_evt_t e = {};
  // event/state bỏ qua; chỉ copy relay
  if (re && *re) {
    strncpy(e.relay, re, sizeof(e.relay) - 1);
  } else {
    e.relay[0] = '\0';
  }
  return xQueueSend(s_logq, &e, 0) == pdTRUE;
}

// ======================================================
// Helpers
// ======================================================
static bool postJsonToSheets(const String& json) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SHEETS] WiFi not connected, skip");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();               // Apps Script HTTPS
  HTTPClient http;
  http.setTimeout(8000);              // 8s network timeout

  if (!http.begin(client, SHEETS_URL)) {
    Serial.println("[SHEETS] http.begin() failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  String resp = http.getString();
  http.end();

  Serial.printf("[SHEETS] POST code=%d resp=%s\n", code, resp.c_str());
  return (code > 0 && code < 400);    // coi 2xx/3xx là thành công
}

// snapshot chung
bool Logger_PostSnapshot() {
  DateTime now = rtc.now();
  String dateStr, timeStr, isoStr;
  RTC_BuildStrings(now, dateStr, timeStr, isoStr);
  const char* modeText = g_autoMode ? "AUTO" : "MANUAL";

  String payload = "{";
  payload += "\"secret\":\"" + String(SHEETS_SECRET) + "\",";
  payload += "\"iso\":\""    + isoStr  + "\",";
  payload += "\"date\":\""   + dateStr + "\",";
  payload += "\"time\":\""   + timeStr + "\",";

  // snapshot: không có relay cụ thể -> để rỗng
  payload += "\"relay\":\"\",";
  payload += "\"mode\":\""   + String(modeText) + "\",";

  // ===== RAW - FILTERED SONG SONG =====
  payload += "\"tempC_raw\":" + String(g_tempC_raw, 1) + ",";
  payload += "\"tempC\":"     + String(g_tempC)       + ",";

  payload += "\"hum_raw\":"   + String(g_hum_raw, 1)  + ",";
  payload += "\"hum\":"       + String(g_hum)         + ",";

  payload += "\"ph_raw\":"    + String(g_ph_raw, 1)   + ",";
  payload += "\"ph\":"        + String(g_ph, 1)       + ",";

  payload += "\"lux_raw\":"   + String(g_lux_raw, 1)  + ",";
  payload += "\"lux\":"       + String(g_lux)         + ",";

  payload += "\"moist_raw\":" + String(g_moist_raw)   + ",";
  payload += "\"moist\":"     + String(g_moist)       + ",";

  // Rain
  payload += "\"rain\":"      + String(g_rain ? "true" : "false") + ",";

  // Relays
  payload += "\"r1\":"        + String(r1 ? "true":"false") + ",";
  payload += "\"r2\":"        + String(r2 ? "true":"false") + ",";
  payload += "\"r3\":"        + String(r3 ? "true":"false") + ",";
  payload += "\"r4\":"        + String(r4 ? "true":"false") + ",";

  // Buttons
  payload += "\"btn_open\":"  + String(btn_open_state  ? "true" : "false") + ",";
  payload += "\"btn_close\":" + String(btn_close_state ? "true" : "false");

  payload += "}";

  return postJsonToSheets(payload);
}

// event đơn lẻ: CHỈ gửi 'relay', bỏ event & state (để trống)
bool Logger_PostEvent(const String& event, const String& relay, const String& state) {
  DateTime now = rtc.now();
  String dateStr, timeStr, isoStr;
  RTC_BuildStrings(now, dateStr, timeStr, isoStr);
  const char* modeText = g_autoMode ? "AUTO" : "MANUAL";

  String payload = "{";
  payload += "\"secret\":\"" + String(SHEETS_SECRET) + "\",";
  payload += "\"iso\":\""    + isoStr  + "\",";
  payload += "\"date\":\""   + dateStr + "\",";
  payload += "\"time\":\""   + timeStr + "\",";

  // event/state hiện anh chưa dùng, cứ để trống cho đủ form
  payload += "\"event\":\"\",";                          // vẫn trống
  payload += "\"relay\":\""  + relay   + "\",";          // relay có dữ liệu (R1=ON...)
  payload += "\"state\":\"\",";                          // trống
  payload += "\"mode\":\""   + String(modeText) + "\",";

  // ===== RAW - FILTERED SONG SONG =====
  payload += "\"tempC_raw\":" + String(g_tempC_raw, 1) + ",";
  payload += "\"tempC\":"     + String(g_tempC)       + ",";

  payload += "\"hum_raw\":"   + String(g_hum_raw, 1)  + ",";
  payload += "\"hum\":"       + String(g_hum)         + ",";

  payload += "\"ph_raw\":"    + String(g_ph_raw, 1)   + ",";
  payload += "\"ph\":"        + String(g_ph, 1)       + ",";

  payload += "\"lux_raw\":"   + String(g_lux_raw, 1)  + ",";
  payload += "\"lux\":"       + String(g_lux)         + ",";

  payload += "\"moist_raw\":" + String(g_moist_raw)   + ",";
  payload += "\"moist\":"     + String(g_moist)       + ",";

  // Rain
  payload += "\"rain\":"      + String(g_rain ? "true" : "false") + ",";

  // Relays
  payload += "\"r1\":"        + String(r1 ? "true" : "false") + ",";
  payload += "\"r2\":"        + String(r2 ? "true" : "false") + ",";
  payload += "\"r3\":"        + String(r3 ? "true" : "false") + ",";
  payload += "\"r4\":"        + String(r4 ? "true" : "false") + ",";

  // Buttons
  payload += "\"btn_open\":"  + String(btn_open_state  ? "true" : "false") + ",";
  payload += "\"btn_close\":" + String(btn_close_state ? "true" : "false");

  payload += "}";

  return postJsonToSheets(payload);
}

// ======================================================
// Task chính
// ======================================================
void TaskLogger(void *pv) {
  Logger_InitQueue();
  s_nextSnapshot = millis();           // gửi ngay lần đầu

  for (;;) {
    uint32_t nowMs = millis();

    // snapshot định kỳ, tôn trọng backoff
    if (nowMs >= s_nextSnapshot && nowMs >= s_backoffUntil) {
      bool ok = Logger_PostSnapshot();
      if (!ok) {
        // tăng backoff: 10s → 20s → 40s … tối đa 60s
        s_backoffMs    = (s_backoffMs == 0) ? 10000 : min<uint32_t>(s_backoffMs * 2, 60000);
        s_backoffUntil = nowMs + s_backoffMs;
      } else {
        s_backoffMs = 0;
        s_backoffUntil = 0;
      }
      // lịch lần kế tiếp
      s_nextSnapshot = nowMs + LOG_PERIOD_MS;
    }

    // gửi các sự kiện trong queue (nếu không backoff)
    if (nowMs >= s_backoffUntil && s_logq) {
      log_evt_t e;
      while (xQueueReceive(s_logq, &e, 0) == pdTRUE) {
    
        if (!g_autoMode) {
          // ===== MANUAL: gom trong 2 giây =====
          String batch;
          if (e.relay[0]) { batch += e.relay; }
    
          uint32_t until = millis() + 2000;                  // cửa sổ 2s
          for (;;) {
            // nếu hết cửa sổ thì dừng
            if ((int32_t)(millis() - until) >= 0) break;
    
            log_evt_t e2;
            // chờ tối đa ~80ms để gom thêm; nếu không có thì vòng lại cho tới khi hết 2s
            if (xQueueReceive(s_logq, &e2, pdMS_TO_TICKS(80)) == pdTRUE) {
              if (e2.relay[0]) {
                if (batch.length()) batch += ' ';
                batch += e2.relay;
              }
            }
          }
    
          // Gửi 1 phát duy nhất cho cả 2 giây
          bool ok = Logger_PostEvent("", String("MANUAL: ") + batch, "");
          if (!ok) {
            s_backoffMs    = (s_backoffMs == 0) ? 10000 : min<uint32_t>(s_backoffMs * 2, 60000);
            s_backoffUntil = millis() + s_backoffMs;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(50));
        } else {
          // ===== AUTO: giữ nguyên — gửi từng event ngay =====
          bool ok = Logger_PostEvent("", String(e.relay), "");
          if (!ok) {
            s_backoffMs    = (s_backoffMs == 0) ? 10000 : min<uint32_t>(s_backoffMs * 2, 60000);
            s_backoffUntil = millis() + s_backoffMs;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(50));
        }
      }
    }   

    vTaskDelay(pdMS_TO_TICKS(200));      // nhịp cho task
  }
}
