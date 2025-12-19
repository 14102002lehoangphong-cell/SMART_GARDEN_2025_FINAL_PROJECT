#include "Sensors.h"
#include <math.h>

// ===== Kalman 1D đơn giản =====
class SimpleKalman {
  float Q, R, P, X, K;
public:
  SimpleKalman(float q=0.01f, float r=0.10f, float p=1.0f, float x0=0.0f)
    : Q(q), R(r), P(p), X(x0), K(0.0f) {}
  float update(float z) {
    P += Q;                 // predict
    K  = P / (P + R);       // gain
    X += K * (z - X);       // correct
    P *= (1.0f - K);        // update cov
    return X;
  }
};

// ===== Bộ lọc cho từng cảm biến (tune nhẹ theo đặc tính) =====
static SimpleKalman kTemp(0.01f, 0.20f, 1.0f, 25.0f);   // Temp biến chậm
static SimpleKalman kHum (0.02f, 0.30f, 1.0f, 50.0f);   // Humidity
static SimpleKalman kLux (0.50f, 5.00f, 1.0f, 100.0f);  // Lux nhiễu hơn
static SimpleKalman kSoil(0.20f, 1.50f, 1.0f, 50.0f);   // Soil ADC dao động
static SimpleKalman kPH  (0.02f, 0.20f, 1.0f, 7.00f);   // pH dao động nhỏ

// ===== Biến RAW cho log / so sánh Kalman =====
float g_tempC_raw = 0.0f;  // °C chưa lọc
float g_hum_raw   = 0.0f;  // % chưa lọc
float g_lux_raw   = 0.0f;  // lux chưa lọc
int   g_moist_raw = 0;     // % chưa lọc
float g_ph_raw    = 7.0f;  // pH chưa lọc

void Sensors_Init() {
  bool bmeOK = bme.begin(0x76);
  if (!bmeOK) bmeOK = bme.begin(0x77);
  if (!bmeOK) {
    Serial.println("BME280 init failed!");
  } else {
    Serial.println("BME280 ready");
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 init failed!");
  } else {
    Serial.println("BH1750 ready");
  }

  pinMode(SOIL_PIN, INPUT);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
  pinMode(PH_PIN, INPUT);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  pinMode(RAIN_D0_PIN, INPUT);
}

void TaskSensors(void *pvParameters) {
  const TickType_t T_BME  = pdMS_TO_TICKS(1000);
  const TickType_t T_BH   = pdMS_TO_TICKS(400);
  const TickType_t T_SOIL = pdMS_TO_TICKS(500);
  TickType_t tBme = xTaskGetTickCount(), tBh = tBme, tSoil = tBme;

  for(;;){
    TickType_t now = xTaskGetTickCount();

    // --- BME280: Temp/Hum (Pressure nếu anh vẫn dùng) ---
    if (now - tBme >= T_BME) {
      float t = bme.readTemperature();
      float h = bme.readHumidity();
      // float p = bme.readPressure() / 100.0F; // nếu còn dùng
    
      if (!isnan(t)) {
        // RAW trước
        g_tempC_raw = t;
        // Sau Kalman
        float tf = kTemp.update(g_tempC_raw);
        g_tempC = (int)roundf(tf);
      }
      if (!isnan(h)) {
        // RAW trước
        g_hum_raw = h;
        // Sau Kalman
        float hf = kHum.update(g_hum_raw);
        g_hum = (int)roundf(hf);
      }
      // if (!isnan(p)) { float pf = kPres.update(p); g_pres = (int)roundf(pf); }
    
      tBme = now;
    }
    
    // --- BH1750: Lux ---
    if (now - tBh >= T_BH) {
      float lux = lightMeter.readLightLevel();
      if (!isnan(lux)) {
        // RAW
        g_lux_raw = lux;
        // Sau Kalman
        float lf = kLux.update(g_lux_raw);
        g_lux = (int)roundf(lf);
      }
      tBh = now;
    }
    
    // --- Soil + pH: chung 1 khối T_SOIL ---
    if (now - tSoil >= T_SOIL) {
      // Soil moisture (ADC -> %)
      int   rawSoil = analogRead(SOIL_PIN);
      int   percent = map(rawSoil, SOIL_RAW_WET, SOIL_RAW_DRY, 100, 0);
      // RAW (đã map %, chưa Kalman)
      g_moist_raw = constrain(percent, 0, 100);
      // Sau Kalman
      float moistF  = kSoil.update((float)g_moist_raw);
      g_moist       = constrain((int)roundf(moistF), 0, 100);

    
      // pH (ADC -> pH) rồi lọc
      int   rawPH   = analogRead(PH_PIN);
      float voltage = rawPH * (3.3f / 4095.0f);
      float phRaw   = 7.0f + (2.5f - voltage) * 3.5f;        // anh vẫn có thể hiệu chuẩn 2 điểm
      phRaw         = constrain(phRaw, 0.0f, 14.0f);
      // RAW pH
      g_ph_raw = phRaw;
      // Sau Kalman
      g_ph          = kPH.update(phRaw);                     // g_ph là float (giữ 1 chữ số nơi hiển thị)


      int rainLevel = digitalRead(RAIN_D0_PIN);
      g_rain = (rainLevel == LOW);   // nếu test ngược thì đổi thành == HIGH    
      
      tSoil = now;
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
