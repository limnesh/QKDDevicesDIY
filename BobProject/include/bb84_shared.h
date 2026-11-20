#pragma once
// ColorSync V7 hardware, optical measurement and display helpers.
// NOT quantum-secure QKD. Block math and PA are in bb84_blocks.h.
// THIS HEADER IS IDENTICAL ON BOTH DEVICES.
#include <Arduino.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_arduino_version.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <math.h>
#include <algorithm>

namespace demo {
constexpr char SSID[] = "BB84-DEMO";
constexpr char PASS[] = "ChangeThis2026"; // Change before demonstrating in public.
constexpr uint16_t TCP_PORT=7777;
constexpr int TFT_SCLK=21, TFT_MOSI=47, TFT_RST=45, TFT_DC=40, TFT_CS=41, TFT_BL=42;
constexpr uint16_t BG=0x080F, PANEL=0x192B, WHITE=0xFFFF, MUTED=0xAD55,
  CYAN=0x07FF, MINT=0x3FED, GOLD=0xFFE0, GREEN=0x07E0, RED=0xF9C7;
constexpr float ADC_MV_PER_COUNT=0.125f;  // ADS1115 GAIN_ONE
constexpr uint32_t SERVO_SETTLE_MIN_MS=260;
constexpr uint32_t SERVO_MS_PER_DEG=6;
constexpr uint32_t LASER_WARMUP_MS=35;
constexpr uint32_t MAX_PULSE_ON_MS=1400; // Alice safety interlock; Bob reads in ~100ms
constexpr uint32_t ALIGN_MAX_MS=30000;   // Hard limit in Alice; operator re-arms manually
constexpr uint8_t DARK_SAMPLES=4;
constexpr uint8_t LIGHT_SAMPLES=10;
constexpr int ADC_CLIP_COUNTS=26000;
// Successful full sessions hold their post-processed demo key for 20 seconds.
constexpr uint32_t KEY_DISPLAY_MS=20000;

constexpr int CAL_A_PLUS_0=120, CAL_A_PLUS_1=29;
constexpr int CAL_A_X_0=165, CAL_A_X_1=65;
constexpr int CAL_B_PLUS=50, CAL_B_X=105;
inline bool elapsed(uint32_t start, uint32_t ms){return uint32_t(millis()-start)>=ms;}
inline char basisFromToken(const String& token){return token=="X"?'X':'+';}
inline int angleFor(char basis, uint8_t bit){
  if(basis=='+')return bit?CAL_A_PLUS_1:CAL_A_PLUS_0;
  return bit?CAL_A_X_1:CAL_A_X_0;
}
inline char toBasis(uint8_t b){return b?'X':'+';}
inline String field(const String& s,int pos){
  int start=0;
  for(int i=0;i<pos;i++){int p=s.indexOf(',',start);if(p<0)return "";start=p+1;}
  int end=s.indexOf(',',start);return end<0?s.substring(start):s.substring(start,end);
}
inline bool send(WiFiClient& c,const String& s){return c.connected()&&c.println(s)>0;}
inline String signature(const String& input){
  uint8_t digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256((const unsigned char*)input.c_str(), input.length(), digest, 0);
#else
  mbedtls_sha256_ret((const unsigned char*)input.c_str(), input.length(), digest, 0);
#endif
  const char* hex="0123456789ABCDEF";
  String out;out.reserve(64);
  for(auto c:digest){out+=hex[c>>4];out+=hex[c&15];}
  return out;
}
inline String hexNonce(uint32_t number){
  char buffer[9];snprintf(buffer,sizeof(buffer),"%08lX",(unsigned long)number);
  return String(buffer);
}
// The stable PWM wrapper comes from the already working calibration firmware.
inline bool attachServo(int pin,int channel){
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;return ledcAttach(pin,50,14);
#else
  if(ledcSetup(channel,50,14)<=0)return false;
  ledcAttachPin(pin,channel);return true;
#endif
}
inline void writeServo(int pin,int channel,int degrees){
  const int us=map(constrain(degrees,0,180),0,180,1000,2000);
  const uint32_t duty=uint32_t(us)*16384UL/20000UL;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;ledcWrite(pin,duty);
#else
  (void)pin;ledcWrite(channel,duty);
#endif
}
inline void lcdInit(Adafruit_ST7789& lcd){
  pinMode(TFT_BL,OUTPUT);digitalWrite(TFT_BL,HIGH);
  lcd.init(240,240);lcd.setRotation(0);lcd.setTextWrap(false);lcd.fillScreen(BG);
}
inline void drawText(Adafruit_ST7789& lcd,int x,int y,const String& text,
                     uint16_t color=WHITE,uint8_t scale=1){
  lcd.setTextColor(color);lcd.setTextSize(scale);lcd.setCursor(x,y);lcd.print(text);
}
inline void lcdRow(Adafruit_ST7789& lcd,int y,const String& s,
                   uint16_t color=WHITE,uint8_t scale=2){
  const int h=scale==2?19:13;
  lcd.fillRect(5,y,230,h,BG);drawText(lcd,8,y,s,color,scale);
}
}
