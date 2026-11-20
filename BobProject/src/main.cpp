/*
  BOB - ColorSync V7 32-slot trains / block QBER / sparse parity / Toeplitz PA

  Confirmed keypad wiring from the photograph of the completed mapper:
    Rows R1..R4: GPIO11, GPIO12, GPIO13, GPIO14
    Cols C1..C4: GPIO18, GPIO8, GPIO9, GPIO10
  Physical key numbers: 01 02 03 04 / 05 06 07 08 /
                        09 10 11 12 / 13 14 15 16

  Bob HWP servo GPIO19, ADS1115 I2C SDA=GPIO17 SCL=GPIO20 (0x48),
  OPT101 detectors -> ADS A0 and A1. ST7789 shared pins in header.
  Alice AP = BB84-DEMO, 10.0.0.1:7777.

  No external Keypad library required. Matrix scanning never drives
  any keypad pin HIGH. No motor on Bob GPIO14 (it is a keypad row).
  VERIFIED: Alice GPIO14 is the LASER; Alice GPIO17 is the attenuator/decoy
  servo which this firmware NEVER controls. Bob GPIO14 is a separate device
  and is safely used as keypad R4.

  This is a classical-light polarization/BB84 education simulator,
  NOT physical single-photon QKD or a cryptographically secure key source.
  Alignment beam is TIME LIMITED to 30s by Alice, with software STOP.
  Always provide a physical laser switch, beam enclosure and beam dump.
*/
#include "bb84_shared.h"
#include "bb84_sync_ui.h"
#include "bb84_audio.h"
#include "bb84_key_view.h"
#include "bb84_blocks.h"
#include "bb84_block_report.h"
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
using namespace demo;

constexpr int BOB_HWP_PIN=19;
constexpr int BOB_SDA=17;
constexpr int BOB_SCL=20;    // User's actual wiring; NOT native USB when I2C is active.
constexpr uint8_t ADS_ADDRESS=0x48;
constexpr uint8_t ROWS[4]={11,12,13,14};
constexpr uint8_t COLS[4]={18,8,9,10};
constexpr uint16_t KEY_DEBOUNCE_MS=45;

Adafruit_ADS1115 ads;
Adafruit_ST7789 tft(TFT_CS,TFT_DC,TFT_MOSI,TFT_SCLK,TFT_RST);
SyncDashboard syncUi(tft,false);
SoftChime chime;
DemoKeyView keyView(tft,false);
BlockReportView reportView(tft,false);
WiFiClient alice;
String lineBuffer, serialBuffer;
bool adcOK=false,abortRequested=false,running=false,aligningLive=false;
uint32_t nextId=1000;
int bAngle=CAL_B_PLUS,aAngle=CAL_A_X_0;
char aBasis='X',bBasis='+';
uint8_t aBit=0;
int adjustStep=1;
float minSignalMv=30.0f;
float maxDarkDriftMv=35.0f;
int mixedThreshold=25;
String lastNote="READY",lastHash="",lastSummary="NO RUN";

// Physical position 1..16 maps to 1..9 then A..G internally.
const char KEYCHARS[16]={'1','2','3','4','5','6','7','8','9','A','B','C','D','E','F','G'};
char rawKey=0,stableKey=0,pendingControlKey=0;
uint32_t rawChanged=0;
bool keyReported=false;
enum class Page:uint8_t{HOME,MANUAL,ALIGN,SETTINGS,RESULT};
Page page=Page::HOME;
// Screen ownership: only rebuild the colorful dashboard when switching
// from a keypad/menu/diagnostic screen; never on each slot or repeated read.
enum class View:uint8_t{OTHER,SYNC};
View view=View::OTHER;
void enterSyncView(){
  if(view!=View::SYNC){syncUi.invalidate();view=View::SYNC;}
}

struct Samples {
  int32_t sum0=0,sum1=0;
  int16_t min0=32767,max0=0,min1=32767,max1=0;
  int clips=0;
  bool good=true;
  float a0() const{return float(sum0)/float(DARK_SAMPLES);} // use avg(count) for clarity elsewhere
};
struct Reading {
  uint32_t id=0;
  char aliceBasis='?',bobBasis='?';
  uint8_t aliceBit=0;
  int aliceAngle=0,bobAngle=0;
  float dark0=0,dark1=0,net0=0,net1=0,contrast=0,drift=0;
  int clipped=0,range0=0,range1=0;
  int bit=-1; // -1 ambiguous/invalid
  String quality="INVALID";
  bool valid=false;
};
struct Stats {
  int total=0,matched=0,matchedValid=0,matchedInvalid=0,errors=0;
  int mismatched=0,mixedWrong=0,clipped=0,weak=0,unstable=0;
  String siftBits; // unused in V7: private raw bits stay in 64-position blocks.
  uint32_t trains=0,attempts=0,accepted=0,rejected=0,qberSamples=0;
} stats;
uint64_t parityH[PARITY_CHECKS];
uint8_t pendingBob[BLOCK_BITS+TRAIN_SLOTS]={0};
uint8_t keyMaterial[MATERIAL_BITS]={0};
uint8_t pendingCount=0;
String opticalStage="";
uint32_t blockAttempt=0;
String activeSessionNonce;
Reading frameReadings[TRAIN_SLOTS];
BlockReportData summaryData;

Reading lastReading;
// Track every successful optical slot, including mismatched bases.
// Net voltages are light minus the mean of pre-/post-pulse DARK readings.
// Ambient is the average laser-OFF baseline seen at each detector.
struct VoltageAccumulator {
  uint32_t n=0;
  float minA0=0,maxA0=0,minA1=0,maxA1=0,minSum=0,maxSum=0;
  double sumA0=0,sumA1=0,sumTotal=0,sumDarkA0=0,sumDarkA1=0;
  void add(const Reading& r){
    const float total=r.net0+r.net1;
    if(n==0){
      minA0=maxA0=r.net0;minA1=maxA1=r.net1;
      minSum=maxSum=total;
    }else{
      minA0=fminf(minA0,r.net0);maxA0=fmaxf(maxA0,r.net0);
      minA1=fminf(minA1,r.net1);maxA1=fmaxf(maxA1,r.net1);
      minSum=fminf(minSum,total);maxSum=fmaxf(maxSum,total);
    }
    ++n;
    sumA0+=r.net0;sumA1+=r.net1;sumTotal+=total;
    sumDarkA0+=r.dark0;sumDarkA1+=r.dark1;
  }
  OpticalVoltageReport report() const{
    OpticalVoltageReport v;v.readings=n;
    if(!n)return v;
    const double inv=1.0/double(n);
    v.netA0Min=minA0;v.netA0Avg=float(sumA0*inv);v.netA0Max=maxA0;
    v.netA1Min=minA1;v.netA1Avg=float(sumA1*inv);v.netA1Max=maxA1;
    v.totalMin=minSum;v.totalAvg=float(sumTotal*inv);v.totalMax=maxSum;
    v.ambientA0Avg=float(sumDarkA0*inv);v.ambientA1Avg=float(sumDarkA1*inv);
    return v;
  }
};
VoltageAccumulator voltageStats;


