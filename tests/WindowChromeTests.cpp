#include "ui/AppShellWindow.h"
#include <QApplication>
#include <QPushButton>
#include <QElapsedTimer>
#include <QThread>
#include <QtMath>
#include <Windows.h>
#include <iostream>
#include <stdexcept>

void Pump() {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<150){QApplication::processEvents();QThread::msleep(1);}
}
int main(int argc,char** argv) {
    QApplication application(argc,argv);
    try {
        AppShellWindow window;window.resize(850,550);
        window.setAttribute(Qt::WA_ShowWithoutActivating);window.show();Pump();
        const auto hwnd=reinterpret_cast<HWND>(window.winId());
        for(bool maximize:{false,true,false}) {
            ShowWindow(hwnd,maximize?SW_MAXIMIZE:SW_RESTORE);Pump();
            if(bool(IsZoomed(hwnd))!=maximize)throw std::runtime_error("Window state did not change");
            auto buttons=window.findChildren<QPushButton*>("WindowControlButton");
            buttons.append(window.findChildren<QPushButton*>("WindowCloseButton"));
            if(buttons.size()!=3)throw std::runtime_error("Missing window buttons");
            for(auto* button:buttons) {
                // Native screen coordinates used before Windows delivers hover.
                for(QPoint point:{button->rect().center(),QPoint(3,3),QPoint(button->width()-4,button->height()-4)}) {
                    const auto local=button->mapTo(&window,point);
                    POINT native{qRound(local.x()*window.devicePixelRatioF()),qRound(local.y()*window.devicePixelRatioF())};
                    ClientToScreen(hwnd,&native);
                    const auto hit=SendMessageW(hwnd,WM_NCHITTEST,0,MAKELPARAM(native.x,native.y));
                    if(hit!=HTCLIENT)throw std::runtime_error("Caption button intercepted by native hit testing");
                }
            }
        }
        std::cout<<"Caption button hit tests passed at scale "<<window.devicePixelRatioF()<<" (normal, maximized, restored)\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
