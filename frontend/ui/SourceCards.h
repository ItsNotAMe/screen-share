#pragma once
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QResizeEvent>

class SourceCardDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        const auto card = option.rect.adjusted(8,6,-8,-6);
        const bool selected = option.state & QStyle::State_Selected;
        painter->setBrush(QColor(option.state & QStyle::State_MouseOver ? "#111d18" : "#080e0b"));
        painter->setPen(QPen(QColor(selected ? "#38d8c8" : "#30413a"),selected ? 1.5 : 1));
        painter->drawRoundedRect(card,7,7);
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QRect preview(card.left()+12,card.top()+10,card.width()-24,72);
        icon.paint(painter,preview,Qt::AlignCenter,QIcon::Normal);
        const int y = card.bottom()-17;
        painter->setBrush(Qt::NoBrush); painter->setPen(QPen(QColor(selected ? "#38d8c8" : "#8b9d95"),1.5));
        painter->drawEllipse(QPoint(card.left()+18,y),5,5);
        if (selected) { painter->setBrush(QColor("#38d8c8")); painter->drawEllipse(QPoint(card.left()+18,y),2,2); }
        painter->setFont(option.font); painter->setPen(QColor("#dce7e1"));
        painter->drawText(QRect(card.left()+29,y-10,card.width()-37,20),Qt::AlignVCenter,
            option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),Qt::ElideRight,card.width()-37));
        painter->restore();
    }
};
class SourceList final : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        // Reserve scrollbar width even while it is hidden. Using viewport width
        // here made wrapping add/remove the scrollbar and oscillate indefinitely.
        const QSize cell(qMax(120, (width()-24)/2),128);
        if (gridSize() == cell) return;
        setGridSize(cell);
        for (int row=0; row<count(); ++row) item(row)->setSizeHint(cell);
    }
};

