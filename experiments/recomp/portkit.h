// SPDX-License-Identifier: MIT
#pragma once
// Experimental N64 recomp hosting primitives; no game code or game assets.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace tour::recomp {
using Byte=std::uint8_t;
constexpr std::size_t RdramBytes=8*1024*1024;
constexpr std::size_t Sm64UsBytes=8*1024*1024;
constexpr const char* Sm64UsSha1="9bef1128717f958171a4afac3ed78ee2bb4e86ce";
enum class RomOrder { Z64, V64, N64 };
inline std::uint32_t Be32(const Byte* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|
           (std::uint32_t(p[2])<<8)|p[3];
}
inline std::uint32_t Rotl(std::uint32_t x,unsigned n) {
    return (x<<n)|(x>>(32-n));
}
inline void Sha1Block(const Byte* p,std::array<std::uint32_t,5>& h) {
    std::uint32_t w[80]{};
    for(int i=0;i<16;++i) w[i]=Be32(p+4*i);
    for(int i=16;i<80;++i) w[i]=Rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
    for(int i=0;i<80;++i) {
        std::uint32_t f,k;
        if(i<20){f=(b&c)|(~b&d);k=0x5a827999;}
        else if(i<40){f=b^c^d;k=0x6ed9eba1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;}
        else{f=b^c^d;k=0xca62c1d6;}
        const auto t=Rotl(a,5)+f+e+k+w[i];
        e=d;d=c;c=Rotl(b,30);b=a;a=t;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
}
inline std::string Sha1(std::span<const Byte> input) {
    std::array<std::uint32_t,5> h{
        0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    std::size_t i=0;
    while(input.size()-i>=64){Sha1Block(input.data()+i,h);i+=64;}
    std::array<Byte,128> tail{};
    const auto n=input.size()-i;
    if(n) std::memcpy(tail.data(),input.data()+i,n);
    tail[n]=0x80;
    const std::size_t padded=n<56?64:128;
    const auto bits=std::uint64_t(input.size())*8;
    for(unsigned j=0;j<8;++j) tail[padded-j-1]=Byte(bits>>(j*8));
    Sha1Block(tail.data(),h);
    if(padded==128) Sha1Block(tail.data()+64,h);
    std::ostringstream os;
    os<<std::hex<<std::setfill('0');
    for(auto x:h) os<<std::setw(8)<<x;
    return os.str();
}
inline bool NormalizeRom(std::span<const Byte> src,std::vector<Byte>& out,
                         RomOrder& order,std::string& err) {
    out.clear();
    if(src.size()<4 || src.size()%4){err="ROM is not word aligned";return false;}
    const auto first=Be32(src.data());
    if(first==0x80371240) order=RomOrder::Z64;
    else if(first==0x37804012) order=RomOrder::V64;
    else if(first==0x40123780) order=RomOrder::N64;
    else {err="Invalid N64 ROM header";return false;}
    out.assign(src.begin(),src.end());
    if(order==RomOrder::V64)
        for(std::size_t i=0;i<out.size();i+=2) std::swap(out[i],out[i+1]);
    if(order==RomOrder::N64)
        for(std::size_t i=0;i<out.size();i+=4) {
            std::swap(out[i],out[i+3]);std::swap(out[i+1],out[i+2]);
        }
    err.clear();return true;
}
inline bool LoadSm64UsRom(const std::filesystem::path& path,
                         std::vector<Byte>& rom,std::string& err) {
    rom.clear();
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file){err="Bring your own legally obtained SM64 USA ROM";return false;}
    if(file.tellg()<0 || std::uint64_t(file.tellg())!=Sm64UsBytes){
        err="SM64 USA ROM must be 8 MiB";return false;
    }
    file.seekg(0);
    std::vector<Byte> original(Sm64UsBytes);
    if(!file.read(reinterpret_cast<char*>(original.data()),original.size())){
        err="Failed to read ROM";return false;
    }
    RomOrder order;
    if(!NormalizeRom(original,rom,order,err)) return false;
    if(Sha1(rom)!=Sm64UsSha1){
        rom.clear();err="SM64 USA ROM SHA-1 mismatch";return false;
    }
    err.clear();return true;
}
// Physical DMA offsets and cached/uncached KSEG0/KSEG1 virtual aliases.
class GuestMemory {
public:
    GuestMemory():data_(RdramBytes,0){}
    Byte* Data(){return data_.data();}
    std::size_t Size() const{return data_.size();}
    Byte* Translate(std::uint32_t a,std::size_t bytes) {
        std::uint32_t p;
        if(a<RdramBytes) p=a;
        else if((a&0xe0000000u)==0x80000000u ||
                (a&0xe0000000u)==0xa0000000u) p=a&0x1fffffffu;
        else return nullptr;
        if(p>data_.size() || bytes>data_.size()-p) return nullptr;
        return data_.data()+p;
    }
    const Byte* Translate(std::uint32_t a,std::size_t n) const {
        return const_cast<GuestMemory*>(this)->Translate(a,n);
    }
    bool Read32(std::uint32_t a,std::uint32_t& v) const {
        const auto p=Translate(a,4);if(!p) return false;v=Be32(p);return true;
    }
    bool Write32(std::uint32_t a,std::uint32_t v){
        auto p=Translate(a,4);if(!p) return false;
        for(int i=0;i<4;++i) p[i]=Byte(v>>(24-8*i));
        return true;
    }
private:
    std::vector<Byte> data_;
};
enum Button:std::uint16_t {
    A=0x8000,B=0x4000,Z=0x2000,Start=0x1000,
    DUp=0x0800,DDown=0x0400,DLeft=0x0200,DRight=0x0100,
    L=0x0020,R=0x0010,CUp=0x0008,CDown=0x0004,
    CLeft=0x0002,CRight=0x0001
};
struct Controller{std::uint16_t buttons=0;std::int8_t x=0,y=0;};
inline std::int8_t Stick(float x){
    if(!std::isfinite(x)) return 0;
    return std::int8_t(std::lround(std::clamp(x,-1.0f,1.0f)*80.0f));
}
// Original simulation at 30 Hz, independent of presentation frequency.
class FixedClock {
public:
    explicit FixedClock(std::uint32_t maxCatchup=4):max_(std::max(1u,maxCatchup)){}
    std::uint32_t Advance(double seconds){
        if(!std::isfinite(seconds)||seconds<0) return 0;
        leftover_+=std::min(seconds,1.0);
        const auto due=std::uint64_t(std::floor((leftover_+1.e-12)*30.0));
        leftover_=std::max(0.0,leftover_-double(due)/30.0);
        const auto count=std::min<std::uint64_t>(due,max_);
        dropped_+=due-count;
        return std::uint32_t(count);
    }
    double Alpha() const{return leftover_*30.0;}
    std::uint64_t DroppedTicks() const{return dropped_;}
private:
    double leftover_=0;
    std::uint32_t max_;
    std::uint64_t dropped_=0;
};
struct VideoView{const Byte* pixels=nullptr;std::uint32_t width=0,height=0,pitch=0;};
class VideoFrame{
public:
    bool Copy(VideoView src,std::string& error){
        if(!src.pixels || !src.width || !src.height ||
           src.width>4096 || src.height>4096 ||
           src.pitch<std::uint64_t(src.width)*4 ||
           src.pitch>std::uint64_t(src.width)*16){
            error="Invalid RGBA8 source frame";return false;
        }
        std::vector<Byte> pixels(std::size_t(src.width)*src.height*4);
        for(std::uint32_t y=0;y<src.height;++y)
            std::memcpy(pixels.data()+std::size_t(y)*src.width*4,
                        src.pixels+std::size_t(y)*src.pitch,
                        std::size_t(src.width)*4);
        data_=std::move(pixels);width_=src.width;height_=src.height;
        error.clear();return true;
    }
    const std::vector<Byte>& Pixels() const{return data_;}
    std::uint32_t Width() const{return width_;}
    std::uint32_t Height() const{return height_;}
private:
    std::vector<Byte> data_;
    std::uint32_t width_=0,height_=0;
};
// EEPROM lives in user storage; no original save data is included.
class Eeprom {
public:
    Eeprom():bytes_(512,0){}
    Byte* Data(){return bytes_.data();}
    std::size_t Size() const{return bytes_.size();}
    bool Load(const std::filesystem::path& path,std::string& err){
        std::ifstream in(path,std::ios::binary|std::ios::ate);
        if(!in){
            std::error_code ec;
            if(!std::filesystem::exists(path,ec) && !ec){
                std::fill(bytes_.begin(),bytes_.end(),0);err.clear();return true;
            }
            err="Save file could not be opened";return false;
        }
        if(in.tellg()<0||std::uint64_t(in.tellg())!=bytes_.size()){
            err="Invalid EEPROM length";return false;
        }
        in.seekg(0);
        std::vector<Byte> tmp(bytes_.size());
        if(!in.read(reinterpret_cast<char*>(tmp.data()),tmp.size())){
            err="EEPROM read failed";return false;
        }
        bytes_.swap(tmp);err.clear();return true;
    }
    bool Save(const std::filesystem::path& path,std::string& err) const {
        std::error_code ec;
        if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path(),ec);
        if(ec){err="Cannot create save directory";return false;}
        const auto tmp=std::filesystem::path(path.string()+".tmp");
        {
            std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
            if(!out || !out.write(reinterpret_cast<const char*>(bytes_.data()),
                                  bytes_.size()) || !out.flush()){
                err="EEPROM write failed";return false;
            }
        }
        std::filesystem::rename(tmp,path,ec);
        if(ec){err="EEPROM rename failed";return false;}
        err.clear();return true;
    }
private:
    std::vector<Byte> bytes_;
};
// An external GPL runtime can implement this ABI without vendoring game code.
// Frame capture is a correctness/debug fallback; a real RT64 port needs GPU interop.
constexpr std::uint32_t BridgeAbi=1;
struct Bridge {
    std::uint32_t abi=BridgeAbi;
    void* user=nullptr;
    bool (*boot)(void*,const Byte*,std::size_t,Byte*,std::size_t,
                 Byte*,std::size_t)=nullptr;
    void (*step)(void*,Controller)=nullptr;
    bool (*capture)(void*,VideoView&)=nullptr;
    void (*shutdown)(void*)=nullptr;
};
class Session {
public:
    ~Session(){Shutdown();}
    bool Boot(const std::filesystem::path& romPath,const std::filesystem::path& savePath,
              Bridge bridge,std::string& error){
        Shutdown();
        if(bridge.abi!=BridgeAbi || !bridge.boot || !bridge.step ||
           !bridge.capture || !bridge.shutdown){
            error="Invalid recomp bridge ABI";return false;
        }
        if(!LoadSm64UsRom(romPath,rom_,error) || !save_.Load(savePath,error))
            return false;
        bridge_=bridge;savePath_=savePath;
        if(!bridge_.boot(bridge_.user,rom_.data(),rom_.size(),ram_.Data(),ram_.Size(),
                         save_.Data(),save_.Size())){
            bridge_.shutdown(bridge_.user);
            bridge_={};rom_.clear();error="Recomp adapter boot failed";return false;
        }
        running_=true;error.clear();return true;
    }
    bool Pump(double seconds,Controller input,std::string& error){
        if(!running_){error="Session not running";return false;}
        const auto count=clock_.Advance(seconds);
        for(std::uint32_t i=0;i<count;++i) bridge_.step(bridge_.user,input);
        if(count){
            VideoView frame{};
            if(!bridge_.capture(bridge_.user,frame)){
                error="Recomp renderer capture failed";return false;
            }
            return video_.Copy(frame,error);
        }
        error.clear();return true;
    }
    bool Save(std::string& error) const {
        if(!running_){error="Session not running";return false;}
        return save_.Save(savePath_,error);
    }
    void Shutdown(){
        if(running_){bridge_.shutdown(bridge_.user);running_=false;}
        bridge_={};rom_.clear();savePath_.clear();
    }
    bool Running() const{return running_;}
    const VideoFrame& Video() const{return video_;}
    const FixedClock& Clock() const{return clock_;}
private:
    std::vector<Byte> rom_;
    GuestMemory ram_;
    Eeprom save_;
    FixedClock clock_;
    VideoFrame video_;
    Bridge bridge_{};
    std::filesystem::path savePath_;
    bool running_=false;
};
} // namespace tour::recomp
