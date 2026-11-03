// SPDX-License-Identifier: MIT
#include "portkit.h"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
using namespace tour::recomp;
void Check(bool ok,const char* name){
    if(!ok){std::cerr<<"FAILED "<<name<<"\n";std::abort();}
}

// A synthetic recomp verifies the *actual* callback table + lifecycle.
// It does not contain original game code, symbols or assets.
struct MockGame {
    int boots=0,steps=0,shutdowns=0,logs=0,rumbles=0,audio=0;
    TourRecompControllerV1 last{};
    TourRecompHostV1 host{};
    std::array<Byte,24> video{};
};
extern "C" int MockBoot(void* user,const Byte* source,std::size_t sourceBytes,
                        Byte* memory,std::size_t memoryBytes,
                        Byte* save,std::size_t saveBytes,
                        const TourRecompHostV1* host){
    auto& g=*static_cast<MockGame*>(user);
    ++g.boots;
    if(!source||sourceBytes!=16||!memory||memoryBytes!=RdramBytes ||
       !save||saveBytes!=512 || !host || host->struct_bytes!=sizeof(*host))
        return 0;
    g.host=*host;
    if(g.host.log)g.host.log(g.host.user,1,"synthetic game boot");
    return 1;
}
extern "C" void MockStep(void* user,TourRecompControllerV1 input) {
    auto& g=*static_cast<MockGame*>(user);
    ++g.steps;g.last=input;
    g.video.fill(static_cast<Byte>(g.steps));
    if(g.host.rumble)g.host.rumble(g.host.user,0.25f,0.5f,100);
    if(g.host.audio){
        float samples[4]{0.2f,0.2f,-0.2f,-0.2f};
        TourRecompAudioV1 chunk{samples,2,2,48000};
        g.host.audio(g.host.user,&chunk);
    }
}
extern "C" int MockCapture(void* user,TourRecompFrameV1* out) {
    auto& g=*static_cast<MockGame*>(user);
    if(!out||g.steps==0)return 0;
    *out={g.video.data(),2,2,12};
    return 1;
}
extern "C" void MockShutdown(void* user) {
    ++static_cast<MockGame*>(user)->shutdowns;
}
void MockLog(void* user,std::uint32_t,const char* message) {
    auto& g=*static_cast<MockGame*>(user);
    if(message) ++g.logs;
}
void MockRumble(void* user,float low,float high,std::uint32_t ms) {
    auto& g=*static_cast<MockGame*>(user);
    if(low>0 && high>0 && ms>0)++g.rumbles;
}
void MockAudio(void* user,const TourRecompAudioV1* data) {
    auto& g=*static_cast<MockGame*>(user);
    if(data && data->interleaved && data->frames==2 &&
       data->channels==2 && data->sample_rate==48000) ++g.audio;
}

