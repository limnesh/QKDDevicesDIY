#pragma once
// ColorSync V7: readable shared AES-256-FORMAT DEMO key page on both screens.
// Draw once, then update only the seconds counter (no repeated full refresh).
#include "bb84_shared.h"
namespace demo {
class DemoKeyView {
public:
 explicit DemoKeyView(Adafruit_ST7789& lcd,bool alice):lcd_(lcd),alice_(alice){}
 void start(const String& hex, unsigned session, unsigned sifted,
            const String& qber){
   started_=millis();lastSeconds_=-1;active_=true;
   lcd_.fillScreen(BG);lcd_.setTextWrap(false);
   lcd_.fillRoundRect(5,5,230,32,5,PANEL);
   lcd_.fillRect(7,9,4,24,CYAN);
   lcd_.setTextSize(2);lcd_.setTextColor(WHITE);
   lcd_.setCursor(16,11);lcd_.print(alice_?"ALICE":"BOB");
   lcd_.setTextSize(1);lcd_.setTextColor(CYAN);
   lcd_.setCursor(108,10);lcd_.print("AES-256");
   lcd_.setTextColor(MUTED);lcd_.setCursor(108,22);lcd_.print("DEMO KEY");
   lcd_.fillRoundRect(7,42,226,33,5,uint16_t(0x1347));
   lcd_.setTextSize(2);lcd_.setTextColor(MINT);
   lcd_.setCursor(37,47);lcd_.print("KEY AGREED");
   lcd_.setTextSize(1);lcd_.setTextColor(MUTED);
   lcd_.setCursor(12,66);lcd_.print("RUN ");lcd_.print(session);
   lcd_.print(" / RAW ");lcd_.print(sifted);
   lcd_.print(" / QBER ");lcd_.print(qber);lcd_.print("%");
   for(unsigned r=0;r<4;r++){
     const int y=81+28*r;
     lcd_.fillRoundRect(7,y,226,25,4,PANEL);
     const String text=hex.substring(16*r,16*r+8)+" "+
                       hex.substring(16*r+8,16*r+16);
     lcd_.setTextSize(2);
     lcd_.setTextColor((r&1u)?WHITE:CYAN);
     lcd_.setCursor(18,y+5);lcd_.print(text);
   }
   lcd_.setTextSize(1);lcd_.setTextColor(GOLD);
   lcd_.setCursor(13,197);lcd_.print("DISPLAY: 20 SECONDS");
   lcd_.setTextColor(MUTED);
   lcd_.setCursor(13,207);lcd_.print("NOT QUANTUM-SECURE QKD");
   lcd_.fillRoundRect(7,216,226,21,4,PANEL);
   tick();
 }
 void tick(){
   if(!active_)return;
   uint32_t passed=uint32_t(millis()-started_)/1000u;
   if(passed>20u)passed=20u;
   const int remain=int(20u-passed);
   if(remain==lastSeconds_)return;
   lastSeconds_=remain;
   lcd_.fillRect(12,218,216,17,PANEL);
   lcd_.setTextSize(1);lcd_.setTextColor(remain<=5?GOLD:MINT);
   lcd_.setCursor(15,223);
   lcd_.print("RESULTS IN ");lcd_.print(remain);lcd_.print("s");
   lcd_.setTextColor(WHITE);lcd_.print("   16=STOP");
 }
 bool done() const {return active_ && elapsed(started_,KEY_DISPLAY_MS);}
 bool active() const {return active_;}
 void stop(){active_=false;}
private:
 Adafruit_ST7789& lcd_;bool alice_;bool active_=false;
 uint32_t started_=0;int lastSeconds_=-1;
};
} // namespace demo
