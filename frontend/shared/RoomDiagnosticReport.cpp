#include <QString>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <dxgi.h>
#include <wrl/client.h>

QString RoomReportBuildVersion() {
    return QString::fromUtf8(SCREENSHARE_REPORT_VERSION);
}

QJsonObject RoomReportBuildInfo() {
    static const auto fingerprint = [] {
        QFile executable(QCoreApplication::applicationFilePath());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        return executable.open(QIODevice::ReadOnly) && hash.addData(&executable) ?
            QString::fromLatin1(hash.result().toHex()) : QString();
    }();
    return {{"executableSha256", fingerprint}, {"qtRuntime", qVersion()}, {"qtBuild", QT_VERSION_STR},
        {"compiled", __DATE__ " " __TIME__}, {"pointerBits", int(sizeof(void*) * 8)}};
}
QJsonArray RoomReportGraphicsInfo() {
    QJsonArray adapters;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))) return adapters;
    for (UINT i = 0; i < 16; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description))) continue;
        LARGE_INTEGER driver{};
        const auto driverResult = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        adapters.append(QJsonObject{{"vendorId", int(description.VendorId)}, {"deviceId", int(description.DeviceId)},
            {"dedicatedVideoBytes", double(description.DedicatedVideoMemory)}, {"sharedSystemBytes", double(description.SharedSystemMemory)},
            {"driverVersion", SUCCEEDED(driverResult) ? QJsonValue(QString::number(quint64(driver.QuadPart), 16)) : QJsonValue(QJsonValue::Null)}});
    }
    return adapters;
}
