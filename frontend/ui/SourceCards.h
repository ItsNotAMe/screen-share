#pragma once
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

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
        const QRect preview(card.left()+12,card.top()+10,card.width()-24,qMax(1,card.height()-48));
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
    explicit SourceList(QWidget* parent = nullptr) : QListWidget(parent) {
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        // Qt normally derives this step from the card height. Use a text-sized
        // step so the system wheel setting moves only part of a row at a time.
        verticalScrollBar()->setSingleStep(20);
    }
    void setEmptyMessage(const QString& message) {
        emptyMessage_=message;setAccessibleDescription(message);viewport()->update();
    }
protected:
    void wheelEvent(QWheelEvent* event) override {
        const auto pixels = event->pixelDelta();
        if (pixels.isNull() || (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
            QListWidget::wheelEvent(event);
            return;
        }

        // Use the touchpad's actual movement. QListWidget routes wheel input
        // by angleDelta, which can be rounded or absent for pixel events.
        auto* bar = verticalScrollBar();
        const int previous = bar->value();
        if (qAbs(pixels.y()) >= qAbs(pixels.x())) bar->setValue(previous - pixels.y());
        event->setAccepted(bar->value() != previous);
    }
    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        if(!emptyMessage_.isEmpty()) {
            QPainter painter(viewport());painter.setPen(QColor("#a3b5af"));
            painter.drawText(viewport()->rect().adjusted(16,16,-16,-16),Qt::AlignCenter|Qt::TextWordWrap,emptyMessage_);
        }
    }
    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        // Reserve scrollbar width even while it is hidden. Using viewport width
        // here made wrapping add/remove the scrollbar and oscillate indefinitely.
        constexpr int minimumCellWidth=210;
        const int availableWidth=qMax(1,width()-24);
        const int columns=qBound(1,availableWidth/minimumCellWidth,3);
        const int cellWidth=qMax(120,availableWidth/columns);
        const QSize cell(cellWidth,qBound(128,(cellWidth-40)*9/16+60,220));
        if (gridSize() == cell) return;
        setGridSize(cell);
        for (int row=0; row<count(); ++row) item(row)->setSizeHint(cell);
    }
private:
    QString emptyMessage_;
};

