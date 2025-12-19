#ifndef RELAY_BUTTON_H
#define RELAY_BUTTON_H

#include <Arduino.h>

// ================= Pins (đã khai báo extern trong .ino) =================
extern const int RELAY1;
extern const int RELAY2;
extern const int RELAY3;
extern const int RELAY4;
extern const int RELAY_ON;
extern const int RELAY_OFF;

extern const int BTN1;
extern const int BTN2;
extern const int BTN3;
extern const int BTN4;
extern const int BTN_MODE;
extern const int BTN_UI;
extern const int BTN_OPEN;   
extern const int BTN_CLOSE;  

extern bool btn_open_state;
extern bool btn_close_state;

// Active low?
#ifndef BTN_ACTIVE_LOW
#define BTN_ACTIVE_LOW true
#endif

// ================= Global states =================
extern volatile bool g_autoMode;  
extern bool r1, r2, r3, r4;

// ================= API =================
void RB_Init();
void RB_Task();        // chạy mỗi vòng trong TaskButtons
void RB_TaskManual();  // toggle relay thủ công khi MANUAL

void RB_SetAuto(bool on);
bool RB_IsManual();
bool RB_IsAuto();

// Relay APIs
void RB_SetRelay1(bool on);
void RB_SetRelay2(bool on);
void RB_SetRelay3(bool on);
void RB_SetRelay4(bool on);

void RB_ToggleRelay1();
void RB_ToggleRelay2();
void RB_ToggleRelay3();
void RB_ToggleRelay4();

void RB_TriggerRoofOpen();
void RB_TriggerRoofClose();

extern uint8_t g_roofPwmMin;

#endif