int main(){
    std::string error;
    Check(Sha1({})=="da39a3ee5e6b4b0d3255bfef95601890afd80709","sha1 empty");
    const std::array<Byte,3> abc{'a','b','c'};
    Check(Sha1(abc)=="a9993e364706816aba3e25717850c26c9cd0d89d","sha1 abc");
    const std::vector<Byte> normal{0x80,0x37,0x12,0x40,1,2,3,4};
    const std::vector<Byte> swap16{0x37,0x80,0x40,0x12,2,1,4,3};
    const std::vector<Byte> swap32{0x40,0x12,0x37,0x80,4,3,2,1};
    std::vector<Byte> out;RomOrder order{};
    Check(NormalizeRom(normal,out,order,error)&&out==normal&&order==RomOrder::Z64,"z64");
    Check(NormalizeRom(swap16,out,order,error)&&out==normal&&order==RomOrder::V64,"v64");
    Check(NormalizeRom(swap32,out,order,error)&&out==normal&&order==RomOrder::N64,"n64");
    Check(!NormalizeRom(std::array<Byte,4>{1,2,3,4},out,order,error),"invalid rom");
    GuestMemory ram;
    Check(ram.Write32(0x80000004,0x12345678),"write");
    std::uint32_t word=0;
    Check(ram.Read32(0xa0000004,word)&&word==0x12345678,"uncached alias");
    Check(ram.Data()[4]==0x12&&ram.Data()[7]==0x78,"big endian");
    Check(!ram.Write32(0x807fffff,123)&&!ram.Read32(0xc0000000,word),"bounds");
    Check(Stick(-2)==-80&&Stick(2)==80&&
          Stick(std::numeric_limits<float>::quiet_NaN())==0,"controller");
    FixedClock clock;
    Check(clock.Advance(1.0/60)==0&&clock.Advance(1.0/60)==1,"30Hz");
    Check(clock.Advance(1.0)==4&&clock.DroppedTicks()==26,"catchup");
    const std::array<Byte,24> pixels{
        1,2,3,4,5,6,7,8,9,9,9,9,10,11,12,13,14,15,16,17,9,9,9,9};
    VideoFrame frame;
    Check(frame.Copy({pixels.data(),2,2,12},error),"video");
    Check(frame.Pixels().size()==16&&frame.Pixels()[8]==10,"row pitch");
    Check(!frame.Copy({pixels.data(),0,2,0},error),"bad video");
    namespace fs=std::filesystem;
    const auto path=fs::temp_directory_path()/"tour-portkit-test.eep";
    fs::remove(path);
    Eeprom save;
    Check(save.Load(path,error),"empty save");
    save.Data()[5]=42;
    Check(save.Save(path,error),"save");
    Eeprom restored;
    Check(restored.Load(path,error)&&restored.Data()[5]==42,"load");
    fs::remove(path);
    // Only synthetic bytes are used: a ROM-shaped fake is refused.
    const auto badRom=fs::temp_directory_path()/"tour-portkit-test.z64";
    {
        std::vector<Byte> fake(Sm64UsBytes);
        std::copy(normal.begin(),normal.begin()+4,fake.begin());
        std::ofstream os(badRom,std::ios::binary);
        os.write(reinterpret_cast<const char*>(fake.data()),fake.size());
    }
    Check(!LoadSm64UsRom(badRom,out,error)&&out.empty(),"SHA1 game gate");
    fs::remove(badRom);
    Session session;
    Check(!session.Boot(badRom,path,{},error),"must have an adapter");
    // Exercise a complete adapter with synthetic content, zero ROMs.
    MockGame mock;
    std::array<Byte,16> dummy{};
    GuestMemory guest;
    Eeprom guestSave;
    Bridge abi{};
    abi.abi_version=BridgeAbi;
    abi.struct_bytes=sizeof(abi);
    abi.capabilities=TOUR_RECOMP_CAP_RGBA_FRAME |
                     TOUR_RECOMP_CAP_AUDIO | TOUR_RECOMP_CAP_RUMBLE;
    abi.user=&mock;
    abi.boot=MockBoot;
    abi.step=MockStep;
    abi.frame_rgba=MockCapture;
    abi.shutdown=MockShutdown;
    const TourRecompHostV1 callbacks{sizeof(TourRecompHostV1),&mock,
                                   MockLog,MockAudio,MockRumble};
    BridgeRunner runner;
    Check(runner.Start(abi,dummy,guest,guestSave,callbacks,error),
          "boot synthetic adapter");
    Check(mock.boots==1 && mock.logs==1,"boot and log callbacks");
    Check(runner.Pump(1.0/60.0,{A,60,-25},error) && mock.steps==0,
          "no extra 60Hz simulation step");
    Check(runner.Pump(1.0/60.0,{A,60,-25},error) && mock.steps==1 &&
          mock.last.buttons==A && mock.last.stick_x==60 &&
          mock.last.stick_y==-25,"30Hz controller tick");
    Check(mock.audio==1 && mock.rumbles==1,"callback audio and rumble");
    Check(runner.Video().Pixels().size()==16 &&
          runner.Video().Pixels()[0]==1,"frame from adapter");
    Check(runner.Pump(1.0/30.0,{Start,-80,80},error) &&
          mock.steps==2,"second game tick");
    runner.Stop();
    Check(mock.shutdowns==1 && !runner.Running(),"cleanup once");
    runner.Stop();
    Check(mock.shutdowns==1,"idempotent shutdown");
    abi.struct_bytes=0;
    Check(!runner.Start(abi,dummy,guest,guestSave,callbacks,error) &&
          mock.boots==1,"incompatible ABI blocks execution");
    abi.struct_bytes=sizeof(abi);
    abi.capabilities=0;
    Check(!runner.Start(abi,dummy,guest,guestSave,callbacks,error),
          "required frame capability");
    abi.capabilities=TOUR_RECOMP_CAP_RGBA_FRAME;
    Check(!runner.Start(abi,{},guest,guestSave,callbacks,error),
          "empty content rejected");
    // Game-specific Session cannot bypass SM64 USA verification.
    abi.capabilities=TOUR_RECOMP_CAP_RGBA_FRAME;
    Check(!session.Boot(badRom,path,abi,error),"session ROM gate");
    Check(mock.boots==1,"no original ROM => no adapter boot");
    std::cout<<"portkit: asset-free tests passed\n";
}
