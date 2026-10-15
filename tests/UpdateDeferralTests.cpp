#include "ui/UpdateManager.h"
#include "ui/AppShellWindow.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QDialog>
#include <QDir>
#include <QScrollArea>
#include <QScrollBar>
#include <iostream>
#include <stdexcept>
#include <source_location>

static void Check(bool value, std::source_location at = std::source_location::current()) {
    if(!value) throw std::runtime_error("Update check failed at line " + std::to_string(at.line()));
}
template<class F> void Wait(F condition) {
    QElapsedTimer elapsed; elapsed.start();
    while(!condition()) { Check(elapsed.elapsed()<5000); QApplication::processEvents(); QThread::msleep(1); }
}
struct UpdateManagerTestAccess {
    static void Layout() {
        AppShellWindow shell;
        UpdateManager manager(&shell);
        UpdateManager::UpdateInfo update;
        update.version = "1.0.9";
        update.notes = {
            "Fix missing audio when sharing Miyomu and other apps that play sound through WebView2 helpers.",
            "Automatically follow the selected app audio process as playback starts or helpers restart, while keeping other apps outside the capture."
        };
        const auto previews = qEnvironmentVariable("SCREENSHARE_UI_PREVIEWS");
        for (const bool longList : {false, true}) {
            if (longList) for (int i = 0; i < 14; ++i) update.notes.push_back(QString("Additional fix %1: keep longer release notes readable when downloading an update, resizing the dialog, or using display scaling.").arg(i + 1));
            manager.showUpdateDialog(update);
            auto* dialog = shell.findChild<QDialog*>("UpdateDialog"); Check(dialog);
            auto* scroll = dialog->findChild<QScrollArea*>("UpdateBodyScroll"); Check(scroll);
            auto* status = dialog->findChild<QLabel*>("UpdateStatus"); Check(status);
            auto* progress = dialog->findChild<QProgressBar*>("UpdateProgress"); Check(progress);
            auto* install = dialog->findChild<QPushButton*>("UpdatePrimary"); Check(install);
            auto* later = dialog->findChild<QPushButton*>("UpdateSecondary"); Check(later);
            for (const auto size : {QSize(560, 600), QSize(420, 440), QSize(760, 700)}) {
                dialog->resize(size); QApplication::processEvents();
                const auto buttonBefore = install->mapTo(dialog, QPoint());
                auto verify = [&] {
                    QApplication::processEvents();
                    Check(scroll->widget()->width() <= scroll->viewport()->width());
                    for (auto* label : dialog->findChildren<QLabel*>()) {
                        if (label->hasHeightForWidth() && label->isVisible()) Check(label->height() >= label->heightForWidth(label->width()));
                    }
                    for (auto* button : {install, later}) {
                        const QRect bounds(button->mapTo(dialog, QPoint()), button->size());
                        Check(dialog->rect().contains(bounds));
                        Check(button->width() >= button->minimumSizeHint().width());
                    }
                    Check(progress->height() >= progress->fontMetrics().height() + 4);
                };
                verify();
                status->setText("Downloading update..."); status->show(); progress->show(); progress->setValue(63);
                verify(); Check(buttonBefore == install->mapTo(dialog, QPoint()));
                status->setText("Could not download the update. Check your internet connection and try again.");
                verify(); // Longer recovery messages may wrap; actions stay reachable.
                status->setText("Update verified and ready to install."); install->setText("Install and restart"); progress->setValue(100);
                verify();
                if (!previews.isEmpty()) {
                    Check(QDir().mkpath(previews));
                    Check(dialog->grab().save(QDir(previews).filePath(QString("update-%1-%2.png").arg(longList ? "long" : "short").arg(size.width()))));
                }
                if (longList) {
                    Check(scroll->verticalScrollBar()->maximum() > 0);
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    QApplication::processEvents();
                    auto* security = dialog->findChild<QLabel*>("UpdateSecurity"); Check(security);
                    Check(scroll->viewport()->rect().contains(QRect(security->mapTo(scroll->viewport(), QPoint()), security->size())));
                    scroll->verticalScrollBar()->setValue(0);
                }
                status->hide(); progress->hide(); install->setText("Download Update");
            }
            dialog->close(); QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
    static void Run() {
        AppShellWindow shell;
        bool active = true;
        shell.hasActiveSession = [&] { return active; };
        UpdateManager manager(&shell);
        QTcpServer server; Check(server.listen(QHostAddress::LocalHost));
        const QByteArray payload("verified-update-deferral-fixture");
        QObject::connect(&server,&QTcpServer::newConnection,&server,[&] {
            auto* socket=server.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,payload] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload);
                socket->disconnectFromHost();
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        UpdateManager::UpdateInfo update;
        update.version="0.0.0-deferral-test";
        update.packageUrl=QString("http://127.0.0.1:%1/package").arg(server.serverPort());
        update.sha256=QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex());
        QProgressBar progress; QLabel status; QPushButton install, later;
        manager.downloadUpdate(update,&progress,&status,&install,&later);
        Wait([&] { return status.text().contains("Leave your room"); });
        Check(!install.isEnabled() && later.isEnabled());
        active=false;
        Wait([&] { return install.isEnabled(); });
        Check(install.text()=="Install and restart");
        // A new session between timer refresh and click must still block launch.
        active=true; install.click();
        Check(status.text().contains("Leave your room"));
        QString error;
        Check(!manager.launchUpdater(update,"unused",&error));
        Check(error.contains("Leave your room"));
        active=false;
        Wait([&] { return install.isEnabled() && status.text().contains("verified"); });
        Check(progress.value()==100);
        // Closing the update UI during an outstanding transfer must cancel it.
        auto* transient=new QWidget;
        auto* transientStatus=new QLabel(transient);
        auto* transientProgress=new QProgressBar(transient);
        auto* transientInstall=new QPushButton(transient);
        auto* transientLater=new QPushButton(transient);
        manager.downloadUpdate(update,transientProgress,transientStatus,transientInstall,transientLater);
        delete transient;
        QApplication::processEvents();
    }
};
int main(int argc,char** argv) {
    QApplication application(argc,argv);
    QApplication::setStyle("Fusion");
    try { UpdateManagerTestAccess::Layout(); UpdateManagerTestAccess::Run(); std::cout<<"Update layout, scrolling and session deferral passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
