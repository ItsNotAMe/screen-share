#pragma once
#include "input/v2/InputService.h"
#include "api/data_channel_interface.h"

namespace screenshare::media {
// Signaling-owned observers only parse/enqueue. Device work runs on the input
// service's dedicated owner. Observer destruction precedes native peer teardown.
class InputChannels final {
    class Observer final : public webrtc::DataChannelObserver {
        InputChannels& owner_;
        bool reliable_;
    public:
        Observer(InputChannels& owner,bool reliable) : owner_(owner),reliable_(reliable) {}
        void OnStateChange() override { owner_.Refresh(); }
        void OnMessage(const webrtc::DataBuffer& buffer) override {
            if(buffer.binary)owner_.service_->Receive(owner_.peer_,reliable_,{buffer.data.cdata<uint8_t>(),buffer.data.size()});
        }
    } controlObserver_{*this,true}, stateObserver_{*this,false};
    std::shared_ptr<input::Service> service_;
    std::string peer_,connection_;
    bool ready_=false;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> control_,state_;
    static bool Open(const auto& channel) { return channel && channel->state()==webrtc::DataChannelInterface::kOpen; }
    void Refresh() { service_->Bind(peer_,connection_,ready_ && Open(control_) && Open(state_)); }
public:
    InputChannels(std::shared_ptr<input::Service> service,std::string peer) : service_(std::move(service)),peer_(std::move(peer)) {}
    ~InputChannels() {
        if(control_)control_->UnregisterObserver();
        if(state_)state_->UnregisterObserver();
        service_->Remove(peer_);
    }
    void Attach(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
        const bool reliable=channel->label()=="control";
        auto& slot=reliable?control_:state_;
        if(slot) {channel->Close();return;}
        slot=std::move(channel); slot->RegisterObserver(reliable?&controlObserver_:&stateObserver_); Refresh();
    }
    void Advance(const std::string& connection,bool ready) {
        connection_=connection; ready_=ready; Refresh();
        // At most one bounded application batch can be in SCTP. No state retry.
        auto packets=service_->Drain(peer_,Open(control_) && control_->buffered_amount()==0,
            Open(state_) && state_->buffered_amount()==0);
        for(const auto& packet:packets) {
            auto& channel=packet.reliable?control_:state_;
            if(!channel || !channel->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(packet.bytes.data(),packet.bytes.size()),true))) {
                if(packet.reliable) {service_->TransportFailed(peer_);break;}
            }
        }
    }
};
}
