#ifndef ALERTS_H
#define ALERTS_H
#include <Arduino.h>

// Task cảnh báo theo ngưỡng (Temp/Soil/Lux) + gộp MODE+RELAY change
void TaskAlerts(void *pv);

#endif
