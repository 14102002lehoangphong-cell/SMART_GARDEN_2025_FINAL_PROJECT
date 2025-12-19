#include "Telegram.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Preferences.h>   // lưu lý do reset
#include <time.h>          // getLocalTime()
#include <stdio.h>         // sscanf

// ==== EXTERNs từ dự án ====
extern char  g_date[11], g_time[9];
extern bool  g_autoMode;
extern bool  r1, r2, r3, r4;
extern int   g_tempC, g_hum, g_pres, g_lux, g_moist;
extern float g_ph;
extern bool g_buzzerEnabled;

extern void Relays_Set(int idx, bool state);
extern void LCD_ForceRefresh();

// Ngưỡng cố định 24/7 (định nghĩa trong .ino)
extern int Temp_High, Temp_Low;
extern int Hum_High, Hum_Low;
extern int Lux_High,  Lux_Low;
extern int Soil_High, Soil_Low;

extern int Pump_Mor_sec;
extern int Pump_Aft_sec;
extern int Pump_Dur_sec;
extern int Pump2_Mor_sec;
extern int Pump2_Aft_sec;
extern int Pump2_Dur_sec;

extern int BUZZER_PIN;

extern void RB_TriggerRoofOpen();
extern void RB_TriggerRoofClose();
extern uint8_t g_roofPwmMin;

// Hàm sync sliders Blynk (định nghĩa trong BlynkUI.cpp)
extern void BlynkUI_SyncThresholdSliders();

static bool s_hasRebootedOnce = false;

// ==== Bot client ====
static WiFiClientSecure      s_client;
static UniversalTelegramBot* s_bot = nullptr;
static String                s_chatId;

// ==== Mutex chặn “đụng TLS” giữa các task ====
static SemaphoreHandle_t s_tgMux = nullptr;

// ==== NVS (Preferences) để lưu lý do reset ====
static Preferences s_prefs;
static bool        s_prefsInit = false;

static void tg_storeLastUpdate(uint32_t id){
  if (s_prefsInit) s_prefs.putUInt("last_upd", id);
}
static uint32_t tg_loadLastUpdate(){
  return s_prefsInit ? s_prefs.getUInt("last_upd", 0) : 0;
}

// Cờ cho phép chỉnh threshold bằng Telegram
static bool s_threshEditEnabled = false;

// ---- helpers ----
static int relayNameToIndex(const String& name) {
  String t = name; t.toLowerCase();
  if (t == "fan")   return 1;
  if (t == "light") return 2;
  if (t == "pump")  return 3;
  if (t == "pump2") return 4;
  return 0;
}

static String relaysStateString() {
  char buf[96];
  snprintf(buf, sizeof(buf),
    "Relays: Fan=%d  Light=%d  Pump=%d  Pump2=%d",
    r1?1:0, r2?1:0, r3?1:0, r4?1:0);
  return String(buf);
}

static void formatTimeHMfromSec(int sec, char* buf, size_t n) {
  if (sec < 0) { snprintf(buf, n, "--:--"); return; }
  int hh = sec / 3600;
  int mm = (sec % 3600) / 60;
  snprintf(buf, n, "%02d:%02d", hh, mm);
}

static void sendThreshStatus() {
  char mor[8], aft[8];
  char mor2[8], aft2[8];    // MỚI
  formatTimeHMfromSec(Pump_Mor_sec,  mor,  sizeof(mor));
  formatTimeHMfromSec(Pump_Aft_sec,  aft,  sizeof(aft));
  formatTimeHMfromSec(Pump2_Mor_sec, mor2, sizeof(mor2));  // MỚI
  formatTimeHMfromSec(Pump2_Aft_sec, aft2, sizeof(aft2));  // MỚI

  char msg[400];
  snprintf(msg, sizeof(msg),
           "THRESHOLDS:\n"
           "Temp: TH=%d TL=%d\n"
           "Hum : HH=%d HL=%d\n"
           "Lux : LH=%d LL=%d\n"
           "Soil: SH=%d SL=%d\n"
           "Pump1 morning : %s\n"
           "Pump1 afternoon: %s\n"
           "Pump1 duration : %ds\n"
           "Pump2 morning : %s\n"
           "Pump2 afternoon: %s\n"
           "Pump2 duration : %ds\n"
           "Edit: %s",
           Temp_High, Temp_Low,
           Hum_High, Hum_Low,
           Lux_High,  Lux_Low,
           Soil_High, Soil_Low,
           mor, aft,
           Pump_Dur_sec,
           mor2, aft2,
           Pump2_Dur_sec,
           s_threshEditEnabled ? "ON" : "OFF");
  Telegram_Enqueue(msg);
}

