#pragma once
// Gentle I2S speaker cues: pins based on the earlier board's working I2S code.
// BCLK=15, LRCLK=16, DOUT=7. No GPIO17 (Alice attenuator) is touched.
// Core 3.x: ESP_I2S; Core 2.x: legacy IDF i2s driver.
#include "bb84_shared.h"
#include <math.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include <ESP_I2S.h>
#else
  #include <driver/i2s.h>
#endif

namespace demo {
class SoftChime {
public:
  enum : uint8_t {NONE=0, KEY_READY=1, NEXT_SESSION=2};
  bool begin(){
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    port_.setPins(15,16,7); // BCLK, WS, DIN
    ready_=port_.begin(I2S_MODE_STD,16000,I2S_DATA_BIT_WIDTH_16BIT,
                      I2S_SLOT_MODE_STEREO);
#else
    i2s_config_t cfg={};
    cfg.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX);
    cfg.sample_rate=16000;cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format=I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count=4;cfg.dma_buf_len=128;cfg.tx_desc_auto_clear=true;
    i2s_pin_config_t pins={};
    pins.mck_io_num=I2S_PIN_NO_CHANGE;
    pins.bck_io_num=15;pins.ws_io_num=16;
    pins.data_out_num=7;pins.data_in_num=I2S_PIN_NO_CHANGE;
    ready_=(i2s_driver_install(I2S_NUM_0,&cfg,0,nullptr)==ESP_OK);
    if(ready_)ready_=(i2s_set_pin(I2S_NUM_0,&pins)==ESP_OK);
#endif
    if(ready_){
      BaseType_t created=xTaskCreatePinnedToCore(taskEntry,"BB84-quiet-chime",
                   4096,this,1,nullptr,1);
      if(created!=pdPASS)ready_=false;
    }
    return ready_;
  }
  void signal(uint8_t which){if(ready_)requested_=which;}
  bool available() const{return ready_;}
private:
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  I2SClass port_;
#endif
  volatile uint8_t requested_=NONE;
  bool ready_=false;
  // About 2.7% of full 16-bit amplitude; intentionally quiet.
  static constexpr int16_t AMPLITUDE=900;
  static void taskEntry(void* user){static_cast<SoftChime*>(user)->taskLoop();}
  void taskLoop(){
    for(;;){
      const uint8_t cue=requested_;
      if(cue){
        requested_=NONE;
        if(cue==KEY_READY){
          note(659,85);silence(55);note(831,90);silence(45);note(1047,155);
        }else if(cue==NEXT_SESSION){
          note(740,75);silence(45);note(988,290);
        }
        silence(180); // flush DMA with silence so amplifier does not hum
      }else vTaskDelay(pdMS_TO_TICKS(18));
    }
  }
  void emit(const int16_t* data,size_t nframes){
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    port_.write(reinterpret_cast<const uint8_t*>(data),nframes*4);
#else
    size_t written=0;
    i2s_write(I2S_NUM_0,data,nframes*4,&written,portMAX_DELAY);
#endif
  }
  void silence(uint16_t ms){
    int16_t pcm[256]={};
    uint32_t frames=(uint32_t(ms)*16000u)/1000u;
    while(frames){
      const size_t n=frames>128?128:size_t(frames);
      emit(pcm,n);frames-=n;
    }
  }
  void note(uint16_t freq,uint16_t ms){
    int16_t pcm[256];
    const uint32_t length=(uint32_t(ms)*16000u)/1000u;
    const float dtheta=6.28318530718f*float(freq)/16000.0f;
    float phase=0.0f;
    uint32_t pos=0;
    while(pos<length){
      const size_t n=(length-pos)>128?128:size_t(length-pos);
      for(size_t i=0;i<n;i++){
        const uint32_t index=pos+uint32_t(i);
        float ramp=1.0f;
        if(index<128u)ramp=float(index)/128.0f;
        if(length-index<128u)ramp=fminf(ramp,float(length-index)/128.0f);
        const int16_t sample=int16_t(AMPLITUDE*ramp*sinf(phase));
        phase+=dtheta;
        if(phase>=6.28318530718f)phase-=6.28318530718f;
        pcm[2*i]=sample;pcm[2*i+1]=sample;
      }
      emit(pcm,n);pos+=n;
    }
  }
};
} // namespace demo
