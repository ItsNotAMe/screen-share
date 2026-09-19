#pragma once

#include <QtCore/QString>
#include <QtWidgets/QLayout>

namespace UiSpacing {
inline constexpr int PageInset = 32;
inline constexpr int PageVerticalInset = 24;
inline constexpr int SectionGap = 16;
inline constexpr int CardInset = 20;
inline void applyPage(QLayout* layout) {
    layout->setContentsMargins(PageInset, PageVerticalInset, PageInset, PageVerticalInset);
    layout->setSpacing(SectionGap);
}
}

QString uiStyleSheet();
#include <QIcon>
QIcon uiIcon(const QString& name, const QByteArray& color = "#b7c8c0");
class QComboBox;
void styleComboPopup(QComboBox* combo);
class QFormLayout;
void alignOptionRows(QFormLayout* form);
