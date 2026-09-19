#pragma once
#include "SourceCards.h"
#include "SessionPopup.h"
#include "UiStyle.h"
#include "capture/DesktopCapturer.h"
#include "media/CaptureSelection.h"
#include <QDialog>
#include <QButtonGroup>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QThread>
#include <QImage>
#include <QHash>
#include <functional>

// Reuses the Create screen's source-card painting and responsive grid.
class SourcePickerDialog final : public SessionPopup {
public:
    std::function<void(screenshare::media::CaptureSelection)> chosen;
    SourcePickerDialog(screenshare::media::CaptureSelection current,bool enumerate,QWidget* parent)
        :SessionPopup("Change source",parent),current_(current),enumerate_(enumerate) {
        setObjectName("SourcePickerDialog");
        auto* tabs=new QHBoxLayout;auto* group=new QButtonGroup(this);
        for(int index=0;index<2;++index) {
            auto* button=new QPushButton(index?"Window":"Display");button->setObjectName("SegmentButton");button->setCheckable(true);
            button->setIcon(uiIcon(index?"window":"display"));button->setChecked(index==int(current.kind==screenshare::media::CaptureKind::Window));
            group->addButton(button,index);tabs->addWidget(button,1);
        }
        windows_=current.kind==screenshare::media::CaptureKind::Window;
        connect(group,&QButtonGroup::idClicked,this,[this](int index){windows_=index==1;ShowCards();});
        refresh_=new QPushButton;refresh_->setIcon(uiIcon("refresh"));refresh_->setAccessibleName("Refresh sources");refresh_->setToolTip("Refresh sources");refresh_->setFixedSize(36,36);heading->insertWidget(1,refresh_);body->addLayout(tabs);
        cards_=new SourceList;cards_->setObjectName("SourceCards");cards_->setItemDelegate(new SourceCardDelegate(cards_));cards_->setMouseTracking(true);
        cards_->setViewMode(QListView::IconMode);cards_->setResizeMode(QListView::Adjust);cards_->setMovement(QListView::Static);cards_->setUniformItemSizes(true);
        cards_->setGridSize(QSize(184,128));cards_->setIconSize(QSize(144,80));cards_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);body->addWidget(cards_,1);
        auto* actions=new QHBoxLayout;actions->addStretch();
        share_=new QPushButton("Share selected source");share_->setObjectName("confirmSourceSelection");actions->addWidget(share_);body->addLayout(actions);
        connect(cards_,&QListWidget::currentItemChanged,this,[this](auto* item){share_->setEnabled(item&&item->data(Qt::UserRole).isValid());});
        connect(share_,&QPushButton::clicked,this,[this]{
            auto* item=cards_->currentItem();if(!item||!item->data(Qt::UserRole).isValid())return;
            const auto selection=sources_.at(item->data(Qt::UserRole).toInt()).selection;
            if(chosen)chosen(selection);accept();
        });
        connect(refresh_,&QPushButton::clicked,this,[this]{Refresh();});Refresh();
    }
    ~SourcePickerDialog() override {if(worker_){worker_->requestInterruption();worker_->wait();delete worker_;}}
protected:
    void done(int result) override {if(worker_)worker_->requestInterruption();SessionPopup::done(result);}
private:
    struct Source {QString name;screenshare::media::CaptureSelection selection;};
    QVector<Source> sources_;
    QHash<int,QIcon> previews_;
    screenshare::media::CaptureSelection current_;
    bool enumerate_,windows_=false;
    SourceList* cards_;QPushButton *refresh_,*share_;QThread* worker_=nullptr;
    void ShowCards() {
        cards_->clear();share_->setEnabled(false);
        for(int index=0;index<sources_.size();++index) {
            const auto& source=sources_[index];if((source.selection.kind==screenshare::media::CaptureKind::Window)!=windows_)continue;
            auto* item=new QListWidgetItem(previews_.value(index,uiIcon(windows_?"window":"display")),source.name,cards_);
            item->setData(Qt::UserRole,index);item->setToolTip(source.name);item->setData(Qt::AccessibleTextRole,source.name);item->setSizeHint(cards_->gridSize());
            if(source.selection.kind==current_.kind&&source.selection.display==current_.display&&source.selection.window==current_.window)cards_->setCurrentItem(item);
        }
        if(!cards_->currentItem()&&cards_->count())cards_->setCurrentRow(0);
        if(!cards_->count()){auto* item=new QListWidgetItem(windows_?"No available windows":"No available displays",cards_);item->setFlags(Qt::NoItemFlags);}
    }
    void Refresh() {
        if(worker_)return;sources_.clear();previews_.clear();
        using namespace screenshare;
        if(!enumerate_)sources_.push_back({"Current source",current_});
        else try {
            for(const auto& display:DesktopCapturer::EnumerateDisplays())if(display.attachedToDesktop) {
                media::CaptureSelection source;source.display=display.index;source.fps=current_.fps;
                sources_.push_back({QString("Display %1 · %2 × %3").arg(display.index+1).arg(display.right-display.left).arg(display.bottom-display.top),source});
            }
            for(const auto& window:DesktopCapturer::EnumerateWindows()) {
                media::CaptureSelection source;source.kind=media::CaptureKind::Window;source.window=window.handle;source.fps=current_.fps;
                sources_.push_back({QString::fromStdWString(window.title),source});
            }
        }catch(...){}
        ShowCards();if(!enumerate_||sources_.empty())return;
        refresh_->setEnabled(false);const auto sources=sources_;
        worker_=QThread::create([this,sources]{
            for(int index=0;index<sources.size()&&!QThread::currentThread()->isInterruptionRequested();++index)try {
                const auto source=sources[index].selection;CaptureConfig config;config.targetFps=15;config.allowDisplayFallback=false;
                config.sourceType=source.kind==media::CaptureKind::Window?CaptureSourceType::Window:CaptureSourceType::Display;
                config.windowHandle=source.window;config.displayIndex=source.display;config.backend=CaptureBackend::WindowsGraphicsCapture;
                DesktopCapturer capture;capture.Start(config);const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                while(!QThread::currentThread()->isInterruptionRequested()&&std::chrono::steady_clock::now()<until) {
                    auto frame=capture.TryCaptureFrame(std::chrono::milliseconds(30));if(!frame||frame->pixels.empty())continue;
                    auto image=QImage(reinterpret_cast<const uchar*>(frame->pixels.data()),frame->width,frame->height,frame->rowPitch,QImage::Format_RGB32).scaled(144,80,Qt::KeepAspectRatio,Qt::SmoothTransformation);
                    QMetaObject::invokeMethod(this,[this,index,image]{
                        QIcon icon;const auto pixels=QPixmap::fromImage(image);icon.addPixmap(pixels,QIcon::Normal);icon.addPixmap(pixels,QIcon::Selected);previews_.insert(index,icon);
                        for(int row=0;row<cards_->count();++row){auto* item=cards_->item(row);if(item->data(Qt::UserRole).isValid()&&item->data(Qt::UserRole).toInt()==index)item->setIcon(icon);}
                    },Qt::QueuedConnection);break;
                }
            }catch(...){}
        });
        connect(worker_,&QThread::finished,this,[this]{auto* worker=worker_;worker_=nullptr;worker->deleteLater();refresh_->setEnabled(true);});worker_->start();
    }
};