// ===== THRESH EDIT API (dùng chung với Blynk) =====
bool Telegram_IsThreshEditEnabled() {
  return s_threshEditEnabled;
}

// Được gọi từ Blynk khi user gạt switch THRESH EDIT
void Telegram_SetThreshEditEnabled(bool en) {
  s_threshEditEnabled = en;

  // Sync sang Blynk UI (chắc ăn)
  extern void BlynkUI_SetThreshEditSwitch(bool on);
  BlynkUI_SetThreshEditSwitch(en);

  // Gửi alert lên Telegram
  Telegram_Enqueue(en
    ? "✅ THRESH EDIT: ON (from Blynk)"
    : "✅ THRESH EDIT: OFF (from Blynk)");
}

// ---- public API ----
void Telegram_Init(const char* token, const char* chatId) {
  s_client.setInsecure();                      // đơn giản TLS
  static UniversalTelegramBot bot(token, s_client);
  s_bot = &bot;

  s_chatId = chatId ? chatId : "";

  if (!s_tgMux) s_tgMux = xSemaphoreCreateMutex();

  // Mở namespace NVS cho Telegram
  if (!s_prefsInit) {
    s_prefsInit = s_prefs.begin("telegram", false); // RW
  }

  // Khôi phục last update id để không ăn lại lệnh cũ sau reboot
  if (s_prefsInit && s_bot) {
    uint32_t last_upd = s_prefs.getUInt("last_upd", 0);
    s_bot->last_message_received = last_upd;
  }
}

