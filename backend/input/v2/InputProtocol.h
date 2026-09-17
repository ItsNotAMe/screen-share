#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace screenshare::input {
enum Capability : uint8_t { Mouse = 1, Keyboard = 2, Gamepad = 4 };
enum class Kind : uint8_t { Request, Permission, Release, Heartbeat, Pointer, Button, Wheel, Key, Pad };
struct Event {
    Kind kind = Kind::Heartbeat;
    uint8_t capabilities = 0, button = 0;
    bool down = false;
    float x = 0, y = 0;
    int16_t wheelX = 0, wheelY = 0;
    uint16_t key = 0, scan = 0, buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    std::array<int16_t, 4> axes{};
};
struct Message {
    std::string connection;
    uint64_t permission = 0, sequence = 0;
    Event event;
};
inline bool Replaceable(Kind kind) { return kind == Kind::Pointer || kind == Kind::Pad || kind == Kind::Heartbeat; }
inline bool Valid(const Event& e) {
    if (uint8_t(e.kind) > uint8_t(Kind::Pad)) return false;
    switch (e.kind) {
    case Kind::Request: return e.capabilities && !(e.capabilities & ~7);
    case Kind::Permission: return !(e.capabilities & ~7);
    case Kind::Pointer: case Kind::Button:
        return std::isfinite(e.x) && std::isfinite(e.y) && e.x >= 0 && e.x <= 1 && e.y >= 0 && e.y <= 1 &&
            (e.kind != Kind::Button || e.button < 5);
    case Kind::Key: return e.key > 0 && e.key <= 255 && e.scan <= 0x1ff;
    case Kind::Wheel: return e.wheelX >= -1200 && e.wheelX <= 1200 && e.wheelY >= -1200 && e.wheelY <= 1200;
    case Kind::Pad: return !(e.buttons & ~uint16_t(0xf3ff));
    default: return true;
    }
}
// Explicit big-endian fields. Variable payloads have exact lengths; no native
// structs, timestamps, credentials, addresses or caller-selected pad slots.
inline std::vector<uint8_t> Encode(const Message& m) {
    if (m.connection.empty() || m.connection.size() > 128 || !m.sequence || !Valid(m.event) ||
        (!m.permission && m.event.kind != Kind::Request)) return {};
    std::vector<uint8_t> b{'S','I','N',1,uint8_t(m.event.kind),uint8_t(m.connection.size())};
    auto put = [&](uint64_t n, unsigned size) { while (size--) b.push_back(uint8_t(n >> (size * 8))); };
    put(m.permission,8); put(m.sequence,8);
    b.insert(b.end(),m.connection.begin(),m.connection.end());
    const auto& e=m.event;
    switch (e.kind) {
    case Kind::Request: case Kind::Permission: put(e.capabilities,1); break;
    case Kind::Pointer: case Kind::Button:
        put(std::bit_cast<uint32_t>(e.x),4); put(std::bit_cast<uint32_t>(e.y),4);
        if(e.kind==Kind::Button) { put(e.button,1); put(e.down,1); } break;
    case Kind::Wheel: put(uint16_t(e.wheelX),2); put(uint16_t(e.wheelY),2); break;
    case Kind::Key: put(e.key,2); put(e.scan,2); put(e.down,1); break;
    case Kind::Pad:
        put(e.buttons,2); put(e.leftTrigger,1); put(e.rightTrigger,1);
        for(auto axis:e.axes) put(uint16_t(axis),2); break;
    default: break;
    }
    return b;
}
inline std::optional<Message> Decode(std::span<const uint8_t> b) {
    if(b.size()<23 || b.size()>162 || b[0]!='S' || b[1]!='I' || b[2]!='N' || b[3]!=1 || b[4]>8 || !b[5] || b[5]>128) return {};
    constexpr unsigned sizes[]={1,1,0,0,8,10,4,5,12};
    if(b.size()!=22+b[5]+sizes[b[4]]) return {};
    size_t at=6;
    auto get=[&](unsigned size) { uint64_t n=0; while(size--) n=(n<<8)|b[at++]; return n; };
    Message m; m.event.kind=Kind(b[4]); m.permission=get(8); m.sequence=get(8);
    m.connection.assign(reinterpret_cast<const char*>(b.data()+at),b[5]); at+=b[5];
    auto& e=m.event;
    switch(e.kind) {
    case Kind::Request: case Kind::Permission: e.capabilities=uint8_t(get(1)); break;
    case Kind::Pointer: case Kind::Button:
        e.x=std::bit_cast<float>(uint32_t(get(4))); e.y=std::bit_cast<float>(uint32_t(get(4)));
        if(e.kind==Kind::Button) { e.button=uint8_t(get(1)); const auto down=get(1); if(down>1)return {}; e.down=down!=0; } break;
    case Kind::Wheel: e.wheelX=std::bit_cast<int16_t>(uint16_t(get(2))); e.wheelY=std::bit_cast<int16_t>(uint16_t(get(2))); break;
    case Kind::Key: {
        e.key=uint16_t(get(2)); e.scan=uint16_t(get(2)); const auto down=get(1); if(down>1)return {}; e.down=down!=0; break;
    }
    case Kind::Pad:
        e.buttons=uint16_t(get(2)); e.leftTrigger=uint8_t(get(1)); e.rightTrigger=uint8_t(get(1));
        for(auto& axis:e.axes) axis=std::bit_cast<int16_t>(uint16_t(get(2))); break;
    default: break;
    }
    if(!m.sequence || (!m.permission && e.kind!=Kind::Request) || !Valid(e))return {};
    return m;
}
}
