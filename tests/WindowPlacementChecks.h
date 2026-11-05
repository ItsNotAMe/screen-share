#pragma once
#include "ui/AppShellWindow.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QScreen>
#include <QSettings>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <stdexcept>

// Shared by the offscreen room UI suite and the native chrome proof. All
// settings use a temporary INI; these checks never alter the user's layout.
inline void WindowPlacementChecks() {
    auto check=[](bool value,const char* message) {if(!value)throw std::runtime_error(message);};
    auto pump=[] {
        QElapsedTimer elapsed;elapsed.start();
        while(elapsed.elapsed()<120) {QApplication::processEvents();QThread::msleep(1);}
    };
    QTemporaryDir temporary;check(temporary.isValid(),"Window placement temporary directory unavailable");
    const auto available=QGuiApplication::primaryScreen()->availableGeometry();
    const QRect wanted(available.topLeft()+QPoint(40,40),QSize(620,400));
    auto toggleMaximized=[&](AppShellWindow& window) {
        const auto controls=window.findChildren<QPushButton*>("WindowControlButton");
        check(controls.size()==2,"Expected minimize and maximize controls");controls[1]->click();pump();
    };
    auto roundTrip=[&](const QString& name,Qt::WindowStates state,bool maximizeBeforeFullscreen=false,bool deferred=false) {
        const auto file=temporary.filePath(name+".ini");
        QRect originalNormal;
        {
            AppShellWindow window;window.setAttribute(Qt::WA_ShowWithoutActivating);
            window.enableWindowPlacementPersistence(file);window.setGeometry(wanted);window.show();pump();
            check(window.geometry()==wanted,"Initial normal placement changed");
            if(maximizeBeforeFullscreen)toggleMaximized(window);
            if(state & Qt::WindowFullScreen)window.showFullScreen();
            else if(state & Qt::WindowMaximized)toggleMaximized(window);
            else if(state & Qt::WindowMinimized)window.showMinimized();
            pump();
            originalNormal=window.normalGeometry();
            bool ready=!deferred;
            if(deferred)window.setCloseHandler([&]{return ready;});
            window.close();pump();
            if(deferred) {
                check(window.isVisible(),"Deferred Close hid the window before drain");
                window.setGeometry(wanted.adjusted(10,10,70,50));pump();
                ready=true;window.close();pump();
            }
        }
        AppShellWindow restored;restored.setAttribute(Qt::WA_ShowWithoutActivating);
        restored.resize(420,260);restored.enableWindowPlacementPersistence(file);
        const auto loadedNormal=restored.normalGeometry();const auto loadedState=restored.windowState();restored.show();pump();
        const auto shownNormal=restored.normalGeometry();
        const bool expectedMaximized=maximizeBeforeFullscreen || (state & Qt::WindowMaximized);
        if(restored.isFullScreen() || restored.isMinimized())throw std::runtime_error(
            QString("Window placement %1 restored temporary state %2 (loaded %3, native maximized %4, actual %5,%6 %7x%8)")
                .arg(name).arg(int(restored.windowState())).arg(int(loadedState))
                .arg(restored.findChild<QWidget*>("AppShellFrame")->property("maximized").toBool())
                .arg(restored.x()).arg(restored.y()).arg(restored.width()).arg(restored.height()).toStdString());
        check(restored.isMaximized()==expectedMaximized,"Maximized placement was not restored");
        if(expectedMaximized)toggleMaximized(restored);
        if(restored.geometry()!=wanted) {
            const auto actual=restored.geometry();
            throw std::runtime_error(QString("Window placement %1: restored %2,%3 %4x%5; expected %6,%7 %8x%9; original normal %10,%11 %12x%13; loaded %14x%15; shown %16x%17")
                .arg(name).arg(actual.x()).arg(actual.y()).arg(actual.width()).arg(actual.height())
                .arg(wanted.x()).arg(wanted.y()).arg(wanted.width()).arg(wanted.height())
                .arg(originalNormal.x()).arg(originalNormal.y()).arg(originalNormal.width()).arg(originalNormal.height())
                .arg(loadedNormal.width()).arg(loadedNormal.height()).arg(shownNormal.width()).arg(shownNormal.height()).toStdString());
        }
        restored.close();pump();
    };
    roundTrip("normal",Qt::WindowNoState);
    roundTrip("maximized",Qt::WindowMaximized);
    roundTrip("fullscreen",Qt::WindowFullScreen);
    roundTrip("maximized-fullscreen",Qt::WindowFullScreen,true);
    roundTrip("minimized",Qt::WindowMinimized);
    roundTrip("deferred",Qt::WindowNoState,false,true);

    for(const auto& bytes:{QByteArray("invalid geometry"),QByteArray(5000,'x')}) {
        const auto file=temporary.filePath("invalid.ini");
        {QSettings settings(file,QSettings::IniFormat);settings.setValue("geometry/v1",bytes);settings.sync();}
        {
            AppShellWindow window;window.resize(620,400);window.enableWindowPlacementPersistence(file);
            check(window.size()==QSize(620,400) && !window.isFullScreen(),"Invalid settings changed the default layout");
        }
        QSettings unchanged(file,QSettings::IniFormat);
        check(unchanged.value("geometry/v1").toByteArray()==bytes,"An unused startup overwrote the saved layout");
    }
    const auto missingScreenFile=temporary.filePath("missing-screen.ini");
    {
        QWidget removedMonitor;removedMonitor.setGeometry(50000,50000,620,400);
        QSettings settings(missingScreenFile,QSettings::IniFormat);
        settings.setValue("geometry/v1",removedMonitor.saveGeometry());settings.sync();
    }
    AppShellWindow relocated;relocated.setAttribute(Qt::WA_ShowWithoutActivating);
    relocated.enableWindowPlacementPersistence(missingScreenFile);relocated.show();pump();
    const auto workingArea=relocated.screen()->availableGeometry();
    check(workingArea.contains(relocated.frameGeometry().center()) &&
          workingArea.contains(relocated.mapToGlobal(QPoint(relocated.width()-30,25))),
          "A missing monitor left the title controls off screen");
    relocated.close();pump();
}
