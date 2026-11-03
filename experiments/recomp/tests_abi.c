// SPDX-License-Identifier: MIT
// Proof that recomp_abi.h is C-usable (no C++ runtime or Tourmaline engine).
#include "recomp_abi.h"
#include <assert.h>
#include <string.h>

static int boot(void* user,const uint8_t* content,size_t content_bytes,
                uint8_t* rdram,size_t rdram_bytes,
                uint8_t* eeprom,size_t eeprom_bytes,
                const TourRecompHostV1* host) {
    (void)user;
    return content && content_bytes==1 && rdram && rdram_bytes==8*1024*1024 &&
           eeprom && eeprom_bytes==512 && host &&
           host->struct_bytes==sizeof(TourRecompHostV1);
}
static void step(void* user,TourRecompControllerV1 pad){(void)user;(void)pad;}
static int frame(void* user,TourRecompFrameV1* out){
    (void)user;
    if(!out)return 0;
    memset(out,0,sizeof(*out));
    return 1;
}
static void stop(void* user){(void)user;}
int main(void){
    TourRecompApiV1 api={0};
    TourRecompHostV1 host={0};
    TourRecompControllerV1 pad={0};
    api.abi_version=TOUR_RECOMP_ABI_VERSION;
    api.struct_bytes=(uint32_t)sizeof(api);
    api.capabilities=TOUR_RECOMP_CAP_RGBA_FRAME;
    api.boot=boot;
    api.step=step;
    api.frame_rgba=frame;
    api.shutdown=stop;
    host.struct_bytes=(uint32_t)sizeof(host);
    assert(api.abi_version==1u && api.struct_bytes==sizeof(TourRecompApiV1));
    assert(api.capabilities & TOUR_RECOMP_CAP_RGBA_FRAME);
    assert(api.boot && api.step && api.frame_rgba && api.shutdown);
    api.step(api.user,pad);
    return 0;
}
