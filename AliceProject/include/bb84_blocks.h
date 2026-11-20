#pragma once
// Classical-light BB84 training, sparse-parity reconciliation and Toeplitz
// post-processing for instructional use. NOT a quantum-safe key agreement.
#include "bb84_shared.h"
#include <string.h>

namespace demo {
constexpr uint8_t TRAIN_SLOTS=32;
constexpr uint8_t BLOCK_BITS=64;
constexpr uint8_t QBER_SAMPLE_BITS=8;
constexpr uint8_t PARITY_CHECKS=24;
constexpr uint8_t BLOCKS_NEEDED=16;
constexpr uint8_t RETAINED_PER_BLOCK=BLOCK_BITS-QBER_SAMPLE_BITS; // 56
constexpr size_t MATERIAL_BITS=size_t(BLOCKS_NEEDED)*RETAINED_PER_BLOCK; // 896
constexpr size_t KEY_BITS=256;
constexpr size_t TOEPLITZ_SEED_BITS=MATERIAL_BITS+KEY_BITS-1; // 1151
constexpr size_t TOEPLITZ_SEED_BYTES=(TOEPLITZ_SEED_BITS+7)/8; // 144
constexpr uint16_t MAX_TRAINS_PER_SESSION=180;
constexpr uint16_t MAX_REJECTED_BLOCKS=24;
constexpr uint8_t MAX_SAMPLED_QBER_PERCENT=10;
constexpr uint32_t REPORT_HOLD_MS=10000;

static_assert(MATERIAL_BITS == 896 && TOEPLITZ_SEED_BYTES==144, "PA sizes changed");

inline String hex64(uint64_t n){
  char b[17];snprintf(b,sizeof(b),"%08lX%08lX",(unsigned long)(n>>32),(unsigned long)n);
  return String(b);
}
inline uint64_t toU64(const String& s){return uint64_t(strtoull(s.c_str(),nullptr,16));}
inline String hex32(uint32_t n){char b[9];snprintf(b,sizeof(b),"%08lX",(unsigned long)n);return String(b);}
inline uint32_t toU32(const String& s){return uint32_t(strtoul(s.c_str(),nullptr,16));}
inline bool bitString32(const String& s){
  if(s.length()!=TRAIN_SLOTS)return false;
  for(unsigned i=0;i<TRAIN_SLOTS;i++)if(s[i]!='0'&&s[i]!='1')return false;
  return true;
}
inline uint64_t pack64(const uint8_t *a){
  uint64_t r=0;for(int i=0;i<64;i++)if(a[i])r|=1ULL<<i;return r;
}
inline uint64_t chooseSample8(){
  uint64_t mask=0;
  while(__builtin_popcountll(mask)!=QBER_SAMPLE_BITS)
    mask|=uint64_t(1)<<uint8_t(esp_random()%BLOCK_BITS);
  return mask;
}
// Deterministic sparse parity-check matrix H(24 x 64), five ones per column.
// The demonstration decoder exhaustively tests at most three corrected bit
// positions. This is NOT an industrial belief-propagation LDPC decoder.
inline void buildH(uint64_t H[PARITY_CHECKS]){
  memset(H,0,sizeof(uint64_t)*PARITY_CHECKS);
  for(uint8_t i=0;i<64;i++){
    uint32_t x=0x9E3779B9UL^(uint32_t(i+1)*0x85EBCA6BUL);
    uint32_t rows=0;
    while(__builtin_popcount(rows)<5){
      x^=x<<13;x^=x>>17;x^=x<<5;rows|=1UL<<(x%PARITY_CHECKS);
    }
    for(uint8_t r=0;r<PARITY_CHECKS;r++)if(rows&(1UL<<r))H[r]|=1ULL<<i;
  }
}
inline uint32_t syndrome(uint64_t bits,const uint64_t H[PARITY_CHECKS]){
  uint32_t syn=0;
  for(uint8_t r=0;r<PARITY_CHECKS;r++)
    if(__builtin_parityll(bits&H[r]))syn|=1UL<<r;
  return syn;
}
inline bool sparseReconcile(uint64_t& bob, uint32_t aliceSyn,
                            const uint64_t H[PARITY_CHECKS]){
  const uint32_t difference=syndrome(bob,H)^aliceSyn;
  if(!difference)return true;
  uint32_t col[64];
  for(unsigned i=0;i<64;i++)col[i]=syndrome(1ULL<<i,H);
  for(unsigned i=0;i<64;i++)if(col[i]==difference){bob^=1ULL<<i;return true;}
  for(unsigned i=0;i<64;i++)for(unsigned j=i+1;j<64;j++)
    if((col[i]^col[j])==difference){bob^=(1ULL<<i)|(1ULL<<j);return true;}
  for(unsigned i=0;i<64;i++)for(unsigned j=i+1;j<64;j++)
    for(unsigned k=j+1;k<64;k++)
      if((col[i]^col[j]^col[k])==difference){
        bob^=(1ULL<<i)|(1ULL<<j)|(1ULL<<k);return true;
      }
  return false;
}
inline void retainUnsampled(uint8_t *output, unsigned blockNumber,
                            uint64_t corrected, uint64_t sampleMask){
  size_t index=size_t(blockNumber)*RETAINED_PER_BLOCK;
  for(unsigned bit=0;bit<64;bit++)if(!(sampleMask&(1ULL<<bit)))
    output[index++]=uint8_t((corrected>>bit)&1U);
}
inline String hexBytes(const uint8_t *bytes,size_t count){
  static constexpr char hex[]="0123456789ABCDEF";
  String s;s.reserve(2*count);
  for(size_t i=0;i<count;i++){s+=hex[bytes[i]>>4];s+=hex[bytes[i]&15];}
  return s;
}
inline bool parseHexBytes(const String& s,uint8_t *bytes,size_t count){
  if(size_t(s.length())!=2*count)return false;
  for(size_t i=0;i<count;i++){
    auto digit=[](char c)->int{
      if(c>='0'&&c<='9')return c-'0';
      if(c>='A'&&c<='F')return c-'A'+10;
      if(c>='a'&&c<='f')return c-'a'+10;
      return -1;
    };
    const int x=digit(s[2*i]),y=digit(s[2*i+1]);
    if(x<0||y<0)return false;
    bytes[i]=uint8_t((x<<4)|y);
  }
  return true;
}
inline void randomPublicToeplitzSeed(uint8_t seed[TOEPLITZ_SEED_BYTES]){
  for(size_t i=0;i<TOEPLITZ_SEED_BYTES;i+=4){
    uint32_t value=esp_random();
    for(size_t j=0;j<4 && i+j<TOEPLITZ_SEED_BYTES;j++)
      seed[i+j]=uint8_t(value>>(8*j));
  }
  // The final unused padding bit does not influence Toeplitz.
  seed[TOEPLITZ_SEED_BYTES-1]&=0x7f;
}
inline void toeplitzPA(const uint8_t raw[MATERIAL_BITS],
                       const uint8_t seed[TOEPLITZ_SEED_BYTES],uint8_t key[32]){
  memset(key,0,32);
  for(size_t out=0;out<KEY_BITS;out++){
    uint8_t parity=0;
    for(size_t in=0;in<MATERIAL_BITS;in++)if(raw[in]){
      const size_t seedIndex=KEY_BITS-1-out+in;
      parity^=uint8_t((seed[seedIndex>>3]>>(seedIndex&7))&1U);
    }
    if(parity)key[out>>3]|=uint8_t(1U<<(out&7));
  }
}
inline String blockCheck(uint64_t corrected,uint64_t mask){
  // Public 32-bit confirmation; disclosed verification information (not MAC).
  return signature(String("BLOCK-V7|")+hex64(corrected)+"|"+hex64(mask)).substring(0,8);
}
inline String paTag(const uint8_t key[32]){
  return signature(String("KEY-CONFIRM-V7|")+hexBytes(key,32)).substring(0,16);
}
}
