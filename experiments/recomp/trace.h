// SPDX-License-Identifier: MIT
#pragma once
// Compact deterministic comparison traces for static-recomp porting.
// Only per-frame hashes/input/timing are recorded; no copyrighted pixels
// or source ROM bytes are stored in this log.
#include "portkit.h"
#include <optional>
#include <sstream>
#include <vector>
namespace tour::recomp {
inline std::uint64_t Fnv1a64(std::span<const Byte> bytes) {
    std::uint64_t hash=0xcbf29ce484222325ull;
    for(Byte b:bytes){hash^=b;hash*=0x100000001b3ull;}
    return hash;
}
struct FrameSignature {
    std::uint64_t tick=0;
    std::uint64_t rgbaHash=0;
    Controller input{};
    std::uint32_t width=0,height=0;
    bool operator==(const FrameSignature&) const=default;
};
class FrameTrace {
public:
    explicit FrameTrace(std::size_t capacity=1024):capacity_(capacity){}
    bool Record(std::uint64_t tick,Controller pad,const VideoFrame& frame) {
        if(frame.Pixels().empty() || !capacity_)return false;
        if(entries_.size()==capacity_){
            entries_.erase(entries_.begin());++dropped_;
        }
        entries_.push_back({tick,Fnv1a64(frame.Pixels()),pad,
                            frame.Width(),frame.Height()});
        return true;
    }
    void Clear(){entries_.clear();dropped_=0;}
    const std::vector<FrameSignature>& Entries() const{return entries_;}
    std::size_t Dropped() const{return dropped_;}
    // The first different sample (or first missing sample) in two traces.
    // A null optional means exact parity of all retained samples.
    std::optional<std::size_t> FirstMismatch(const FrameTrace& rhs) const {
        const auto n=std::min(entries_.size(),rhs.entries_.size());
        for(std::size_t i=0;i<n;++i)
            if(!(entries_[i]==rhs.entries_[i]))return i;
        if(entries_.size()!=rhs.entries_.size())return n;
        return {};
    }
    std::string Csv() const {
        std::ostringstream out;
        out<<"tick,rgba_fnv64,width,height,buttons,stick_x,stick_y\n";
        for(const auto& e:entries_)
            out<<e.tick<<','<<std::hex<<e.rgbaHash<<std::dec<<','
               <<e.width<<','<<e.height<<','<<e.input.buttons<<','
               <<int(e.input.x)<<','<<int(e.input.y)<<'\n';
        return out.str();
    }
private:
    std::size_t capacity_;
    std::size_t dropped_=0;
    std::vector<FrameSignature> entries_;
};
} // namespace tour::recomp
