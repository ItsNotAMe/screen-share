#include "ui/UpdatePackageSelection.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>

namespace screenshare::ui {

bool IsInstalledApplicationDirectory(const QString& applicationDirectory)
{
    const QDir directory(applicationDirectory);
    return QFileInfo::exists(directory.filePath(QStringLiteral("ScreenShare-installed.marker"))) ||
        QFileInfo::exists(directory.filePath(QStringLiteral("unins000.exe")));
}

std::optional<UpdatePackageAsset> SelectUpdatePackageAsset(
    const QJsonObject& manifest,
    bool installedApplication)
{
    const QJsonObject assets = manifest.value(QStringLiteral("assets")).toObject();
    const QString assetKey = installedApplication ?
        QStringLiteral("windowsInstaller") : QStringLiteral("portableZip");
    const QJsonObject asset = assets.value(assetKey).toObject();
    if (asset.isEmpty()) {
        return std::nullopt;
    }

    UpdatePackageAsset result;
    result.kind = installedApplication ?
        UpdatePackageKind::WindowsInstaller : UpdatePackageKind::PortableZip;
    result.url = asset.value(QStringLiteral("url")).toString().trimmed();
    result.sha256 = asset.value(QStringLiteral("sha256")).toString().trimmed();
    result.signatureBase64 = asset.value(QStringLiteral("signature")).toString().trimmed();
    result.sizeBytes = asset.value(QStringLiteral("size")).toVariant().toLongLong();

    // Manifests published before installed updates existed stored the portable
    // signature at the root. Keep those working without ever applying that
    // signature to an installer asset.
    if (!installedApplication && result.signatureBase64.isEmpty()) {
        result.signatureBase64 = manifest.value(QStringLiteral("signature")).toString().trimmed();
    }
    return result;
}

QString UpdatePackageFileName(const QString& version, UpdatePackageKind kind)
{
    QString safeVersion = version.left(64);
    static const QRegularExpression unsafeCharacters(QStringLiteral("[^A-Za-z0-9._-]"));
    safeVersion.replace(unsafeCharacters, QStringLiteral("_"));
    if (safeVersion.isEmpty()) {
        safeVersion = QStringLiteral("unknown");
    }
    const QString extension = kind == UpdatePackageKind::WindowsInstaller ?
        QStringLiteral("exe") : QStringLiteral("zip");
    return QStringLiteral("ScreenShare-%1-update.%2").arg(safeVersion, extension);
}

} // namespace screenshare::ui
