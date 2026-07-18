#include "ui/UpdatePackageSelection.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QJsonObject Asset(const QString& url, const QString& hash, const QString& signature)
{
    return QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("sha256"), hash},
        {QStringLiteral("signature"), signature},
        {QStringLiteral("size"), 1234},
    };
}

} // namespace

int main()
{
    const QString hash(64, QLatin1Char('a'));
    const QJsonObject manifest{
        {QStringLiteral("version"), QStringLiteral("0.3.1")},
        {QStringLiteral("assets"), QJsonObject{
            {QStringLiteral("portableZip"), Asset(QStringLiteral("https://example.test/app.zip"), hash, QStringLiteral("zip-signature"))},
            {QStringLiteral("windowsInstaller"), Asset(QStringLiteral("https://example.test/setup.exe"), hash, QStringLiteral("setup-signature"))},
        }},
    };

    const auto portable = screenshare::ui::SelectUpdatePackageAsset(manifest, false);
    Require(portable.has_value(), "portable asset was not selected");
    Require(portable->kind == screenshare::ui::UpdatePackageKind::PortableZip, "portable kind is wrong");
    Require(portable->signatureBase64 == QStringLiteral("zip-signature"), "portable signature is wrong");

    const auto installed = screenshare::ui::SelectUpdatePackageAsset(manifest, true);
    Require(installed.has_value(), "installer asset was not selected");
    Require(installed->kind == screenshare::ui::UpdatePackageKind::WindowsInstaller, "installer kind is wrong");
    Require(installed->signatureBase64 == QStringLiteral("setup-signature"), "installer signature is wrong");

    QJsonObject legacyManifest = manifest;
    QJsonObject legacyAssets = legacyManifest.value(QStringLiteral("assets")).toObject();
    QJsonObject legacyZip = legacyAssets.value(QStringLiteral("portableZip")).toObject();
    legacyZip.remove(QStringLiteral("signature"));
    legacyAssets.insert(QStringLiteral("portableZip"), legacyZip);
    legacyAssets.remove(QStringLiteral("windowsInstaller"));
    legacyManifest.insert(QStringLiteral("assets"), legacyAssets);
    legacyManifest.insert(QStringLiteral("signature"), QStringLiteral("legacy-signature"));
    const auto legacyPortable = screenshare::ui::SelectUpdatePackageAsset(legacyManifest, false);
    Require(legacyPortable && legacyPortable->signatureBase64 == QStringLiteral("legacy-signature"),
        "legacy portable signature fallback failed");
    Require(!screenshare::ui::SelectUpdatePackageAsset(legacyManifest, true),
        "installed copy must not fall back to a portable package");

    QTemporaryDir directory;
    Require(directory.isValid(), "temporary directory could not be created");
    Require(!screenshare::ui::IsInstalledApplicationDirectory(directory.path()),
        "empty directory was detected as installed");
    QFile marker(QDir(directory.path()).filePath(QStringLiteral("ScreenShare-installed.marker")));
    Require(marker.open(QIODevice::WriteOnly), "installation marker could not be created");
    marker.close();
    Require(screenshare::ui::IsInstalledApplicationDirectory(directory.path()),
        "installation marker was not detected");

    Require(screenshare::ui::UpdatePackageFileName(
        QStringLiteral("../0.3.1 beta"), screenshare::ui::UpdatePackageKind::WindowsInstaller) ==
        QStringLiteral("ScreenShare-.._0.3.1_beta-update.exe"),
        "installer update filename was not sanitized");
    Require(screenshare::ui::UpdatePackageFileName(
        QStringLiteral("0.3.1"), screenshare::ui::UpdatePackageKind::PortableZip) ==
        QStringLiteral("ScreenShare-0.3.1-update.zip"),
        "portable update filename is wrong");

    return 0;
}
