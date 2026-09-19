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
#include <iostream>
#include <stdexcept>

static void Check(bool value) { if(!value) throw std::runtime_error("Update deferral check failed"); }
template<class F> void Wait(F condition) {
    QElapsedTimer elapsed; elapsed.start();
    while(!condition()) { Check(elapsed.elapsed()<5000); QApplication::processEvents(); QThread::msleep(1); }
}
struct UpdateManagerTestAccess {
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
    try { UpdateManagerTestAccess::Run(); std::cout<<"Update session deferral passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