bool Telegram_Enqueue(const char* msg) {
  if (!msg) return false;
  if (!s_bot || s_chatId.length() == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  // ===== GHÉP NGÀY GIỜ Ở ĐẦU TIN NHẮN =====
  char stamp[32] = "--/--/---- --:--:--";
  struct tm ti;
  if (getLocalTime(&ti, 500)) {
    // Ưu tiên giờ hệ thống (NTP + RTC)
    strftime(stamp, sizeof(stamp), "%d/%m/%Y %H:%M:%S", &ti);
  } else {
    // Fallback dùng g_date + g_time nếu TaskRTC đã fill
    if (g_date[0] != '\0' && g_time[0] != '\0') {
      snprintf(stamp, sizeof(stamp), "%s %s", g_date, g_time);
    } else {
      snprintf(stamp, sizeof(stamp), "N/A");
    }
  }

  // Format: [DD/MM/YYYY HH:MM:SS] + space + nội dung
  String full = "[" + String(stamp) + "]\n" + String(msg);

  bool ok = false;
  if (s_tgMux && xSemaphoreTake(s_tgMux, pdMS_TO_TICKS(2000)) == pdTRUE) {
    ok = s_bot->sendMessage(s_chatId, full, "");
    xSemaphoreGive(s_tgMux);
  }

  vTaskDelay(pdMS_TO_TICKS(60)); // tránh TLS back-to-back
  return ok;
}

bool Telegram_Enqueue(const String& msg) {
  return Telegram_Enqueue(msg.c_str());
}

// Lưu lý do reboot vào NVS (để gửi sau khi boot)
void Telegram_MarkReboot(const char* reason) {
  if (!s_prefsInit) return;
  String r = reason ? String(reason) : String("unknown");
  s_prefs.putString("reboot_reason", r);
  // (tuỳ chọn) lưu timestamp
  s_prefs.putUInt("reboot_epoch", (uint32_t)time(nullptr));
}

// Đọc lý do reboot trong NVS & gửi rồi xoá flag
void Telegram_SendPendingRebootNotice() {
  if (!s_prefsInit) return;
  if (!s_bot || s_chatId.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String reason = s_prefs.getString("reboot_reason", "");
  if (reason.length() == 0) return; // không có gì để gửi
  // XÓA NGAY TRƯỚC KHI GỬI để tránh lặp
  s_prefs.remove("reboot_reason");
  s_prefs.remove("reboot_epoch");
  
  // format thời gian (nếu có)
  uint32_t epoch = s_prefs.getUInt("reboot_epoch", 0);
  char stamp[32] = "--/--/---- --:--:--";
  if (epoch != 0) {
    time_t t = (time_t)epoch;
    struct tm ti;
    if (localtime_r(&t, &ti)) {
      strftime(stamp, sizeof(stamp), "%d/%m/%Y %H:%M:%S", &ti);
    }
  } else {
    struct tm ti;
    if (getLocalTime(&ti, 1000)) {
      strftime(stamp, sizeof(stamp), "%d/%m/%Y %H:%M:%S", &ti);
    }
  }

  char msg[256];
  snprintf(msg, sizeof(msg), "♻️ ESP32 vừa reset\n🗓 %s\nLý do: %s", stamp, reason.c_str());
  Telegram_Enqueue(msg);

  // xoá để không gửi lặp
  s_prefs.remove("reboot_reason");
  s_prefs.remove("reboot_epoch");
}

// Gửi mở màn một lần (anh gọi trong setup)
void Telegram_SendHelloOnce() {
  static bool sent = false;
  if (sent) return;
  if (!s_bot || s_chatId.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Trước khi gửi "online", thử gửi thông báo reset pending (nếu có)
  Telegram_SendPendingRebootNotice();

  // Lấy giờ trực tiếp (không phụ thuộc TaskRTC)
  char stamp[32] = "--/--/---- --:--:--";
  struct tm ti;
  if (getLocalTime(&ti, 1000)) {
    strftime(stamp, sizeof(stamp), "%d/%m/%Y %H:%M:%S", &ti);
  }

  char hello[260];
  snprintf(hello, sizeof(hello),
    "🚀 ESP32 online\n🗓 %s\nMode: %s\nRelays: Fan=%d Light=%d Pump=%d Pump2=%d",
    stamp, g_autoMode ? "AUTO" : "MANUAL",
    r1?1:0, r2?1:0, r3?1:0, r4?1:0);

  Telegram_Enqueue(hello);
  sent = true;
}

// ---- task FreeRTOS: poll lệnh + tránh race bằng mutex ----
void TaskTelegram(void* pv) {
  (void)pv;
  uint32_t lastPoll = 0;

  for (;;) {
    if (s_bot && WiFi.status() == WL_CONNECTED && (millis() - lastPoll) >= 1500) {
      lastPoll = millis();

      int n = 0;
      if (s_tgMux && xSemaphoreTake(s_tgMux, pdMS_TO_TICKS(2000)) == pdTRUE) {
        n = s_bot->getUpdates(s_bot->last_message_received + 1);
        xSemaphoreGive(s_tgMux);
      } else {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      for (int i = 0; i < n; i++) {
        // Lọc chat ID (nếu có set)
        if (s_chatId.length() && s_bot->messages[i].chat_id != s_chatId) continue;

        String txt = s_bot->messages[i].text; txt.trim();
        String low = txt; low.toLowerCase();

        // ===== THRESH COMMANDS =====

        // Bật/tắt chế độ cho phép chỉnh threshold (từ Telegram)
        if (low == "thresh on") {
          s_threshEditEnabled = true;

          // Sync sang Blynk switch
          extern void BlynkUI_SetThreshEditSwitch(bool on);
          BlynkUI_SetThreshEditSwitch(true);

          Telegram_Enqueue(
            "✅ THRESH EDIT: ON\n"
            "Use:\n"
            "set temp H L\n"
            "set lux H L\n"
            "set soil H L\n"
            "set pumpmor HH MM\n"
            "set pumpaft HH MM\n"
            "set pumpdur S"
          );
          continue;
        }

        if (low == "thresh off") {
          s_threshEditEnabled = false;

          // Sync sang Blynk switch
          extern void BlynkUI_SetThreshEditSwitch(bool on);
          BlynkUI_SetThreshEditSwitch(false);

          Telegram_Enqueue("✅ THRESH EDIT: OFF\nThresholds locked.");
          continue;
        }

        // Xem trạng thái hiện tại
        if (low == "thresh" || low == "/thresh") {
          sendThreshStatus();
          continue;
        }

        // set temp H L / set lux H L / set soil H L
        if (low.startsWith("set ")) {
          if (!s_threshEditEnabled) {
            Telegram_Enqueue("⚠️ THRESH EDIT is OFF. Send 'thresh on' first.");
            continue;
          }

          // ví dụ: "set temp 35 28"
          String args = low.substring(4);
          args.trim();

          int hi = 0, lo = 0;

          if (args.startsWith("temp")) {
            if (sscanf(args.c_str(), "temp %d %d", &hi, &lo) == 2) {
              if (hi <= lo) {
                Telegram_Enqueue("❌ Temp: TH must be > TL. Eg: set temp 35 28");
              } else {
                Temp_High = hi;
                Temp_Low  = lo;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated TEMP thresholds");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set temp <TH> <TL>");
            }
            continue;
          }

          if (args.startsWith("hum")) {
            if (sscanf(args.c_str(), "hum %d %d", &hi, &lo) == 2) {
              if (hi <= lo) {
                Telegram_Enqueue("❌ Hum: HH must be > HL. Eg: set hum 80 70");
              } else {
                Hum_High = hi;
                Hum_Low  = lo;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated HUM thresholds");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set hum <HH> <HL>");
            }
            continue;
          }
          
          if (args.startsWith("lux")) {
            if (sscanf(args.c_str(), "lux %d %d", &hi, &lo) == 2) {
              if (hi <= lo) {
                Telegram_Enqueue("❌ Lux: LH must be > LL. Eg: set lux 400 100");
              } else {
                Lux_High = hi;
                Lux_Low  = lo;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated LUX thresholds");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set lux <LH> <LL>");
            }
            continue;
          }

          if (args.startsWith("soil")) {
            if (sscanf(args.c_str(), "soil %d %d", &hi, &lo) == 2) {
              if (hi <= lo) {
                Telegram_Enqueue("❌ Soil: SH must be > SL. Eg: set soil 65 40");
              } else {
                Soil_High = hi;
                Soil_Low  = lo;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated SOIL thresholds");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set soil <SH> <SL>");
            }
            continue;
          }

          if (args.startsWith("roofpwm")) {
            int v = 0;
            if (sscanf(args.c_str(), "roofpwm %d", &v) == 1) {
              if (v < 0) v = 0;
              if (v > 255) v = 255;
              g_roofPwmMin = (uint8_t)v;
          
              char msg[64];
              snprintf(msg, sizeof(msg), "✅ Roof PWM min set to %d", v);
              Telegram_Enqueue(msg);
            } else {
              Telegram_Enqueue("❌ Usage: set roofpwm <0–255>");
            }
            continue;
         }

         if (args.startsWith("pumpmor")) {
            int hh = 0, mm = 0;
            if (sscanf(args.c_str(), "pumpmor %d %d", &hh, &mm) == 2) {
              if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                Telegram_Enqueue("❌ Usage: set pumpmor <HH> <MM> (0–23 0–59)");
              } else {
                Pump_Mor_sec = hh*3600 + mm*60;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated PUMP morning time");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set pumpmor <HH> <MM>");
            }
            continue;
          }

          if (args.startsWith("pumpaft")) {
            int hh = 0, mm = 0;
            if (sscanf(args.c_str(), "pumpaft %d %d", &hh, &mm) == 2) {
              if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                Telegram_Enqueue("❌ Usage: set pumpaft <HH> <MM> (0–23 0–59)");
              } else {
                Pump_Aft_sec = hh*3600 + mm*60;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated PUMP afternoon time");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set pumpaft <HH> <MM>");
            }
            continue;
          }

          if (args.startsWith("pumpdur")) {
            int s = 0;
            if (sscanf(args.c_str(), "pumpdur %d", &s) == 1) {
              if (s < 0) s = 0;
              if (s > 3600) s = 3600;
              Pump_Dur_sec = s;
              BlynkUI_SyncThresholdSliders();
              Telegram_Enqueue("✅ Updated PUMP duration");
              sendThreshStatus();
            } else {
              Telegram_Enqueue("❌ Usage: set pumpdur <seconds>");
            }
            continue;
          }

          if (args.startsWith("pump2mor")) {
            int hh = 0, mm = 0;
            if (sscanf(args.c_str(), "pump2mor %d %d", &hh, &mm) == 2) {
              if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                Telegram_Enqueue("❌ Usage: set pump2mor <HH> <MM> (0–23 0–59)");
              } else {
                Pump2_Mor_sec = hh*3600 + mm*60;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated PUMP2 morning time");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set pump2mor <HH> <MM>");
            }
            continue;
          }

          if (args.startsWith("pump2aft")) {
            int hh = 0, mm = 0;
            if (sscanf(args.c_str(), "pump2aft %d %d", &hh, &mm) == 2) {
              if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                Telegram_Enqueue("❌ Usage: set pump2aft <HH> <MM> (0–23 0–59)");
              } else {
                Pump2_Aft_sec = hh*3600 + mm*60;
                BlynkUI_SyncThresholdSliders();
                Telegram_Enqueue("✅ Updated PUMP2 afternoon time");
                sendThreshStatus();
              }
            } else {
              Telegram_Enqueue("❌ Usage: set pump2aft <HH> <MM>");
            }
            continue;
          }

          if (args.startsWith("pump2dur")) {
            int s = 0;
            if (sscanf(args.c_str(), "pump2dur %d", &s) == 1) {
              if (s < 0) s = 0;
              if (s > 3600) s = 3600;
              Pump2_Dur_sec = s;
              BlynkUI_SyncThresholdSliders();
              Telegram_Enqueue("✅ Updated PUMP2 duration");
              sendThreshStatus();
            } else {
              Telegram_Enqueue("❌ Usage: set pump2dur <seconds>");
            }
            continue;
          }

          Telegram_Enqueue(
            "❌ Unknown 'set' command.\n"
            "Use:\n"
            "set temp H L\n"
            "set lux H L\n"
            "set soil H L\n"
            "set pumpmor HH MM    (Pump1 sáng)\n"
            "set pumpaft HH MM    (Pump1 chiều)\n"
            "set pumpdur S        (Pump1 thời gian tưới)\n"
            "set pump2mor HH MM   (Pump2 sáng)\n"
            "set pump2aft HH MM   (Pump2 chiều)\n"
            "set pump2dur S       (Pump2 thời gian tưới)"
          );
          continue;
        }

        if (low == "/help" || low == "help") {
          Telegram_Enqueue(
            "🤖 COMMAND LIST\n"
            "\n"
            "📌 STATUS\n"
            "/status        → xem trạng thái cảm biến + relay\n"
            "/ping          → kiểm tra WiFi + IP + Mode\n"
            "\n"
            "📌 MODE\n"
            "/mode auto     → bật chế độ AUTO\n"
            "/mode manual   → bật chế độ MANUAL\n"
            "\n"
            "📌 RELAY MANUAL (chỉ MANUAL)\n"
            "/relay fan on/off\n"
            "/relay light on/off\n"
            "/relay pump on/off\n"
            "/relay pump2 on/off\n"
            "\n"
            "📌 THRESH EDIT\n"
            "thresh         → xem toàn bộ ngưỡng hiện tại\n"
            "thresh on      → bật chế độ chỉnh ngưỡng\n"
            "thresh off     → tắt chế độ chỉnh ngưỡng\n"
            "\n"
            "📌 SET THRESHOLDS\n"
            "set temp H L       → ví dụ: set temp 35 28\n"
            "set hum H L       → ví dụ: set hum 80 70\n"
            "set lux H L        → ví dụ: set lux 400 100\n"
            "set soil H L       → ví dụ: set soil 65 40\n"
            "\n"
            "📌 PUMP SCHEDULE (giờ tưới tự động)\n"
            "set pumpmor HH MM  → đặt giờ tưới sáng   (07 30)\n"
            "set pumpaft HH MM  → đặt giờ tưới chiều  (17 30)\n"
            "set pumpdur S      → đặt thời gian tưới  (giây)\n"
            "set pump2mor HH MM  → đặt giờ tưới sáng   (07 30)\n"
            "set pump2aft HH MM  → đặt giờ tưới chiều  (17 30)\n"
            "set pump2dur S      → đặt thời gian tưới  (giây)\n"
            "\n"
            "📌 ROOF (mái che)\n"
            "/roof open      → mở (quay thuận 2s)\n"
            "/roof close     → đóng (quay nghịch 2s)\n"
            "set roofpwm X   → đặt ngưỡng PWM min (0–255)\n"
            "\n"
            "📌 SYSTEM\n"
            "/reset         → khởi động lại ESP32\n"
            "/buzzer on     → bật còi cảnh báo\n"
            "/buzzer off    → tắt (mute) còi cảnh báo\n"
          );
          continue;
        }

        if (low == "/status" || low == "status") {
          char buf[360];
          snprintf(buf, sizeof(buf),
            "📊 STATUS — %s %s\n"
            "Mode: %s\n"
            "T=%d°C  H=%d%%  PH=%.1f\n"
            "Lux=%d  Soil=%d%%\n"
            "%s\n"
            "Buzzer: %s",
            g_date, g_time,
            g_autoMode ? "AUTO" : "MANUAL",
            g_tempC, g_hum, g_ph,
            g_lux, g_moist,
            relaysStateString().c_str(),
            g_buzzerEnabled ? "ON" : "OFF"
          );
          Telegram_Enqueue(buf);
          continue;
        }

        if (low.startsWith("/buzzer")) {
          int sp = low.indexOf(' ');

          // Không có tham số -> trả về trạng thái hiện tại
          if (sp < 0) {
            Telegram_Enqueue(
              String("🔎 Buzzer đang: ") + (g_buzzerEnabled ? "ON (kêu khi có alert)" : "OFF (mute)")
            );
          } else {
            String arg = low.substring(sp + 1);
            arg.trim();

            if (arg == "on") {
              g_buzzerEnabled = true;
              extern void BlynkUI_SetBuzzerSwitch(bool on);
              BlynkUI_SetBuzzerSwitch(true);
              Telegram_Enqueue("🔔 Buzzer: ON (alerts will sound)");
            } else if (arg == "off") {
              g_buzzerEnabled = false;
              extern void BlynkUI_SetBuzzerSwitch(bool on);
              BlynkUI_SetBuzzerSwitch(false);
              Telegram_Enqueue("🔕 Buzzer: OFF (muted)");
            } else {
              Telegram_Enqueue("📌 Usage: /buzzer on | /buzzer off");
            }
          }
          continue;
        }

        if (low.startsWith("/mode")) {
          int sp = low.indexOf(' ');
          if (sp > 0) {
            String arg = low.substring(sp + 1); arg.trim();
            if (arg == "auto")   g_autoMode = true;
            else if (arg == "manual") g_autoMode = false;
            else { Telegram_Enqueue("📌 Usage: /mode auto | /mode manual"); continue; }

            LCD_ForceRefresh();  // cập nhật LCD ngay
            Telegram_Enqueue(String("✅ Mode → ") + (g_autoMode ? "AUTO" : "MANUAL"));
          } else {
            Telegram_Enqueue("📌 Usage: /mode auto | /mode manual");
          }
          continue;
        }

        if (low.startsWith("/relay")) {
          // /relay <fan|light|pump|pump2> <on|off>
          int s1 = low.indexOf(' ');
          int s2 = low.indexOf(' ', s1 + 1);
          if (s1 < 0 || s2 < 0) { Telegram_Enqueue("📌 Usage: /relay <fan|light|pump|pump2> <on|off>"); continue; }

          String name = low.substring(s1 + 1, s2); name.trim();
          String act  = low.substring(s2 + 1);     act.trim();
          int idx = relayNameToIndex(name);
          if (idx == 0) { Telegram_Enqueue("❌ Relay name invalid"); continue; }

          // Nếu muốn chỉ cho MANUAL điều khiển thì để nguyên:
          if (g_autoMode) { Telegram_Enqueue("🛑 AUTO mode. /mode manual để điều khiển tay."); continue; }

          bool on;
          if (act == "on") on = true;
          else if (act == "off") on = false;
          else { Telegram_Enqueue("📌 Action must be on|off"); continue; }

          Relays_Set(idx, on);
          LCD_ForceRefresh();
          const char* nm = (idx==1)?"Fan":(idx==2)?"Light":(idx==3)?"Pump":"Pump2";
          Telegram_Enqueue(String("✅ ") + nm + (on ? " ON" : " OFF"));
          continue;
        }

        if (low == "/roof open") {
          RB_TriggerRoofOpen();
          Telegram_Enqueue("🪟 Roof: OPEN trigger (2s)");
          continue;
        }
        
        if (low == "/roof close") {
          RB_TriggerRoofClose();
          Telegram_Enqueue("🪟 Roof: CLOSE trigger (2s)");
          continue;
        }

        if (low == "/ping" || low == "ping") {
          IPAddress ip = WiFi.localIP();
          char buf[192];
          int rssi = WiFi.RSSI();
          const char* quality;
          
          if      (rssi >= -50) quality = "Rất mạnh";
          else if (rssi >= -60) quality = "Mạnh";
          else if (rssi >= -70) quality = "Trung bình";
          else if (rssi >= -80) quality = "Yếu";
          else                  quality = "Rất yếu";
          
          snprintf(buf, sizeof(buf),
            "✅ ONLINE\n"
            "WiFi: %d dBm (%s)\n"
            "IP: %d.%d.%d.%d\n"
            "Mode: %s",
            rssi, quality,
            ip[0], ip[1], ip[2], ip[3],
            g_autoMode ? "AUTO" : "MANUAL");

          Telegram_Enqueue(buf);
          continue;
        }

        if (low == "/reset" || low == "reset") {
          // Chốt update_id hiện tại để sau boot không polling lại chính lệnh này
          if (s_prefsInit) s_prefs.putUInt("last_upd", s_bot->messages[i].update_id);
        
          Telegram_Enqueue("♻️ Restarting...");
          Telegram_MarkReboot("telegram");
          delay(200);
          ESP.restart();
          continue;
        }

        if (low.startsWith("/")) {
          Telegram_Enqueue("❓ Unknown command. /help");
        }
      }

      if (n > 0) {
        s_bot->last_message_received = s_bot->messages[n - 1].update_id;
        if (s_prefsInit) s_prefs.putUInt("last_upd", s_bot->last_message_received);
      }
      
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