void setNote(const String& s){lastNote=s;Serial.println("#BOB "+s);}

// Scan a passive matrix: all lines INPUT_PULLUP, one row briefly LOW.
// Opposing driven outputs are never possible. Reject multiple simultaneous keys.
char rawScan(){
  int foundR=-1,foundC=-1,count=0;
  for(int r=0;r<4;r++){
    digitalWrite(ROWS[r],LOW);
    pinMode(ROWS[r],OUTPUT);
    delayMicroseconds(70);
    for(int c=0;c<4;c++){
      if(digitalRead(COLS[c])==LOW){foundR=r;foundC=c;count++;}
    }
    pinMode(ROWS[r],INPUT_PULLUP);
  }
  return count==1?KEYCHARS[foundR*4+foundC]:0;
}
char keyEvent(){
  const char now=rawScan();
  if(now!=rawKey){rawKey=now;rawChanged=millis();}
  if(elapsed(rawChanged,KEY_DEBOUNCE_MS)&&stableKey!=rawKey){
    stableKey=rawKey;keyReported=false;
  }
  if(stableKey&&!keyReported){keyReported=true;return stableKey;}
  return 0;
}
char actionKey(){
  if(pendingControlKey){const char k=pendingControlKey;pendingControlKey=0;return k;}
  return keyEvent();
}
int physicalKey(char k){
  for(int i=0;i<16;i++)if(KEYCHARS[i]==k)return i+1;
  return 0;
}
void beginKeypad(){
  for(int r=0;r<4;r++)pinMode(ROWS[r],INPUT_PULLUP);
  for(int c=0;c<4;c++)pinMode(COLS[c],INPUT_PULLUP);
}
// Blocking functions poll only the dedicated 16th physical key as STOP.
bool pollAbort(){
  const char k=keyEvent();
  if(k=='G')abortRequested=true;
  else if(k && !pendingControlKey && (!running || aligningLive))pendingControlKey=k;
  return abortRequested;
}
bool delayWithStop(uint32_t ms){
  const uint32_t start=millis();
  while(!elapsed(start,ms)){
    if(pollAbort())return false;
    delay(3);
  }
  return true;
}
void screenTitle(const String& title){
  view=View::OTHER;     // Next BB84 view must rebuild its static tiles once.
  tft.fillScreen(BG);
  tft.fillRoundRect(4,4,232,29,4,PANEL);
  drawText(tft,10,10,title,CYAN,2);
}
void footer(const String& text){
  tft.fillRect(4,217,232,21,PANEL);
  drawText(tft,9,222,text.substring(0,36),GOLD,1);
}
// For rapidly updating voltage/QBER fields: built-in GFX font draws opaque
// characters directly over the old text instead of blanking the whole row.
void smoothRow(int y,const String& value,uint16_t fg,uint8_t scale=2){
  const int maxChars=scale==2?19:38;
  String s=value.substring(0,maxChars);
  while((int)s.length()<maxChars)s+=' ';
  tft.setTextSize(scale);
  tft.setTextColor(fg,BG);
  tft.setCursor(8,y);
  tft.print(s);
}
void showLink(){
  screenTitle("BOB / NETWORK");
  lcdRow(tft,44,WiFi.status()==WL_CONNECTED?"WIFI CONNECTED":"WIFI OFF",WiFi.status()==WL_CONNECTED?GREEN:RED,2);
  lcdRow(tft,72,alice.connected()?"ALICE ONLINE":"TCP OFFLINE",alice.connected()?GREEN:RED,2);
  lcdRow(tft,103,"AP: 10.0.0.1",WHITE,1);
  lcdRow(tft,123,"BOB: "+WiFi.localIP().toString(),WHITE,1);
  lcdRow(tft,149,"A servo: "+String(aAngle),WHITE,1);
  lcdRow(tft,164,"B servo: "+String(bAngle),WHITE,1);
  lcdRow(tft,182,"ADS: "+String(adcOK?"0x48 OK":"NO LINK"),adcOK?GREEN:RED,1);
  footer("15=BACK  16=STOP");
}
// Each cell is the SAME physical position as Bob's 4x4 keypad.
// Two short text lines fit a 240x240 screen without truncating a long label.
struct KeyLabel { const char *line1; const char *line2; };
void drawMenu(Page p){
  page=p;
  static const KeyLabel home[16]={
    {"BB84","32 TEST"},{"FULL","16 BLOCK"},{"MANUAL","CONTROL"},{"ALIGN","LIVE"},
    {"AMBIENT","VOLT"},{"SYSTEM","SETUP"},{"WIFI","JOIN"},{"RESET","ALICE AP"},
    {"LINK","STATUS"},{"KEY","TEST"},{"SINGLE","READ"},{"PING","ALICE"},
    {"LAST","RESULT"},{"AMBIENT","VOLT"},{"REFRESH","HOME"},{"STOP","LASER"}
  };
  static const KeyLabel manual[16]={
    {"ALICE","+0"},{"ALICE","+1"},{"ALICE","X0"},{"ALICE","X1"},
    {"BOB","+ BASIS"},{"BOB","X BASIS"},{"SINGLE","READ"},{"LIVE","ALIGN"},
    {"ALICE","- STEP"},{"ALICE","+ STEP"},{"BOB","- STEP"},{"BOB","+ STEP"},
    {"STEP","1/5/10"},{"AMBIENT","VOLT"},{"BACK","HOME"},{"STOP","LASER"}
  };
  static const KeyLabel align[16]={
    {"SPLIT","X0 / +"},{"SPLIT","+1 / X"},{"A0 MAX","+0 / +"},{"A1 MAX","+1 / +"},
    {"A0 MAX","X0 / X"},{"A1 MAX","X1 / X"},{"START","30s LIVE"},{"AMBIENT","VOLT"},
    {"SINGLE","READ"},{"ALICE","- STEP"},{"ALICE","+ STEP"},{"BOB","- STEP"},
    {"BOB","+ STEP"},{"STEP","1/5/10"},{"BACK","HOME"},{"STOP","LASER"}
  };
  static const KeyLabel settings[16]={
    {"MIX","-5%"},{"MIX","+5%"},{"SIGNAL","-10mV"},{"SIGNAL","+10mV"},
    {"STEP","1/5/10"},{"WIFI","JOIN"},{"RESET","ALICE AP"},{"LINK","STATUS"},
    {"AMBIENT","VOLT"},{"PING","ALICE"},{"MANUAL","CONTROL"},{"ALIGN","MENU"},
    {"BB84","32 TEST"},{"FULL","16 BLOCK"},{"BACK","HOME"},{"STOP","LASER"}
  };
  const KeyLabel *items=home;
  const char *heading="BOB / HOME";
  if(p==Page::MANUAL){items=manual;heading="BOB / MANUAL";}
  else if(p==Page::ALIGN){items=align;heading="BOB / ALIGN";}
  else if(p==Page::SETTINGS){items=settings;heading="BOB / SETUP";}
  screenTitle(heading);
  drawText(tft,191,14,alice.connected()?"LINK":"OFF",alice.connected()?GREEN:RED,1);
  for(int i=0;i<16;i++){
    const int x=5+(i%4)*58;
    const int y=38+(i/4)*42;
    const uint16_t panel=(i==15)?0x4108:PANEL;
    const uint16_t edge=(i==15)?RED:0x528A;
    tft.fillRoundRect(x,y,54,39,4,panel);
    tft.drawRoundRect(x,y,54,39,4,edge);
    char num[4];snprintf(num,sizeof(num),"%02d",i+1);
    drawText(tft,x+4,y+3,num,i==15?RED:CYAN,1);
    drawText(tft,x+4,y+16,String(items[i].line1).substring(0,8),WHITE,1);
    drawText(tft,x+4,y+27,String(items[i].line2).substring(0,8),
             i==15?GOLD:MUTED,1);
  }
  String summary="A"+String(aAngle)+" B"+String(bAngle)+" step"+String(adjustStep);
  if(p==Page::SETTINGS)summary="MIX "+String(mixedThreshold)+"%  MIN "+String(minSignalMv,0)+"mV";
  else if(p==Page::MANUAL)summary+=" TX "+String(aBasis)+String(aBit);
  else if(p==Page::ALIGN)summary+="  07=LIVE";
  footer(summary);
}
void showManual(){drawMenu(Page::MANUAL);}
void showSettings(){drawMenu(Page::SETTINGS);}
void showMeasurement(const Reading& r,const String& stage){
  (void)stage;
  enterSyncView();
  const String metric="TX "+String(r.aliceBasis)+String(r.aliceBit)+
      " A0="+String(r.net0,0)+" A1="+String(r.net1,0)+"mV";
  syncUi.update(r.bobBasis,r.bit,r.bobAngle,1,1,0,0,
      metric,r.quality+" / 15 BACK",alice.connected(),false);
}
bool statsScreenReady=false;
void showStats(uint32_t slot,uint32_t total){
  // Restore the familiar large +/X and 0/1/? graphics from the old
  // WiFi SYNC version. No fillScreen() between slots.
  if(!statsScreenReady){enterSyncView();statsScreenReady=true;}
  const String metric="BLOCK "+String(stats.accepted)+"/"+String(BLOCKS_NEEDED)+
    " P:"+String(pendingCount)+" REJ:"+String(stats.rejected)+
    " Q:"+(stats.qberSamples?String(100.0f*stats.errors/stats.qberSamples,1):"--")+"%";
  syncUi.update(lastReading.bobBasis,lastReading.bit,
     lastReading.bobAngle,slot,total,stats.matchedValid,stats.errors,
     metric,(opticalStage.isEmpty()?lastReading.quality:opticalStage)+
           " / 16 STOP",alice.connected(),false);
}
BlockReportData makeReport(const String& verdict){
  BlockReportData r;
  r.trains=stats.trains;r.slots=stats.total;
  r.matched=stats.matched;r.valid=stats.matchedValid;
  r.accepted=stats.accepted;r.rejected=stats.rejected;
  r.qberErrors=stats.errors;r.qberSamples=stats.qberSamples;
  r.result=verdict;
  r.voltage=voltageStats.report();
  return r;
}
void showSummary(const String& verdict){
  summaryData=makeReport(verdict);
  view=View::OTHER;
  reportView.start(summaryData,REPORT_HOLD_MS);
}
bool holdReport(uint32_t ms=REPORT_HOLD_MS){
  const uint32_t start=millis();
  while(!elapsed(start,ms)){
    if(pollAbort()||!alice.connected())return false;
    reportView.tick();delay(10);
  }
  reportView.stop();return true;
}
void safeStop(){
  if(alice.connected())send(alice,"STOP");
  // Force-close connection so Alice fails closed even if STOP is lost.
  alice.stop();
  setNote("STOP: connection closed; laser fails OFF");
}
// Read newline-delimited TCP replies without blocking the emergency keypad.
bool waitLine(String& out,uint32_t timeoutMs){
  const uint32_t start=millis();out="";
  while(!elapsed(start,timeoutMs)){
    if(pollAbort())return false;
    if(!alice.connected())return false;
    while(alice.available()){
      const char c=char(alice.read());
      if(c=='\n'){out=lineBuffer;lineBuffer="";out.trim();return !out.isEmpty();}
      if(c!='\r'){
        if(lineBuffer.length()>=512){lineBuffer="";return false;}
        lineBuffer+=c;
      }
    }
    delay(2);
  }
  return false;
}
bool transact(const String& command,const String& expect,uint32_t timeoutMs=5500){
  if(abortRequested)return false;
  if(!send(alice,command)){setNote("SEND FAILED: "+command);return false;}
  String response;
  if(!waitLine(response,timeoutMs)){setNote("NO REPLY: "+command);return false;}
  if(field(response,0)!=expect){setNote("BAD REPLY: "+response);return false;}
  return true;
}
// Alice has no ADS1115: Bob sends actual detector statistics just before
// starting the common 10-second results page. Both devices show same values.
bool showSummaryOnBoth(const String& verdict){
  const BlockReportData r=makeReport(verdict);
  const String packet="SHOWREPORT,"+verdict+","+voltageFields(r.voltage);
  // Alice's receive line buffer is 256 chars; warn on unexpected growth.
  if(packet.length()>240){setNote("RESULTS PACKET TOO LONG");return false;}
  if(!transact(packet,"REPORTOK",5000))return false;
  Serial.printf("#BOB_VOLTAGE_SUMMARY,n=%lu,A0_min=%.1f,A0_avg=%.1f,A0_max=%.1f,"
    "A1_min=%.1f,A1_avg=%.1f,A1_max=%.1f,total_min=%.1f,total_avg=%.1f,total_max=%.1f,"
    "ambient_A0_avg=%.1f,ambient_A1_avg=%.1f\n",
    (unsigned long)r.voltage.readings,
    r.voltage.netA0Min,r.voltage.netA0Avg,r.voltage.netA0Max,
    r.voltage.netA1Min,r.voltage.netA1Avg,r.voltage.netA1Max,
    r.voltage.totalMin,r.voltage.totalAvg,r.voltage.totalMax,
    r.voltage.ambientA0Avg,r.voltage.ambientA1Avg);
  showSummary(verdict);
  return holdReport();
}
// Bob can reconnect without rebooting. No login or external router.
bool connectAlice(bool showProgress=true,bool fast=false){
  abortRequested=false;
  if(alice.connected())return true;
  alice.stop();lineBuffer="";
  if(showProgress){screenTitle("BOB / WIFI JOIN");
    lcdRow(tft,54,"JOINING AP...",GOLD,2);}
  WiFi.mode(WIFI_STA);WiFi.setSleep(false);
  WiFi.config(IPAddress(10,0,0,2),IPAddress(10,0,0,1),
    IPAddress(255,255,255,0),IPAddress(0,0,0,0));
  if(WiFi.status()!=WL_CONNECTED){
    WiFi.disconnect();delay(100);WiFi.begin(SSID,PASS);
    const uint32_t begin=millis();
    while(WiFi.status()!=WL_CONNECTED&&!elapsed(begin,fast?1800:12000)){
      if(pollAbort())return false;
      delay(60);
    }
  }
  if(WiFi.status()!=WL_CONNECTED){setNote("WIFI JOIN FAILED");return false;}
  const uint32_t tcpStart=millis();
  while(!alice.connected()&&!elapsed(tcpStart,fast?1200:4500)){
    if(pollAbort())return false;
    if(alice.connect(IPAddress(10,0,0,1),TCP_PORT))break;
    delay(180);
  }
  if(!alice.connected()){setNote("ALICE TCP FAILED");return false;}
  alice.setNoDelay(true);
  if(!send(alice,"HELLO,BOB_V7")){alice.stop();return false;}
  String response;
  if(!waitLine(response,fast?900:3500)||response!="HELLO,ALICE_V7"){
    setNote("ALICE VERSION/HELLO ERROR: "+response);
    alice.stop();return false;
  }
  setNote("LINKED TO ALICE");return true;
}
bool ensureLink(){
  if(abortRequested)return false;
  if(alice.connected())return true;
  return connectAlice();
}
void wifiRejoin(){
  safeStop();abortRequested=false;
  WiFi.disconnect(true,false);
  delay(200);
  const bool ok=connectAlice();
  if(ok)transact("PING","PONG",3000);
  showLink();
}
void wifiResetAlice(){
  abortRequested=false;
  if(!ensureLink()){showLink();return;}
  if(!transact("RESETAP","RESETAPOK",3500)){
    setNote("AP reset ACK uncertain");
  }
  alice.stop();WiFi.disconnect();
  delay(1000);connectAlice();showLink();
}
void moveBob(int newAngle){
  newAngle=constrain(newAngle,0,180);
  if(newAngle==bAngle)return;
  const int delta=abs(newAngle-bAngle);
  writeServo(BOB_HWP_PIN,0,newAngle);
  bAngle=newAngle;
  delayWithStop(SERVO_SETTLE_MIN_MS+SERVO_MS_PER_DEG*delta);
}
bool setAliceAngle(int angle,bool live=false){
  angle=constrain(angle,0,180);
  if(!ensureLink())return false;
  const String op=live?"AANGLE":"SET";
  if(!transact(op+","+String(angle),live?"AANGLEOK":"SETOK"))return false;
  aAngle=angle;return true;
}
Samples capture(uint8_t samples){
  Samples s;
  if(!adcOK){s.good=false;return s;}
  for(uint8_t i=0;i<samples;i++){
    if(pollAbort()){s.good=false;break;}
    const int16_t a0=ads.readADC_SingleEnded(0);
    const int16_t a1=ads.readADC_SingleEnded(1);
    if(a0<0||a1<0){s.good=false;break;}
    s.sum0+=a0;s.sum1+=a1;
    s.min0=std::min(s.min0,a0);s.max0=std::max(s.max0,a0);
    s.min1=std::min(s.min1,a1);s.max1=std::max(s.max1,a1);
    if(a0>=ADC_CLIP_COUNTS||a1>=ADC_CLIP_COUNTS)s.clips++;
  }
  return s;
}
float mvAvg(int32_t sum,int n){return float(sum)*ADC_MV_PER_COUNT/float(n);}
void printCsvHeader(){
  Serial.println("CSV,id,mode,a_basis,a_bit,a_deg,b_basis,b_deg,dark_a0_mv,dark_a1_mv,"
    "net_a0_mv,net_a1_mv,total_mv,contrast_pct,drift_mv,a0_range_counts,"
    "a1_range_counts,clipped,rx_bit,quality,matched,error");
}
void printCsv(const Reading& r,const String& mode){
  const bool matched=r.aliceBasis==r.bobBasis;
  const int error=(matched&&r.valid&&r.bit!=r.aliceBit)?1:0;
  Serial.printf("CSV,%lu,%s,%c,%u,%d,%c,%d,%.2f,%.2f,%.2f,%.2f,%.2f,"
    "%.2f,%.2f,%d,%d,%d,%d,%s,%d,%d\n",
    (unsigned long)r.id,mode.c_str(),r.aliceBasis,unsigned(r.aliceBit),r.aliceAngle,
    r.bobBasis,r.bobAngle,r.dark0,r.dark1,r.net0,r.net1,r.net0+r.net1,
    r.contrast,r.drift,r.range0,r.range1,r.clipped,r.bit,
    r.quality.c_str(),int(matched),error);
}
// Transaction: Alice's state is hidden until Bob reads and asks for OFF.
// Bob performs dark / laser-on / post-dark sampling with the same ADS1115
// settings as the successful 4.5m test.
bool measure(bool randomAlice,char wantedBobBasis,Reading& r){
  if(!ensureLink()||!adcOK)return false;
  r=Reading();r.id=nextId++;
  r.bobBasis=wantedBobBasis;
  r.bobAngle=wantedBobBasis=='+'?CAL_B_PLUS:CAL_B_X;
  if(!randomAlice){
    r.bobAngle=bAngle;
    r.aliceAngle=aAngle;r.aliceBasis=aBasis;r.aliceBit=aBit;
  }
  moveBob(r.bobAngle);
  if(abortRequested)return false;
  const String id=String(r.id);
  String cmd="PREP,"+id+",R";
  if(!randomAlice)cmd="PREP,"+id+",M,"+String(aAngle)+","+String(aBasis)+","+String(aBit);
  if(!transact(cmd,"READY",6000))return false;
  const Samples pre=capture(DARK_SAMPLES);
  if(!pre.good||abortRequested)return false;
  if(!send(alice,"GO,"+id))return false;
  String response;
  if(!waitLine(response,2000)||field(response,0)!="ON"||field(response,1)!=id){
    setNote("GO/ON handshake error");return false;
  }
  const Samples light=capture(LIGHT_SAMPLES);
  // Extinguish IMMEDIATELY after acquisition, before any display operations.
  if(!send(alice,"OFF,"+id))return false;
  if(!waitLine(response,2500)||field(response,0)!="DONE"||field(response,1)!=id){
    setNote("OFF/DONE handshake error");return false;
  }
  if(!randomAlice){
    r.aliceBasis=basisFromToken(field(response,2));
    r.aliceBit=uint8_t(field(response,3).toInt());
    r.aliceAngle=field(response,4).toInt();
  }else{
    // In automatic mode DONE only acknowledges light OFF. The sender's
    // basis is not revealed until the entire 32-slot train completes.
    if(response!="DONE,"+id)return false;
    r.aliceBasis='?';r.aliceAngle=-1;
  }
  const Samples post=capture(DARK_SAMPLES);
  if(!post.good||!light.good||abortRequested)return false;
  r.dark0=0.5f*(mvAvg(pre.sum0,DARK_SAMPLES)+mvAvg(post.sum0,DARK_SAMPLES));
  r.dark1=0.5f*(mvAvg(pre.sum1,DARK_SAMPLES)+mvAvg(post.sum1,DARK_SAMPLES));
  r.net0=fmaxf(0.0f,mvAvg(light.sum0,LIGHT_SAMPLES)-r.dark0);
  r.net1=fmaxf(0.0f,mvAvg(light.sum1,LIGHT_SAMPLES)-r.dark1);
  const float total=r.net0+r.net1;
  r.contrast=total>0?100.0f*fabsf(r.net0-r.net1)/total:0.0f;
  r.drift=fmaxf(fabsf(mvAvg(pre.sum0,DARK_SAMPLES)-mvAvg(post.sum0,DARK_SAMPLES)),
                  fabsf(mvAvg(pre.sum1,DARK_SAMPLES)-mvAvg(post.sum1,DARK_SAMPLES)));
  r.clipped=pre.clips+light.clips+post.clips;
  r.range0=light.max0-light.min0;r.range1=light.max1-light.min1;
  if(r.clipped){r.quality="CLIPPED";}
  else if(total<minSignalMv){r.quality="WEAK";}
  else if(r.drift>maxDarkDriftMv){r.quality="UNSTABLE";}
  else if(r.contrast<float(mixedThreshold)){r.quality="MIXED";}
  else {r.quality="OK";r.valid=true;r.bit=r.net0>=r.net1?0:1;}
  lastReading=r;
  return true;
}
void manualMeasure(){
  abortRequested=false;
  if(!ensureLink()){showLink();return;}
  running=true;Reading r;
  const bool ok=measure(false,bBasis,r);
  running=false;
  if(!ok){safeStop();abortRequested=false;setNote("MANUAL MEASUREMENT FAILED");showManual();return;}
  printCsv(r,"MANUAL");
  showMeasurement(r,"BOB / MANUAL RX");
}
// Alice's bit is never sent per slot in the automatic block protocol.
// Basis announcements happen only AFTER 32 measurements have finished.
void resetBlockData(){
  stats=Stats();lastHash="";lastReading=Reading();
  voltageStats=VoltageAccumulator();
  pendingCount=0;blockAttempt=0;statsScreenReady=false;opticalStage="";
  memset(pendingBob,0,sizeof(pendingBob));
  memset(keyMaterial,0,sizeof(keyMaterial));
}
bool protocolLine(String& out,const String& opcode,uint32_t ms=5500){
  if(!waitLine(out,ms)){
    setNote("TIMEOUT WAIT "+opcode);return false;
  }
  if(field(out,0)!=opcode){
    setNote("EXPECTED "+opcode+" RECEIVED "+out);return false;
  }
  return true;
}
void removeAttemptBits(){
  if(pendingCount>=BLOCK_BITS){
    pendingCount-=BLOCK_BITS;
    memmove(pendingBob,pendingBob+BLOCK_BITS,pendingCount);
  }
}
bool attemptBlock(){
  if(pendingCount<BLOCK_BITS)return true;
  String r;
  if(!protocolLine(r,"SAMPLE"))return false;
  const uint32_t attempt=uint32_t(field(r,1).toInt());
  if(attempt!=blockAttempt+1){setNote("BLOCK NUMBER ERROR");return false;}
  const String maskText=field(r,2),sampleText=field(r,3);
  if(maskText.length()!=16||sampleText.length()!=16){setNote("BAD SAMPLE HEX");return false;}
  const uint64_t sampleMask=toU64(maskText);
  if(__builtin_popcountll(sampleMask)!=QBER_SAMPLE_BITS){setNote("SAMPLE MASK BAD");return false;}
  const uint64_t original=pack64(pendingBob);
  const uint64_t aliceSamples=toU64(sampleText);
  const unsigned bad=__builtin_popcountll((original^aliceSamples)&sampleMask);
  ++blockAttempt;stats.attempts++;
  stats.errors+=bad;stats.qberSamples+=QBER_SAMPLE_BITS;
  opticalStage="QBER 8/64";showStats(TRAIN_SLOTS,TRAIN_SLOTS);
  setNote("QBER SAMPLE "+String(bad)+"/8 - BLOCK "+String(attempt));
  if(!send(alice,"QBER,"+String(attempt)+","+String(bad)))return false;
  if(!protocolLine(r,"SYND"))return false;
  if(uint32_t(field(r,1).toInt())!=attempt)return false;
  const uint32_t expectedSyn=toU32(field(r,2));
  opticalStage="LDPC-LIKE 24x64";showStats(TRAIN_SLOTS,TRAIN_SLOTS);
  uint64_t corrected=original&~sampleMask;
  // A high per-block public sample error rate must reject the block too.
  const bool candidate=(bad<=1u) && sparseReconcile(corrected,expectedSyn,parityH);
  if(!send(alice,"RECON,"+String(attempt)+","+(candidate?"CAND":"BAD")))return false;
  bool verified=false;
  if(candidate){
    if(!protocolLine(r,"BLOCKTAG"))return false;
    if(uint32_t(field(r,1).toInt())!=attempt)return false;
    verified=(field(r,2)==blockCheck(corrected,sampleMask));
    if(!send(alice,"VERDICT,"+String(attempt)+","+(verified?"OK":"BAD")))return false;
  }
  if(!protocolLine(r,"BLOCKDONE"))return false;
  if(uint32_t(field(r,1).toInt())!=attempt)return false;
  const bool accepted=field(r,2)=="OK";
  if(accepted){
    if(!candidate||!verified||stats.accepted>=BLOCKS_NEEDED){
      setNote("BLOCK ACCEPTED WITHOUT PROOF");return false;
    }
    retainUnsampled(keyMaterial,stats.accepted,corrected,sampleMask);
    stats.accepted++;
    opticalStage="BLOCK ACCEPTED";
    setNote("BLOCK "+String(stats.accepted)+"/"+String(BLOCKS_NEEDED)+" ACCEPTED");
  }else{
    stats.rejected++;opticalStage="BLOCK REJECTED";
    setNote("BLOCK "+String(attempt)+" REJECTED; RETRY");
  }
  if(uint32_t(field(r,3).toInt())!=stats.accepted ||
     uint32_t(field(r,4).toInt())!=stats.rejected){
    setNote("ALICE/BOB BLOCK COUNT DIFFERENCE");return false;
  }
  removeAttemptBits();
  showStats(TRAIN_SLOTS,TRAIN_SLOTS);
  return !abortRequested;
}
bool runOpticalTrain(){
  const uint32_t id=++nextId;
  String s;
  if(!send(alice,"TRBEGIN,"+String(id))||!protocolLine(s,"TRREADY"))return false;
  if(uint32_t(field(s,1).toInt())!=id)return false;
  opticalStage="RX OPTICAL";
  String bobBasisString="",validString="";
  bobBasisString.reserve(TRAIN_SLOTS);validString.reserve(TRAIN_SLOTS);
  for(unsigned i=0;i<TRAIN_SLOTS;i++){
    if(pollAbort())return false;
    const char selected=toBasis(uint8_t(esp_random()&1));
    Reading r;
    if(!measure(true,selected,r))return false;
    frameReadings[i]=r;
    bobBasisString+=(selected=='X'?'1':'0');
    validString+=(r.valid?'1':'0');
    lastReading=r;
    stats.total++;
    voltageStats.add(r);
    // No basis comparison or true Alice bit on this screen during the train.
    showStats(i+1,TRAIN_SLOTS);
    Serial.printf("#RX_PRE_SIFT,train=%lu,slot=%u,bob_basis=%c,"
      "rx_bit=%d,net0=%.1f,net1=%.1f,contrast=%.1f,quality=%s\n",
      (unsigned long)(stats.trains+1),i+1,selected,r.bit,
      r.net0,r.net1,r.contrast,r.quality.c_str());
    if(r.clipped)stats.clipped++;
    if(r.quality=="WEAK")stats.weak++;
    if(r.quality=="UNSTABLE")stats.unstable++;
  }
  // Post-train basis announcement and sifting (no Alice bit disclosure).
  opticalStage="SIFT AFTER TRAIN";
  showStats(TRAIN_SLOTS,TRAIN_SLOTS);
  if(!send(alice,"TRDONE,"+String(id)+","+bobBasisString+","+validString))return false;
  if(!protocolLine(s,"META"))return false;
  if(uint32_t(field(s,1).toInt())!=id)return false;
  const String aliceBasisString=field(s,2);
  if(!bitString32(aliceBasisString))return false;
  unsigned newValid=0;
  for(unsigned i=0;i<TRAIN_SLOTS;i++){
    const bool same=aliceBasisString[i]==bobBasisString[i];
    if(same){
      stats.matched++;
      if(frameReadings[i].valid){
        if(pendingCount>=sizeof(pendingBob)){setNote("SIFT BUFFER OVERFLOW");return false;}
        pendingBob[pendingCount++]=uint8_t(frameReadings[i].bit);
        stats.matchedValid++;newValid++;
      }else stats.matchedInvalid++;
    }else{
      stats.mismatched++;
      if(frameReadings[i].quality=="MIXED")stats.mixedWrong++;
    }
  }
  if(field(s,3).toInt()!=int(newValid) ||
     field(s,4).toInt()!=int(pendingCount)){
    setNote("SIFT COUNT DIFF: "+s);return false;
  }
  if(!send(alice,"SIFTACK,"+String(id)+","+String(pendingCount)))return false;
  if(!protocolLine(s,"SIFTOK"))return false;
  if(field(s,1).toInt()!=int(id)||field(s,2).toInt()!=int(pendingCount))return false;
  stats.trains++;
  setNote("SIFT TRAIN "+String(stats.trains)+": +"+String(newValid)+
    " bits, pending "+String(pendingCount));
  Serial.printf("#TRAIN_SIFT,%lu,matched=%d,valid=%u,pending=%u\n",
    (unsigned long)stats.trains,stats.matched,unsigned(newValid),unsigned(pendingCount));
  if(pendingCount>=BLOCK_BITS && !attemptBlock())return false;
  showStats(TRAIN_SLOTS,TRAIN_SLOTS);
  return true;
}

