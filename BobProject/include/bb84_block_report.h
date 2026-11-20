#pragma once
// One 240x240, low-flicker 10-second results page, synchronized by Bob.
// Optical figures are derived from the actual Bob ADS1115 pre-/post-dark
// and pulse captures. They are NOT photon counts or quantum statistics.
#include "bb84_blocks.h"
namespace demo {

struct OpticalVoltageReport {
  uint32_t readings=0;
  float netA0Min=0,netA0Avg=0,netA0Max=0;
  float netA1Min=0,netA1Avg=0,netA1Max=0;
  float totalMin=0,totalAvg=0,totalMax=0;
  float ambientA0Avg=0,ambientA1Avg=0;
};

struct BlockReportData {
  uint32_t trains=0, slots=0, matched=0, valid=0;
  uint32_t accepted=0, rejected=0, qberErrors=0, qberSamples=0;
  OpticalVoltageReport voltage;
  String result="NO KEY";
};
inline String qberString(const BlockReportData& r){
  return r.qberSamples ? String(100.0f*float(r.qberErrors)/float(r.qberSamples),2)+"%" : "N/A";
}

// Comma-delimited payload is short enough for Alice's TCP line buffer.
// n,A0min,A0avg,A0max,A1min,A1avg,A1max,totalMin,totalAvg,totalMax,dark0,dark1
inline String voltageFields(const OpticalVoltageReport& v){
  return String(v.readings)+","+
    String(v.netA0Min,1)+","+String(v.netA0Avg,1)+","+String(v.netA0Max,1)+","+
    String(v.netA1Min,1)+","+String(v.netA1Avg,1)+","+String(v.netA1Max,1)+","+
    String(v.totalMin,1)+","+String(v.totalAvg,1)+","+String(v.totalMax,1)+","+
    String(v.ambientA0Avg,1)+","+String(v.ambientA1Avg,1);
}
inline bool parseVoltageFields(const String& message, int first,OpticalVoltageReport& v){
  // Allow an explicit zero-reading report but reject a truncated message.
  for(int i=0;i<12;i++)if(field(message,first+i).isEmpty())return false;
  v.readings=uint32_t(strtoul(field(message,first).c_str(),nullptr,10));
  v.netA0Min=field(message,first+1).toFloat();
  v.netA0Avg=field(message,first+2).toFloat();
  v.netA0Max=field(message,first+3).toFloat();
  v.netA1Min=field(message,first+4).toFloat();
  v.netA1Avg=field(message,first+5).toFloat();
  v.netA1Max=field(message,first+6).toFloat();
  v.totalMin=field(message,first+7).toFloat();
  v.totalAvg=field(message,first+8).toFloat();
  v.totalMax=field(message,first+9).toFloat();
  v.ambientA0Avg=field(message,first+10).toFloat();
  v.ambientA1Avg=field(message,first+11).toFloat();
  return true;
}

class BlockReportView {
public:
  BlockReportView(Adafruit_ST7789& lcd,bool alice):lcd_(lcd),alice_(alice){}
  void start(const BlockReportData& r,uint32_t holdMs=REPORT_HOLD_MS){
    active_=true;started_=millis();hold_=holdMs;lastSeconds_=-1;
    lcd_.fillScreen(BG);lcd_.setTextWrap(false);
    lcd_.fillRoundRect(5,5,230,29,5,PANEL);
    lcd_.fillRect(7,9,4,21,CYAN);
    drawText(lcd_,16,10,alice_?"ALICE":"BOB",WHITE,2);
    drawText(lcd_,108,11,"BB84 / RESULTS",CYAN,1);
    drawText(lcd_,108,23,"SPARSE + TOEPLITZ",MUTED,1);

    lcd_.fillRoundRect(7,38,226,30,5,r.result=="KEY AGREED"?uint16_t(0x1347):uint16_t(0x4108));
    drawText(lcd_,15,45,r.result.substring(0,17),r.result=="KEY AGREED"?MINT:GOLD,2);

    drawText(lcd_,13,75,"QBER "+qberString(r),CYAN,2);
    drawText(lcd_,13,95,"SAMPLE ERR "+String(r.qberErrors)+"/"+String(r.qberSamples)+
      "  (8 PER BLOCK)",r.qberErrors?GOLD:MINT,1);
    drawText(lcd_,13,109,"BLOCK OK "+String(r.accepted)+"/"+String(BLOCKS_NEEDED)+
      "    REJECT "+String(r.rejected),WHITE,1);
    drawText(lcd_,13,122,"TRAINS "+String(r.trains)+"  SLOTS "+String(r.slots)+
      "  SIFT "+String(r.valid),MUTED,1);

    lcd_.drawFastHLine(10,139,220,0x528A);
    drawText(lcd_,13,144,"NET mV",CYAN,1);
    drawText(lcd_,64,144,"MIN",MUTED,1);
    drawText(lcd_,121,144,"AVG",MUTED,1);
    drawText(lcd_,181,144,"MAX",MUTED,1);
    if(r.voltage.readings){
      voltageRow(159,"A0",r.voltage.netA0Min,r.voltage.netA0Avg,r.voltage.netA0Max,CYAN);
      voltageRow(175,"A1",r.voltage.netA1Min,r.voltage.netA1Avg,r.voltage.netA1Max,MINT);
      voltageRow(191,"SUM",r.voltage.totalMin,r.voltage.totalAvg,r.voltage.totalMax,WHITE);
      drawText(lcd_,13,208,"AMBIENT A0:"+String(r.voltage.ambientA0Avg,0)+
        " A1:"+String(r.voltage.ambientA1Avg,0)+" mV",GOLD,1);
    }else{
      drawText(lcd_,13,173,"NO DETECTOR VOLTAGE DATA",GOLD,1);
    }
    lcd_.fillRoundRect(7,221,226,16,4,PANEL);
    tick();
  }
  void tick(){
    if(!active_)return;
    const uint32_t passed=uint32_t(millis()-started_);
    const int remain=passed>=hold_?0:int((hold_-passed+999)/1000);
    if(remain==lastSeconds_)return;
    lastSeconds_=remain;
    lcd_.fillRect(11,223,219,12,PANEL);
    drawText(lcd_,14,225,"NEXT RUN "+String(remain)+"s    16=STOP",remain<=3?GOLD:MINT,1);
  }
  bool done() const{return active_ && elapsed(started_,hold_);}
  bool active() const{return active_;}
  void stop(){active_=false;}
private:
  void voltageRow(int y,const char *label,float mi,float av,float ma,uint16_t color){
    lcd_.fillRoundRect(8,y,224,14,3,PANEL);
    drawText(lcd_,13,y+3,label,color,1);
    drawText(lcd_,49,y+3,String(mi,1),WHITE,1);
    drawText(lcd_,108,y+3,String(av,1),WHITE,1);
    drawText(lcd_,166,y+3,String(ma,1),WHITE,1);
  }
  Adafruit_ST7789& lcd_; bool alice_;bool active_=false;
  uint32_t started_=0,hold_=0;int lastSeconds_=-1;
};
} // namespace demo
