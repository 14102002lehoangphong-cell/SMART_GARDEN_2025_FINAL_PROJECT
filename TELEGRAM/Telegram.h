#ifndef TELEGRAM_H
#define TELEGRAM_H
#include <Arduino.h>

// Khởi tạo bot (gọi trong setup, sau khi có Wi-Fi)
void Telegram_Init(const char* botToken, const char* chatId);

// Gửi tin (đã có mutex bảo vệ).
bool Telegram_Enqueue(const char* msg);
bool Telegram_Enqueue(const String& msg);   // overload cho String

// Gửi mở màn 1 lần (anh đang gọi trong setup)
void Telegram_SendHelloOnce();

// Task polling nhận lệnh từ Telegram (chạy nền)
void TaskTelegram(void *pv);

void Telegram_SendPendingRebootNotice();
void Telegram_MarkReboot(const char* reason);

// ===== THRESH EDIT STATE (dùng chung với Blynk) =====
bool Telegram_IsThreshEditEnabled();
void Telegram_SetThreshEditEnabled(bool en);

#endif