// Alice and Bob independently compute Toeplitz(raw[896], public seed[1151]).
// Bob sends only a short confirmation tag. Key material is NOT transmitted.
bool produceAgreedKey(uint32_t sequence){
  if(stats.accepted!=BLOCKS_NEEDED || !alice.connected())return false;
  String response;
  opticalStage="TOEPLITZ PA";showStats(TRAIN_SLOTS,TRAIN_SLOTS);
  if(!send(alice,"PASTART,"+activeSessionNonce))return false;
  if(!protocolLine(response,"PASEED",15000))return false;
  uint8_t publicSeed[TOEPLITZ_SEED_BYTES];
  uint8_t derived[32];
  if(!parseHexBytes(field(response,1),publicSeed,TOEPLITZ_SEED_BYTES)){
    setNote("TOEPLITZ SEED BAD");return false;
  }
  toeplitzPA(keyMaterial,publicSeed,derived);
  const String tag=paTag(derived);
  if(!send(alice,"PATAG,"+tag))return false;
  if(!protocolLine(response,"PAOK",5000)||field(response,1)!=tag){
    setNote("PRIVACY AMPLIFICATION KEY MISMATCH");return false;
  }
  const String keyHex=hexBytes(derived,32);
  lastHash=keyHex;
  summaryData=makeReport("KEY AGREED");
  const String qber=summaryData.qberSamples?
    String(100.0f*summaryData.qberErrors/summaryData.qberSamples,2):"0.00";
  view=View::OTHER;
  keyView.start(keyHex,sequence,MATERIAL_BITS,qber);
  chime.signal(SoftChime::KEY_READY);
  Serial.println("#BOB_TOEPLITZ256_KEY,"+keyHex);
  Serial.printf("#PA_COMPLETE,raw_bits=%u,output_bits=%u,good=%lu,bad=%lu,qber=%.2f\n",
    unsigned(MATERIAL_BITS),unsigned(KEY_BITS),
    (unsigned long)stats.accepted,(unsigned long)stats.rejected,
    summaryData.qberSamples?100.0f*summaryData.qberErrors/summaryData.qberSamples:0.0f);
  while(!keyView.done()){
    if(pollAbort()||!alice.connected()){
      keyView.stop();return false;
    }
    keyView.tick();delay(10);
  }
  keyView.stop();
  return showSummaryOnBoth("KEY AGREED");
}

