#pragma once

#include <QtCore/QObject>

class ScreenAwakeGuard final : public QObject {
public:
    explicit ScreenAwakeGuard(QObject* parent = nullptr);
    ~ScreenAwakeGuard() override;

    void setActive(bool active);
    bool active() const;

private:
    bool active_ = false;
};
