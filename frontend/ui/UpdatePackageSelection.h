#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <optional>

namespace screenshare::ui {

enum class UpdatePackageKind {
    PortableZip,
    WindowsInstaller,
};

struct UpdatePackageAsset {
    UpdatePackageKind kind = UpdatePackageKind::PortableZip;
    QString url;
    QString sha256;
    QString signatureBase64;
    qint64 sizeBytes = 0;
};

[[nodiscard]] bool IsInstalledApplicationDirectory(const QString& applicationDirectory);
[[nodiscard]] std::optional<UpdatePackageAsset> SelectUpdatePackageAsset(
    const QJsonObject& manifest,
    bool installedApplication);
[[nodiscard]] QString UpdatePackageFileName(
    const QString& version,
    UpdatePackageKind kind);

} // namespace screenshare::ui