// '32' = one TRAIN and its block/sift diagnostics, no premature key.
// '128' = FULL, accumulate 16 successfully reconciled 64-position blocks.
bool runBB84Session(int slots,uint32_t sequence){
  resetBlockData();
  activeSessionNonce=hexNonce(esp_random());
  if(!transact("SESSION,"+String(slots)+","+activeSessionNonce,"SESSIONOK"))return false;
  enterSyncView();syncUi.invalidate();statsScreenReady=true;
  chime.signal(SoftChime::NEXT_SESSION);
  printCsvHeader();
  setNote("V7 SESSION "+String(sequence)+" START");
  const bool full=(slots!=32);
  bool limitReached=false;
  while(!abortRequested){
    if(!full&&stats.trains>=1)break;
    if(full&&stats.accepted>=BLOCKS_NEEDED)break;
    if(stats.trains>=MAX_TRAINS_PER_SESSION ||
       stats.rejected>=MAX_REJECTED_BLOCKS ||
       (stats.attempts>=8 &&
        stats.errors*100u>MAX_SAMPLED_QBER_PERCENT*stats.qberSamples)){
      limitReached=true;break;
    }
    if(!runOpticalTrain())return false;
    // No immediate Alice/Bob basis comparison while the slot pulses run.
  }
  if(abortRequested)return false;
  if(full&&stats.accepted==BLOCKS_NEEDED)return produceAgreedKey(sequence);
  const String outcome=limitReached?"SESSION REJECTED":"TRAIN COMPLETE";
  setNote(full?"NO KEY - BLOCK LIMIT":"32-SLOT TRAIN ENDED - NO KEY YET");
  return showSummaryOnBoth(outcome);
}
void runBB84(int slots){
  abortRequested=false;pendingControlKey=0;
  if(!ensureLink()||!adcOK){showLink();return;}
  const bool continuous=(slots!=32);
  running=true;
  uint32_t number=0;
  bool ok=true;
  do{
    ok=runBB84Session(slots,++number);
    if(!ok)break;
  }while(continuous&&!abortRequested);
  if(!ok||abortRequested){
    safeStop();lastSummary="STOPPED / "+String(stats.total)+" SLOTS";
    setNote(lastSummary);
  }
  running=false;abortRequested=false;pendingControlKey=0;
  drawMenu(Page::HOME);
}

