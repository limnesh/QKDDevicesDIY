/*
  ALICE - ColorSync V7: 32-slot trains, 64-bit sparse-parity blocks, Toeplitz PA
  ESP32-S3 N16R8 / ST7789 240x240 / AP 10.0.0.1:7777.

  HARDWARE: Alice HWP GPIO18; laser GPIO14 (user revision 2026-11-10); TFT pins in bb84_shared.h.
  VERIFIED USER HARDWARE 2026-11-10: Laser on Alice GPIO14; polarization HWP
  servo on Alice GPIO18; attenuator/decoy servo on Alice GPIO17. The
  attenuator/decoy servo GPIO17 MUST NEVER be initialized, attached or moved.
  Bob GPIO14 is separate, and is one of Bob's keypad row inputs.

  Bob is the keypad-controlled operator. Alice chooses private random
  basis/bit during automatic BB84; preparation is revealed only after
  Bob has measured. This is a CLASSICAL-LIGHT educational simulation,
  NOT quantum-secure key distribution.

  Safety: laser defaults OFF. Pulse watchdog 1400 ms; alignment watchdog
  30000 ms. TCP loss forces OFF. Use a physical laser power cutoff and
  a safe beam enclosure/dump for audience demonstrations.
*/
#include "bb84_shared.h"
#include "bb84_sync_ui.h"
#include "bb84_audio.h"
#include "bb84_key_view.h"
#include "bb84_blocks.h"
#include "bb84_block_report.h"
using namespace demo;

constexpr int ALICE_HWP_PIN=18;
constexpr int ALICE_LASER_PIN=14;  // VERIFIED user wiring: laser, active HIGH
constexpr int ALICE_ATTENUATOR_SERVO_RESERVED_PIN=17; // DO NOT INITIALIZE/ATTACH/MOVE
static_assert(ALICE_LASER_PIN != ALICE_ATTENUATOR_SERVO_RESERVED_PIN,
              "Laser and attenuator/decoy servo cannot share one pin");

WiFiServer server(TCP_PORT);
WiFiClient peer;
Adafruit_ST7789 tft(TFT_CS,TFT_DC,TFT_MOSI,TFT_SCLK,TFT_RST);
String tcpBuffer,serialBuffer,siftAlice;
int aliceAngle=-1;
char preparedBasis='+';
uint8_t preparedBit=0;
uint32_t preparedId=0,lastSiftId=UINT32_MAX;
bool prepared=false,laserOn=false,alignMode=false,pulseMode=false;
uint32_t beamStarted=0,apResetAt=0,alignedUntil=0;
uint32_t frames=0,sifted=0;
String state="BOOT",lastResult="NONE";
unsigned targetSlots=32;
SyncDashboard syncUi(tft,true);
SoftChime chime;
DemoKeyView keyView(tft,true);
bool keyDisplayActive=false, reportActive=false;
BlockReportView reportView(tft,true);
BlockReportData reportData;
uint64_t parityH[PARITY_CHECKS];
uint8_t pendingBits[BLOCK_BITS+TRAIN_SLOTS]={0};
uint8_t keyMaterial[MATERIAL_BITS]={0};
uint8_t pendingCount=0,acceptedBlocks=0;
uint32_t rejectedBlocks=0,attemptedBlocks=0,sampleErrors=0,sampleTotal=0;
uint32_t totalTrains=0,totalSlots=0,matched=0,matchedValid=0;
uint64_t sampleMask=0,keptAlice=0;
uint8_t paSeed[TOEPLITZ_SEED_BYTES]={0},paKey[32]={0};
bool paPrepared=false,paVerified=false,trainActive=false,sessionActive=false;
uint8_t frameSlot=0;
uint32_t frameId=0;
char txBasis[TRAIN_SLOTS];
uint8_t txBit[TRAIN_SLOTS];
enum class BlockPhase:uint8_t {IDLE,QBER,RECON,VERDICT};
BlockPhase blockPhase=BlockPhase::IDLE;

String sessionNonce="";
uint32_t completedSessions=0;

