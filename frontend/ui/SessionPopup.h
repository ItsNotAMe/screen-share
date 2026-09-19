#pragma once
#include "UiStyle.h"
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QShortcut>
#include <QApplication>
#include <QPointer>
#include <QEvent>
#include <QKeyEvent>

// A centered dialog inside the session, with no separate native window.
class SessionPopup : public QDialog {
public:
    SessionPopup(const QString& title, QWidget* parent) : QDialog(parent, Qt::Widget) {
        setWindowFlags(Qt::Widget);
        setObjectName("SessionPopup");setAttribute(Qt::WA_StyledBackground);
        auto* outer=new QVBoxLayout(this);outer->setContentsMargins(24,24,24,24);
        panel_=new QWidget;panel_->setObjectName("SessionPopupPanel");panel_->setAttribute(Qt::WA_StyledBackground);
        body=new QVBoxLayout(panel_);body->setContentsMargins(24,24,24,24);body->setSpacing(16);
        heading=new QHBoxLayout;auto* label=new QLabel(title);label->setObjectName("SectionHeading");heading->addWidget(label,1);
        auto* close=new QPushButton;close->setIcon(uiIcon("window-close"));close->setAccessibleName("Close");close->setToolTip("Close");close->setFixedSize(36,36);heading->addWidget(close);
        body->addLayout(heading);outer->addWidget(panel_,0,Qt::AlignCenter);
        connect(close,&QPushButton::clicked,this,&QDialog::reject);
        parent->installEventFilter(this);hide();
    }
    void open() override { previousFocus_=QApplication::focusWidget();Fit();show();raise();focusNextPrevChild(true); }
    void setPanelSize(QSize size){panelSize_=size;Fit();}
    QVBoxLayout* body;
    QHBoxLayout* heading;
protected:
    bool event(QEvent* event) override {
        if((event->type()==QEvent::ShortcutOverride||event->type()==QEvent::KeyPress)&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape) {
            if(event->type()==QEvent::KeyPress)reject();event->accept();return true;
        }
        return QDialog::event(event);
    }
    void done(int result) override {QDialog::done(result);if(previousFocus_)previousFocus_->setFocus();}
    bool eventFilter(QObject* object,QEvent* event) override {
        if(object==parentWidget()&&event->type()==QEvent::Resize)Fit();
        return QDialog::eventFilter(object,event);
    }
    bool focusNextPrevChild(bool next) override {
        auto* current=QApplication::focusWidget();if(!current||!isAncestorOf(current))current=this;
        auto* candidate=current;
        do {candidate=next?candidate->nextInFocusChain():candidate->previousInFocusChain();
            if(candidate!=this&&isAncestorOf(candidate)&&candidate->isVisible()&&candidate->isEnabled()&&(candidate->focusPolicy()&Qt::TabFocus)) {
                candidate->setFocus(next?Qt::TabFocusReason:Qt::BacktabFocusReason);return true;
            }
        }while(candidate!=current);
        return false;
    }
private:
    QWidget* panel_;
    QSize panelSize_{660,520};
    QPointer<QWidget> previousFocus_;
    void Fit(){setGeometry(parentWidget()->rect());panel_->setFixedSize(qMin(panelSize_.width(),qMax(1,width()-48)),qMin(panelSize_.height(),qMax(1,height()-48)));}
};