void darkOnly(){
  abortRequested=false;
  screenTitle("BOB / AMBIENT");
  lcdRow(tft,50,"LASER MUST OFF",GOLD,1);
  // Only Bob reads ADS. The standalone lamp measurements assume laser OFF.
  if(!adcOK){lcdRow(tft,80,"ADS MISSING",RED,2);return;}
  if(alice.connected())transact("STOP","STOPPED",2000);
  const Samples s=capture(LIGHT_SAMPLES);
  if(!s.good)return;
  lcdRow(tft,75,"A0: "+String(mvAvg(s.sum0,LIGHT_SAMPLES),1),WHITE,2);
  lcdRow(tft,105,"A1: "+String(mvAvg(s.sum1,LIGHT_SAMPLES),1),WHITE,2);
  lcdRow(tft,137,"RANGE: "+String(s.max0-s.min0)+"/"+
         String(s.max1-s.min1),CYAN,1);
  lcdRow(tft,162,"CLIPS: "+String(s.clips),GOLD,1);
  footer("15=MENU  16=STOP");
}
// Live alignment is a deliberately bounded continuous-laser mode. Dark samples
// are taken before requesting ALIGN, then raw and net detector voltages shown.
// Operator can adjust A/B angles while the laser is lit, but 30s laser timer
// is enforced independently by ALICE even if the network hangs.
void liveAlignment(){
  abortRequested=false;pendingControlKey=0;
  if(!ensureLink()||!adcOK){showLink();return;}
  running=true;aligningLive=true;
  const Samples dark=capture(DARK_SAMPLES);
  if(!dark.good||abortRequested){running=false;aligningLive=false;return;}
  if(!transact("ALIGN,"+String(aAngle),"ALIGNON",6000)){
    running=false;aligningLive=false;safeStop();abortRequested=false;showLink();return;
  }
  screenTitle("BOB / LIVE ALIGN");
  drawText(tft,8,38,"30s AUTO OFF. 16=STOP",GOLD,1);
  const float d0=mvAvg(dark.sum0,DARK_SAMPLES),d1=mvAvg(dark.sum1,DARK_SAMPLES);
  const uint32_t start=millis();
  uint32_t lastUi=UINT32_MAX-500u;
  while(!elapsed(start,ALIGN_MAX_MS-1000)){
    const char k=actionKey();
    if(k=='G'){abortRequested=true;break;}
    if(k=='F')break;
    // Buttons 09/10 change ALICE, 11/12 change BOB during alignment.
    if(k=='9'||k=='A'){
      const int value=constrain(aAngle+(k=='9'?-adjustStep:adjustStep),0,180);
      if(!setAliceAngle(value,true))break;
    }
    if(k=='B'||k=='C'){
      moveBob(bAngle+(k=='B'?-adjustStep:adjustStep));
    }
    if(abortRequested)break;
    const Samples light=capture(5);
    if(!light.good)break;
    const float raw0=mvAvg(light.sum0,5),raw1=mvAvg(light.sum1,5);
    const float n0=fmaxf(0.0f,raw0-d0),n1=fmaxf(0.0f,raw1-d1);
    const float total=n0+n1;
    const float pc0=total>0?100*n0/total:0;
    const float pc1=total>0?100*n1/total:0;
    const float contrast=total>0?100*fabsf(n0-n1)/total:0;
    if(elapsed(lastUi,400)){
      lastUi=millis();
    smoothRow(56,"A0: "+String(n0,1)+" mV",CYAN,2);
    smoothRow(81,"A1: "+String(n1,1)+" mV",MINT,2);
    smoothRow(108,String(pc0,1)+"% / "+String(pc1,1)+"%",GOLD,2);
    smoothRow(134,"DIFF: "+String(contrast,1)+"%",WHITE,2);
    smoothRow(159,"RAW: "+String(raw0,0)+" / "+String(raw1,0),MUTED,1);
    smoothRow(179,"A:"+String(aAngle)+" B:"+String(bAngle)+
                  " left:"+String((ALIGN_MAX_MS-(millis()-start))/1000)+"s",WHITE,1);
    smoothRow(198,"09/10=A  11/12=B",GOLD,1);
    footer(light.clips?"ADC SAT - STOP/SHIELD":"15=EXIT 16=STOP");
    }
    Serial.printf("ALIGN,A=%d,B=%d,raw0=%.2f,raw1=%.2f,net0=%.2f,net1=%.2f,"
      "pct0=%.1f,pct1=%.1f,clip=%d\n",aAngle,bAngle,raw0,raw1,n0,n1,pc0,pc1,light.clips);
    if(light.clips)break;
    if(!delayWithStop(210))break;
  }
  // Always request shutdown, regardless of how alignment ends.
  if(alice.connected())send(alice,"ALIGNOFF");
  String reply;
  if(alice.connected())waitLine(reply,1800);
  if(abortRequested||!alice.connected())safeStop();
  running=false;aligningLive=false;abortRequested=false;pendingControlKey=0;
  drawMenu(Page::ALIGN);
}
void selectManual(char basis,int bit){
  aBasis=basis;aBit=uint8_t(bit);aAngle=angleFor(aBasis,aBit);
  abortRequested=false;
  if(!setAliceAngle(aAngle))setNote("ALICE MANUAL SET FAILED");
  showManual();
}
void selectBob(char basis){
  bBasis=basis;abortRequested=false;
  moveBob(basis=='+'?CAL_B_PLUS:CAL_B_X);showManual();
}
void stepAlice(int dir,bool live=false){
  abortRequested=false;
  const int next=constrain(aAngle+dir*adjustStep,0,180);
  if(!setAliceAngle(next,live))setNote("A ANGLE SET FAILED");
}
void cycleStep(){adjustStep=adjustStep==1?5:(adjustStep==5?10:1);}
void pingAlice(){
  abortRequested=false;
  if(ensureLink())transact("PING","PONG",2500);
  showLink();
}
void handleKey(int key){
  if(key==16){
    abortRequested=true;safeStop();abortRequested=false;
    drawMenu(Page::HOME);return;
  }
  if(page==Page::HOME||page==Page::RESULT){
    switch(key){
      case 1:runBB84(32);break;
      case 2:runBB84(128);break;
      case 3:showManual();break;
      case 4:drawMenu(Page::ALIGN);break;
      case 5:darkOnly();break;
      case 6:showSettings();break;
      case 7:wifiRejoin();break;
      case 8:wifiResetAlice();break;
      case 9:showLink();break;
      case 10:showLink();setNote("KEY "+String(key)+" OK");break;
      case 11:manualMeasure();break;
      case 12:pingAlice();break;
      case 13:showSummary(stats.accepted==BLOCKS_NEEDED?"KEY AGREED":"LAST RESULT");break;
      case 14:darkOnly();break;
      case 15:drawMenu(Page::HOME);break;
      default:break;
    }
  }else if(page==Page::MANUAL){
    switch(key){
      case 1:selectManual('+',0);break;
      case 2:selectManual('+',1);break;
      case 3:selectManual('X',0);break;
      case 4:selectManual('X',1);break;
      case 5:selectBob('+');break;
      case 6:selectBob('X');break;
      case 7:manualMeasure();break;
      case 8:liveAlignment();break;
      case 9:stepAlice(-1);showManual();break;
      case 10:stepAlice(1);showManual();break;
      case 11:abortRequested=false;moveBob(bAngle-adjustStep);showManual();break;
      case 12:abortRequested=false;moveBob(bAngle+adjustStep);showManual();break;
      case 13:cycleStep();showManual();break;
      case 14:darkOnly();break;
      case 15:drawMenu(Page::HOME);break;
    }
  }else if(page==Page::ALIGN){
    switch(key){
      case 1:aAngle=CAL_A_X_0;aBasis='X';aBit=0;moveBob(CAL_B_PLUS);drawMenu(Page::ALIGN);break;
      case 2:aAngle=CAL_A_PLUS_1;aBasis='+';aBit=1;moveBob(CAL_B_X);drawMenu(Page::ALIGN);break;
      case 3:aAngle=CAL_A_PLUS_0;aBasis='+';aBit=0;moveBob(CAL_B_PLUS);drawMenu(Page::ALIGN);break;
      case 4:aAngle=CAL_A_PLUS_1;aBasis='+';aBit=1;moveBob(CAL_B_PLUS);drawMenu(Page::ALIGN);break;
      case 5:aAngle=CAL_A_X_0;aBasis='X';aBit=0;moveBob(CAL_B_X);drawMenu(Page::ALIGN);break;
      case 6:aAngle=CAL_A_X_1;aBasis='X';aBit=1;moveBob(CAL_B_X);drawMenu(Page::ALIGN);break;
      case 7:liveAlignment();break;
      case 8:darkOnly();break;
      case 9:manualMeasure();break;
      case 10:stepAlice(-1);drawMenu(Page::ALIGN);break;
      case 11:stepAlice(1);drawMenu(Page::ALIGN);break;
      case 12:abortRequested=false;moveBob(bAngle-adjustStep);drawMenu(Page::ALIGN);break;
      case 13:abortRequested=false;moveBob(bAngle+adjustStep);drawMenu(Page::ALIGN);break;
      case 14:cycleStep();drawMenu(Page::ALIGN);break;
      case 15:drawMenu(Page::HOME);break;
    }
  }else if(page==Page::SETTINGS){
    switch(key){
      case 1:mixedThreshold=std::max(5,mixedThreshold-5);showSettings();break;
      case 2:mixedThreshold=std::min(45,mixedThreshold+5);showSettings();break;
      case 3:minSignalMv=std::max(10.0f,minSignalMv-10);showSettings();break;
      case 4:minSignalMv=std::min(200.0f,minSignalMv+10);showSettings();break;
      case 5:cycleStep();showSettings();break;
      case 6:wifiRejoin();break;
      case 7:wifiResetAlice();break;
      case 8:showLink();break;
      case 9:darkOnly();break;
      case 10:pingAlice();break;
      case 11:showManual();break;
      case 12:drawMenu(Page::ALIGN);break;
      case 13:runBB84(32);break;
      case 14:runBB84(128);break;
      case 15:drawMenu(Page::HOME);break;
    }
  }
}
void usbCommands(){
  while(Serial.available()){
    const char ch=char(Serial.read());
    if(ch=='\r'||ch=='\n'){
      serialBuffer.trim();serialBuffer.toUpperCase();
      if(serialBuffer=="STOP"){
        abortRequested=true;safeStop();abortRequested=false;
        drawMenu(Page::HOME);
      }else if(serialBuffer=="HELP"){
        Serial.println("#Bob commands: KEY <1..16>, STATUS, STOP, HELP");
      }else if(serialBuffer=="STATUS"){
        Serial.printf("#BOB_STATUS,wifi=%d,tcp=%d,adc=%d,A=%d,B=%d,thr=%d\n",
          WiFi.status()==WL_CONNECTED,alice.connected(),adcOK,aAngle,bAngle,mixedThreshold);
      }else if(serialBuffer.startsWith("KEY ")){
        const int v=serialBuffer.substring(4).toInt();
        if(v>=1&&v<=16&&!running)handleKey(v);
      }
      serialBuffer="";
    }else if(ch>=32&&ch<=126&&serialBuffer.length()<80)serialBuffer+=ch;
  }
}
void setup(){
  Serial.begin(115200);
  lcdInit(tft);beginKeypad();buildH(parityH);
  const bool speakerOK=chime.begin();
  Serial.printf("#BOB_I2S_SPEAKER,%s,quiet=900/32768\n",speakerOK?"READY":"DISABLED");
  screenTitle("BOB / BOOT");
  lcdRow(tft,65,"ADS SDA17 SCL20",CYAN,1);
  lcdRow(tft,88,"KEYPAD: 16 KEYS",CYAN,1);
  Wire.begin(BOB_SDA,BOB_SCL);Wire.setClock(100000);
  for(int tries=0;tries<8;tries++){
    if(ads.begin(ADS_ADDRESS,&Wire)){adcOK=true;break;}
    delay(250);
  }
  if(adcOK){
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_250SPS);
  }else setNote("ADS1115 NOT FOUND ON SDA17 SCL20");
  if(!attachServo(BOB_HWP_PIN,0)){
    screenTitle("BOB SERVO ERROR");
    while(true)delay(1000);
  }
  writeServo(BOB_HWP_PIN,0,CAL_B_PLUS);bAngle=CAL_B_PLUS;
  delay(500);
  drawMenu(Page::HOME);
  printCsvHeader();
  Serial.println("#BOB_V7: 32-SLOT TRAINS, QBER8, SPARSE CHECK24, TOEPLITZ896->256");
  Serial.println("#KEYPAD R=11,12,13,14 C=18,8,9,10");
  Serial.println("#Physical key 16 is STOP. BOB SCL=20 (not 18).");
  // WiFi link starts on-demand; loop retries in the background if idle.
}
void loop(){
  usbCommands();
  const char k=actionKey();
  if(k){
    Serial.printf("#KEY,physical=%d,symbol=%c\n",physicalKey(k),k);
    handleKey(physicalKey(k));
  }
  // Only join when the operator is idle. The UI remains responsive offline.
  static uint32_t lastAttempt=UINT32_MAX-10000u;
  if(!running&&!alice.connected()&&elapsed(lastAttempt,10000)){
    lastAttempt=millis();
    if(connectAlice(false,true))drawMenu(page);
    abortRequested=false;
  }
  delay(5);
}