void drawAlice(){
  if(keyDisplayActive || reportActive)return; // Temporary results view owns TFT.
  // Original colorful WiFi SYNC dashboard: the static layout is drawn only
  // once, then only changed tiles and progress values are updated.
  const String metric="BLOCK "+String(acceptedBlocks)+"/"+String(BLOCKS_NEEDED)+
    " P:"+String(pendingCount)+" REJ:"+String(rejectedBlocks)+
    " Q:"+(sampleTotal?String(float(sampleErrors)*100.0f/float(sampleTotal),1):"--")+"%";
  syncUi.update(prepared ? preparedBasis : '?',
                prepared ? int(preparedBit) : -1,
                aliceAngle, frames, targetSlots, matchedValid, sampleErrors,
                metric, state+(laserOn?" LASER ON":""),
                peer.connected(), laserOn);
}

void laserOff(){
  digitalWrite(ALICE_LASER_PIN,LOW);
  laserOn=false;alignMode=false;pulseMode=false;
}
void laserStart(bool alignment){
  digitalWrite(ALICE_LASER_PIN,HIGH);
  laserOn=true;alignMode=alignment;pulseMode=!alignment;
  beamStarted=millis();
}
void moveAlice(int angle){
  angle=constrain(angle,0,180);
  if(angle==aliceAngle)return;
  const int change=aliceAngle<0?180:abs(angle-aliceAngle);
  writeServo(ALICE_HWP_PIN,0,angle);
  aliceAngle=angle;
  const uint32_t waitMs=SERVO_SETTLE_MIN_MS + SERVO_MS_PER_DEG*change;
  const uint32_t began=millis();
  while(!elapsed(began,waitMs)){
    // Enforce alignment timeout even during a commanded servo movement.
    if(laserOn && peer && !peer.connected())laserOff();
    if(alignMode && int32_t(millis()-alignedUntil)>=0)laserOff();
    if(pulseMode && elapsed(beamStarted,MAX_PULSE_ON_MS))laserOff();
    delay(2);
  }
}
void setupAP(){
  laserOff();
  if(peer)peer.stop();
  server.end();
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);WiFi.setSleep(false);
  bool ok=WiFi.softAPConfig(IPAddress(10,0,0,1),IPAddress(10,0,0,1),IPAddress(255,255,255,0));
  ok=ok&&WiFi.softAP(SSID,PASS,1,0,2);
  if(ok)server.begin();
  state=ok?"AP READY":"AP ERROR";
  Serial.printf("#ALICE_AP,%s,10.0.0.1:%u\n",ok?"OK":"FAIL",unsigned(TCP_PORT));
  drawAlice();
}
void finishPulse(bool success){
  // This is the only normal path out of a pulse. Always extinguish first.
  laserOff();
  if(success){
    // Automatic BB84: Alice keeps bit and basis private until the train ends.
    // Manual demonstration still reports the explicitly selected state.
    const String r=trainActive ? ("DONE,"+String(preparedId)) :
      ("DONE,"+String(preparedId)+","+String(preparedBasis)+","+
        String(preparedBit)+","+String(aliceAngle));
    send(peer,r);
    lastResult="TX "+String(preparedBasis)+String(preparedBit);
    state="SLOT DONE";
  }else{
    send(peer,"ABORTED,"+String(preparedId));
    state="PULSE STOP";
  }
  drawAlice();
}
void discardAttempt(){
  if(pendingCount>=BLOCK_BITS){
    pendingCount-=BLOCK_BITS;
    memmove(pendingBits,pendingBits+BLOCK_BITS,pendingCount);
  }
  blockPhase=BlockPhase::IDLE;
}
void startCandidateBlock(){
  if(pendingCount<BLOCK_BITS || blockPhase!=BlockPhase::IDLE)return;
  ++attemptedBlocks;
  sampleMask=chooseSample8();
  keptAlice=pack64(pendingBits)&~sampleMask;
  blockPhase=BlockPhase::QBER;
  send(peer,"SAMPLE,"+String(attemptedBlocks)+","+hex64(sampleMask)+","+
            hex64(pack64(pendingBits)&sampleMask));
  state="QBER 8 SAMPLES";drawAlice();
}
void resetBlockSession(){
  memset(pendingBits,0,sizeof(pendingBits));
  memset(keyMaterial,0,sizeof(keyMaterial));
  memset(txBasis,0,sizeof(txBasis));memset(txBit,0,sizeof(txBit));
  pendingCount=acceptedBlocks=0;
  attemptedBlocks=rejectedBlocks=sampleErrors=sampleTotal=0;
  totalTrains=totalSlots=matched=matchedValid=0;
  frameSlot=0;frameId=0;trainActive=false;
  blockPhase=BlockPhase::IDLE;paPrepared=false;paVerified=false;
  memset(paKey,0,sizeof(paKey));memset(paSeed,0,sizeof(paSeed));
  reportData=BlockReportData();
}
void updateFinalReport(const String& outcome){
  reportData.trains=totalTrains;reportData.slots=totalSlots;
  reportData.matched=matched;reportData.valid=matchedValid;
  reportData.accepted=acceptedBlocks;reportData.rejected=rejectedBlocks;
  reportData.qberErrors=sampleErrors;reportData.qberSamples=sampleTotal;
  reportData.result=outcome;
}
void process(String cmd){
  cmd.trim();if(cmd.length()==0)return;
  const String op=field(cmd,0);
  Serial.println("#RX,"+cmd);

  if(op=="HELLO"){
    send(peer,"HELLO,ALICE_V7");state="BOB CONNECTED";drawAlice();return;
  }
  if(op=="PING"){
    send(peer,"PONG,"+String(aliceAngle)+","+(laserOn?"ON":"OFF"));return;
  }
  if(op=="STOP"||op=="ALIGNOFF"){
    if(keyDisplayActive){keyDisplayActive=false;keyView.stop();syncUi.invalidate();}
    if(reportActive){reportActive=false;reportView.stop();syncUi.invalidate();}
    if(pulseMode){finishPulse(false);return;}
    laserOff();prepared=false;trainActive=false;sessionActive=false;
    blockPhase=BlockPhase::IDLE;
    send(peer,op=="STOP"?"STOPPED":"ALIGNOFFOK");
    state="STOPPED";drawAlice();return;
  }
  if(op=="RESETAP"){
    laserOff();send(peer,"RESETAPOK");
    apResetAt=millis()+400;state="WIFI RESET";drawAlice();return;
  }
  if(op=="SET"){
    if(laserOn){send(peer,"ERR,LASER_ACTIVE");return;}
    const int angle=field(cmd,1).toInt();
    if(angle<0||angle>180){send(peer,"ERR,ANGLE");return;}
    moveAlice(angle);send(peer,"SETOK,"+String(aliceAngle));
    state="MANUAL ANGLE";drawAlice();return;
  }
  if(op=="AANGLE"){
    if(!alignMode){send(peer,"ERR,NOT_ALIGN");return;}
    const int angle=field(cmd,1).toInt();
    if(angle<0||angle>180){send(peer,"ERR,ANGLE");return;}
    moveAlice(angle);send(peer,"AANGLEOK,"+String(aliceAngle));
    state="ALIGN MANUAL";drawAlice();return;
  }
  if(op=="ALIGN"){
    // Start a fresh, time-limited alignment. Bob samples DARK BEFORE this.
    laserOff();prepared=false;
    const int angle=field(cmd,1).toInt();
    if(angle<0||angle>180){send(peer,"ERR,ANGLE");return;}
    moveAlice(angle);
    laserStart(true);
    alignedUntil=millis()+ALIGN_MAX_MS;
    delay(LASER_WARMUP_MS);
    send(peer,"ALIGNON,"+String(aliceAngle)+","+String(ALIGN_MAX_MS));
    state="ALIGN 30S MAX";drawAlice();return;
  }
  if(op=="SESSION"){
    if(keyDisplayActive){keyDisplayActive=false;keyView.stop();}
    if(reportActive){reportActive=false;reportView.stop();}
    syncUi.invalidate();
    laserOff();prepared=false;
    sessionNonce=field(cmd,2);
    if(sessionNonce.length()!=8){send(peer,"ERR,NONCE");return;}
    ++completedSessions;
    resetBlockSession();sessionActive=true;
    targetSlots=TRAIN_SLOTS;frames=0;sifted=0;
    send(peer,"SESSIONOK,"+field(cmd,1));
    chime.signal(SoftChime::NEXT_SESSION);
    state="BLOCK BB84 V7";drawAlice();return;
  }
  if(op=="TRBEGIN"){
    const uint32_t id=uint32_t(strtoul(field(cmd,1).c_str(),nullptr,10));
    if(!sessionActive || trainActive || laserOn || blockPhase!=BlockPhase::IDLE){
      send(peer,"ERR,TRBEGIN");return;
    }
    frameId=id;frameSlot=0;frames=0;trainActive=true;
    send(peer,"TRREADY,"+String(id));
    state="TRAIN "+String(totalTrains+1);drawAlice();return;
  }
  if(op=="TRDONE"){
    const uint32_t id=uint32_t(strtoul(field(cmd,1).c_str(),nullptr,10));
    const String bobBases=field(cmd,2),validBits=field(cmd,3);
    if(!trainActive || id!=frameId || frameSlot!=TRAIN_SLOTS || laserOn ||
       !bitString32(bobBases)||!bitString32(validBits)){
      send(peer,"ERR,TRDONE");return;
    }
    trainActive=false;
    unsigned newlyAppended=0;
    String aliceBases="";aliceBases.reserve(TRAIN_SLOTS);
    for(unsigned i=0;i<TRAIN_SLOTS;i++){
      aliceBases+=(txBasis[i]=='X'?'1':'0');
      if((bobBases[i]=='1')==(txBasis[i]=='X')){
        ++matched;
        if(validBits[i]=='1'){
          if(pendingCount>=sizeof(pendingBits)){
            send(peer,"ERR,SIFT_OVERFLOW");return;
          }
          pendingBits[pendingCount++]=txBit[i];
          ++matchedValid;++newlyAppended;
        }
      }
    }
    ++totalTrains;totalSlots+=TRAIN_SLOTS;
    send(peer,"META,"+String(id)+","+aliceBases+","+
              String(newlyAppended)+","+String(pendingCount));
    state="SIFT AFTER TRAIN";drawAlice();return;
  }
  if(op=="SIFTACK"){
    const uint32_t id=uint32_t(strtoul(field(cmd,1).c_str(),nullptr,10));
    if(id!=frameId || trainActive || field(cmd,2).toInt()!=pendingCount){
      send(peer,"ERR,SIFTACK");return;
    }
    send(peer,"SIFTOK,"+String(id)+","+String(pendingCount));
    if(pendingCount>=BLOCK_BITS)startCandidateBlock();
    return;
  }
  if(op=="PREP"){
    laserOff();
    const uint32_t id=uint32_t(strtoul(field(cmd,1).c_str(),nullptr,10));
    const String type=field(cmd,2);
    int angle=0;
    if(type=="R"){
      if(!sessionActive || !trainActive || frameSlot>=TRAIN_SLOTS){
        send(peer,"ERR,NOT_TRAIN");return;
      }
      preparedBasis=toBasis(uint8_t(esp_random()&1));
      preparedBit=uint8_t(esp_random()&1);
      txBasis[frameSlot]=preparedBasis;txBit[frameSlot]=preparedBit;
      ++frameSlot;
      angle=angleFor(preparedBasis,preparedBit);
    }else if(type=="M"){
      angle=field(cmd,3).toInt();
      preparedBasis=basisFromToken(field(cmd,4));
      preparedBit=uint8_t(field(cmd,5).toInt());
      if(angle<0||angle>180||preparedBit>1){send(peer,"ERR,PREP");return;}
    }else {send(peer,"ERR,PREP_TYPE");return;}
    preparedId=id;prepared=true;
    moveAlice(angle);
    // Alice does NOT reveal basis or bit until after Bob has measured.
    send(peer,"READY,"+String(id));
    state="PREPARED";drawAlice();return;
  }
  if(op=="GO"){
    const uint32_t id=uint32_t(strtoul(field(cmd,1).c_str(),nullptr,10));
    if(!prepared||id!=preparedId||laserOn){send(peer,"ERR,GO");return;}
    laserStart(false);delay(LASER_WARMUP_MS);
    send(peer,"ON,"+String(id));
    state="PULSE";drawAlice();return;
  }
  if(op=="OFF"){
    const uint32_t id=uint32_t(strtoul(field(cmd,1).c_str(),nullptr,10));
    if(!pulseMode||id!=preparedId){laserOff();send(peer,"ERR,OFF");return;}
    finishPulse(true);frames++;return;
  }
  if(op=="QBER"){
    if(blockPhase!=BlockPhase::QBER ||
       field(cmd,1).toInt()!=int(attemptedBlocks)){
      send(peer,"ERR,QBER_PHASE");return;
    }
    const int errors=field(cmd,2).toInt();
    if(errors<0||errors>QBER_SAMPLE_BITS){send(peer,"ERR,QBER_VALUE");return;}
    sampleErrors+=errors;sampleTotal+=QBER_SAMPLE_BITS;
    blockPhase=BlockPhase::RECON;
    send(peer,"SYND,"+String(attemptedBlocks)+","+
              hex32(syndrome(keptAlice,parityH)));
    state="LDPC-LIKE CHECK";drawAlice();return;
  }
  if(op=="RECON"){
    if(blockPhase!=BlockPhase::RECON ||
       field(cmd,1).toInt()!=int(attemptedBlocks)){
      send(peer,"ERR,RECON_PHASE");return;
    }
    if(field(cmd,2)=="BAD"){
      ++rejectedBlocks;discardAttempt();
      send(peer,"BLOCKDONE,"+String(attemptedBlocks)+",REJECT,"+
                String(acceptedBlocks)+","+String(rejectedBlocks));
      state="BLOCK REJECTED";drawAlice();return;
    }
    if(field(cmd,2)!="CAND"){send(peer,"ERR,RECON");return;}
    blockPhase=BlockPhase::VERDICT;
    send(peer,"BLOCKTAG,"+String(attemptedBlocks)+","+
              blockCheck(keptAlice,sampleMask));
    state="VERIFY BLOCK";drawAlice();return;
  }
  if(op=="VERDICT"){
    if(blockPhase!=BlockPhase::VERDICT ||
       field(cmd,1).toInt()!=int(attemptedBlocks)){
      send(peer,"ERR,VERDICT_PHASE");return;
    }
    const bool accepted=field(cmd,2)=="OK";
    if(accepted){
      retainUnsampled(keyMaterial,acceptedBlocks,keptAlice,sampleMask);
      ++acceptedBlocks;state="BLOCK ACCEPTED";
    }else{++rejectedBlocks;state="BLOCK REJECTED";}
    discardAttempt();
    send(peer,"BLOCKDONE,"+String(attemptedBlocks)+","+
              String(accepted?"OK":"REJECT")+","+
              String(acceptedBlocks)+","+String(rejectedBlocks));
    drawAlice();return;
  }
  if(op=="PASTART"){
    if(!sessionActive || acceptedBlocks!=BLOCKS_NEEDED ||
       field(cmd,1)!=sessionNonce || laserOn || trainActive){
      send(peer,"ERR,PA_NOT_READY");return;
    }
    randomPublicToeplitzSeed(paSeed);
    toeplitzPA(keyMaterial,paSeed,paKey);
    paPrepared=true;
    send(peer,"PASEED,"+hexBytes(paSeed,TOEPLITZ_SEED_BYTES));
    state="TOEPLITZ 896>256";drawAlice();return;
  }
  if(op=="PATAG"){
    const bool match=paPrepared && field(cmd,1)==paTag(paKey) &&
                     acceptedBlocks==BLOCKS_NEEDED;
    if(!match){paVerified=false;send(peer,"PAFAIL");state="KEY VERIFY FAIL";drawAlice();return;}
    const String key=hexBytes(paKey,32);
    paVerified=true;
    send(peer,"PAOK,"+paTag(paKey));
    updateFinalReport("KEY AGREED");
    keyDisplayActive=true;keyView.start(key,completedSessions,MATERIAL_BITS,
      qberString(reportData).substring(0,qberString(reportData).indexOf('%')));
    chime.signal(SoftChime::KEY_READY);
    state="KEY AGREED";
    Serial.println("#ALICE_PA_KEY,"+key);
    return;
  }
  if(op=="SHOWREPORT"){
    if(keyDisplayActive){keyDisplayActive=false;keyView.stop();}
    const String reason=field(cmd,1);
    updateFinalReport(paVerified?"KEY AGREED":
        (reason=="TRAIN COMPLETE"?"TRAIN COMPLETE":"SESSION REJECTED"));
    // Bob is the only voltage-sampling device. It supplies this 12-field
    // per-session measurement summary; Alice never guesses voltages.
    OpticalVoltageReport measured;
    if(!parseVoltageFields(cmd,2,measured)){
      send(peer,"ERR,VOLTAGE_FIELDS");return;
    }
    reportData.voltage=measured;
    reportActive=true;reportView.start(reportData,REPORT_HOLD_MS);
    send(peer,"REPORTOK");return;
  }
  if(op=="REPORT"){
    lastResult=cmd.substring(0,27);
    send(peer,"REPORTOK");drawAlice();return;
  }
  send(peer,"ERR,UNKNOWN");
}
void pollSerial(){
  while(Serial.available()){
    const char ch=char(Serial.read());
    if(ch=='\r'||ch=='\n'){
      serialBuffer.trim();serialBuffer.toUpperCase();
      if(serialBuffer=="STOP"){
        laserOff();prepared=false;state="USB STOP";drawAlice();
        Serial.println("#LASER_OFF");
      }else if(serialBuffer=="APRESET"){
        laserOff();apResetAt=millis()+200;
      }else if(serialBuffer=="STATUS"){
        Serial.printf("#STATUS,connected=%d,laser=%d,a=%d,frame=%lu\n",
          peer.connected(),laserOn,aliceAngle,(unsigned long)frames);
      }else if(serialBuffer=="HELP"){
        Serial.println("#Commands: STATUS, STOP, APRESET, HELP");
      }
      serialBuffer="";
    }else if(ch>=32&&ch<=126&&serialBuffer.length()<80) serialBuffer+=ch;
  }
}
void setup(){
  // IMPORTANT: GPIO14 is the laser. GPIO17 controls the attenuator servo
  // in the physical build, but this firmware NEVER touches GPIO17.
  digitalWrite(ALICE_LASER_PIN,LOW);pinMode(ALICE_LASER_PIN,OUTPUT);
  Serial.begin(115200);
  lcdInit(tft);buildH(parityH);
  const bool speakerOK=chime.begin();
  Serial.printf("#ALICE_I2S_SPEAKER,%s,quiet=900/32768\n",speakerOK?"READY":"DISABLED");
  if(!attachServo(ALICE_HWP_PIN,0)){
    state="SERVO ERROR";drawAlice();
    while(true){laserOff();delay(100);}
  }
  moveAlice(CAL_A_PLUS_0);
  setupAP();
  Serial.println("#ALICE_V7: TRAIN32 BLOCK64 QBER8 SPARSE24 TOEPLITZ896->256");
  Serial.println("#Laser GPIO14; HWP GPIO18; decoy servo GPIO17 RESERVED; max align 30s");
}
void loop(){
  pollSerial();
  if(reportActive){
    reportView.tick();
    if(reportView.done()){
      reportActive=false;reportView.stop();syncUi.invalidate();
      state="NEXT SESSION";drawAlice();
    }
  }
  if(keyDisplayActive){
    keyView.tick();
    // Keep the last key visible until Bob requests the synchronized results.
    // Bob's screen controls the 20-second countdown and next handshake.
  }
  if(pulseMode&&elapsed(beamStarted,MAX_PULSE_ON_MS)){
    Serial.println("#PULSE_WATCHDOG");finishPulse(false);
  }
  if(alignMode&&int32_t(millis()-alignedUntil)>=0){
    laserOff();state="ALIGN TIMEOUT";drawAlice();
    Serial.println("#ALIGN_WATCHDOG");
  }
  if(apResetAt&&int32_t(millis()-apResetAt)>=0){
    apResetAt=0;setupAP();return;
  }
  if(peer&& !peer.connected()){
    laserOff();peer.stop();tcpBuffer="";state="LINK LOST";drawAlice();
  }
  if(!peer.connected()){
    WiFiClient incoming=server.available();
    if(incoming){peer=incoming;peer.setNoDelay(true);tcpBuffer="";state="LINK NEW";drawAlice();}
  }
  if(peer.connected()){
    while(peer.available()){
      char ch=char(peer.read());
      if(ch=='\n'){
        const String cmd=tcpBuffer;tcpBuffer="";process(cmd);
        if(!peer.connected())break;
      }else if(ch!='\r'&&tcpBuffer.length()<256)tcpBuffer+=ch;
      else if(tcpBuffer.length()>=256){laserOff();peer.stop();tcpBuffer="";}
    }
  }
  delay(1);
}
