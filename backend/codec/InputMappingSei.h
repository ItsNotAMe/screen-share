#pragma once
#include "input/v2/FrameMapping.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace screenshare::input {
// H.264 user_data_unregistered SEI, versioned UUID-owned payload. It travels in
// the same access unit as the image; missing/invalid/duplicate metadata disables
// mapping for that frame. Never retain a mapping from a previous access unit.
inline constexpr std::array<uint8_t,16> MappingUuid{0x71,0x53,0x49,0x4e,0x90,0x21,0x42,0x91,0x83,0x15,0x62,0x07,0x4d,0x50,0x01,0x01};
inline void InsertMappingSei(std::vector<std::byte>& packet, FrameMapping mapping) {
    if (!mapping.Valid()) return;
    std::vector<uint8_t> rbsp{5,36};
    rbsp.insert(rbsp.end(),MappingUuid.begin(),MappingUuid.end());
    auto put=[&](uint64_t value,unsigned count) {while(count--)rbsp.push_back(uint8_t(value>>(count*8)));};
    put(mapping.generation,8);
    for(auto value:{mapping.width,mapping.height,mapping.left,mapping.top,mapping.imageWidth,mapping.imageHeight})put(value,2);
    rbsp.push_back(0x80);
    std::vector<std::byte> nal{std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{6}};
    unsigned zeroes=0;
    for(auto value:rbsp) {
        if(zeroes>=2 && value<=3) {nal.push_back(std::byte{3});zeroes=0;}
        nal.push_back(std::byte(value));zeroes=value==0?zeroes+1:0;
    }
    // Keep SPS/PPS before SEI: the RTP receiver derives keyframe dimensions from
    // the first packet. A leading SEI would hide resize dimensions from stats.
    size_t insertion=0;
    for(size_t i=0;i+4<packet.size();++i) {
        if(packet[i]!=std::byte{0} || packet[i+1]!=std::byte{0})continue;
        const size_t prefix=packet[i+2]==std::byte{1}?3:
            packet[i+2]==std::byte{0} && packet[i+3]==std::byte{1}?4:0;
        if(!prefix || i+prefix>=packet.size())continue;
        const auto type=uint8_t(packet[i+prefix])&31;
        if(type==1 || type==5) {insertion=i;break;}
    }
    packet.insert(packet.begin()+insertion,nal.begin(),nal.end());
}
inline FrameMapping ReadMappingSei(std::span<const uint8_t> packet) {
    if(packet.size()>16*1024*1024)return {};
    FrameMapping result;
    auto start=[&](size_t at) {for(;at+3<=packet.size();++at)if(packet[at]==0 && packet[at+1]==0 && packet[at+2]==1)return at;return packet.size();};
    for(size_t begin=start(0);begin<packet.size();) {
        const size_t header=begin+3, end=start(header);
        if(header>=packet.size())break;
        if((packet[header]&31)==6) {
            if(end-header>4096)return {};
            std::vector<uint8_t> rbsp; unsigned zeroes=0;
            for(size_t i=header+1;i<end;++i) {
                const auto value=packet[i];
                if(zeroes>=2 && value==3) {if(i+1>=end || packet[i+1]>3)return {};zeroes=0;continue;}
                rbsp.push_back(value);zeroes=value==0?zeroes+1:0;
            }
            while(!rbsp.empty() && rbsp.back()==0)rbsp.pop_back();
            if(rbsp.empty() || rbsp.back()!=0x80)return {};
            size_t at=0;
            while(at+2<=rbsp.size() && rbsp[at]!=0x80) {
                unsigned type=0,size=0;
                while(at<rbsp.size() && rbsp[at]==255) {type+=255;++at;}
                if(at>=rbsp.size())return {};type+=rbsp[at++];
                while(at<rbsp.size() && rbsp[at]==255) {size+=255;++at;}
                if(at>=rbsp.size())return {};size+=rbsp[at++];
                if(size>rbsp.size()-at)return {};
                if(type==5 && size>=16 && std::equal(MappingUuid.begin(),MappingUuid.end(),rbsp.begin()+at)) {
                    if(size!=36 || result.generation)return {};
                    size_t cursor=at+16;
                    auto get=[&](unsigned count) {uint64_t value=0;while(count--)value=(value<<8)|rbsp[cursor++];return value;};
                    result.generation=get(8);result.width=uint16_t(get(2));result.height=uint16_t(get(2));
                    result.left=uint16_t(get(2));result.top=uint16_t(get(2));result.imageWidth=uint16_t(get(2));result.imageHeight=uint16_t(get(2));
                    if(!result.Valid())return {};
                }
                at+=size;
            }
        }
        begin=end;
    }
    return result;
}
}
