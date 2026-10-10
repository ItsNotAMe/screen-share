#pragma once
#include <Windows.h>

namespace screenshare {
inline INPUT WindowsRelativeMouseInput(int x, int y) {
    INPUT input{};input.type=INPUT_MOUSE;
    input.mi.dx=x;input.mi.dy=y;
    input.mi.dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    return input;
}
}
