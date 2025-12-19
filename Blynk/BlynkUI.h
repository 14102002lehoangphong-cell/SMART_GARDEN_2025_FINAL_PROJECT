#ifndef BLYNK_UI_H
#define BLYNK_UI_H

#include <Arduino.h>

void BlynkUI_Init(const char* auth, const char* host = nullptr, uint16_t port = 0);
void BlynkUI_Run();

// Hàm cho Telegram gọi để sync lại sliders với 6 ngưỡng hiện tại
void BlynkUI_SyncThresholdSliders();

// Hàm cho Telegram/Blynk sync trạng thái công tắc THRESH EDIT
void BlynkUI_SetThreshEditSwitch(bool on);
void BlynkUI_SetBuzzerSwitch(bool on);

#endif
