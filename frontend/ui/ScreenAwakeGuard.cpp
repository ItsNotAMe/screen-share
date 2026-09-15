#include "ui/ScreenAwakeGuard.h"

#ifdef _WIN32
#include <windows.h>
#endif

ScreenAwakeGuard::ScreenAwakeGuard(QObject* parent)
    : QObject(parent)
{
}

ScreenAwakeGuard::~ScreenAwakeGuard()
{
    setActive(false);
}

void ScreenAwakeGuard::setActive(bool active)
{
    if (active_ == active) {
        return;
    }

    active_ = active;

#ifdef _WIN32
    SetThreadExecutionState(active_
        ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
        : ES_CONTINUOUS);
#endif
}

bool ScreenAwakeGuard::active() const
{
    return active_;
}
