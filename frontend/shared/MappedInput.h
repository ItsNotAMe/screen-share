#pragma once
#include "core/ScreenShareSession.h"
#include "input/v2/InputProtocol.h"
#include <algorithm>

inline std::optional<screenshare::input::Event> MappedInput(const screenshare::RemoteInputEvent& value) {
    using namespace screenshare;
    if(!value.sourceMapping.Valid())return {};
    input::Event event;event.sourceGeneration=value.sourceMapping.generation;
    switch(value.kind) {
    case RemoteInputKind::MouseMove:event.kind=input::Kind::Pointer;break;
    case RemoteInputKind::MouseButton:event.kind=input::Kind::Button;event.button=uint8_t(value.button);event.down=value.pressed;break;
    case RemoteInputKind::MouseScroll:event.kind=input::Kind::Wheel;event.wheelX=int16_t(std::clamp(value.scrollX,-1200,1200));event.wheelY=int16_t(std::clamp(value.scrollY,-1200,1200));break;
    case RemoteInputKind::Key:event.kind=input::Kind::Key;event.key=uint16_t(value.key);event.scan=uint16_t(value.scancode);event.down=value.pressed;break;
    default:return {};
    }
    if(value.relativeMouse && event.kind!=input::Kind::Key) {
        if(event.kind==input::Kind::Pointer) {event.kind=input::Kind::RelativePointer;event.x=value.normX;event.y=value.normY;}
        else if(event.kind==input::Kind::Button)event.kind=input::Kind::RelativeButton;
        else if(event.kind==input::Kind::Wheel)event.kind=input::Kind::RelativeWheel;
    } else if(event.kind!=input::Kind::Key) {
        const auto point=value.sourceMapping.Point(value.normX,value.normY);if(!point)return {};
        event.x=point->first;event.y=point->second;
    }
    return input::Valid(event)?std::optional(event):std::nullopt;
}
