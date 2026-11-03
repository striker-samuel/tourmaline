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
    std::cout<<"portkit: asset-free tests passed\n";
}
