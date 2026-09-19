#pragma once
#include "ProofPeer.h"
#include "media/SignalingExecutor.h"

namespace proofmedia {
// The external caller waits on futures. Every native operation and teardown
// runs on the owned event loop, with no ProcessMessages or AutoThread.
inline void CheckOwnedNegotiation(screenshare::media::SignalingExecutor& executor) {
    using namespace screenshare::media;
    struct Context {
        std::unique_ptr<webrtc::Thread> network = webrtc::Thread::CreateWithSocketServer();
        std::unique_ptr<webrtc::Thread> worker = webrtc::Thread::Create();
        webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
        Peer host, viewer;
    };
    std::unique_ptr<Context> context;
    std::exception_ptr failure;
    auto execute = [&](std::function<void()> task) {
        auto done = executor.Post([&] {
            try { task(); } catch (...) { failure = std::current_exception(); }
        });
        Require(done.get().error == ExecutorError::None, "Owned negotiation command failed");
        if (failure) std::rethrow_exception(std::exchange(failure, {}));
    };
    try {
        execute([&] {
            context = std::make_unique<Context>();
            Require(context->network->Start() && context->worker->Start(), "Owned negotiation threads failed");
            webrtc::PeerConnectionFactoryDependencies dependencies;
            dependencies.env = webrtc::CreateEnvironment();
            dependencies.network_thread = context->network.get();
            dependencies.worker_thread = context->worker.get();
            dependencies.signaling_thread = webrtc::Thread::Current();
            context->factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
            Require(context->factory != nullptr, "Owned negotiation factory failed");
            for (auto* peer : {&context->host, &context->viewer}) {
                webrtc::PeerConnectionInterface::RTCConfiguration config;
                config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
                auto created = context->factory->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(peer));
                Require(created.ok(), "Owned negotiation peer failed");
                peer->connection = created.MoveValue();
            }
            webrtc::DataChannelInit init;
            auto channel = context->host.connection->CreateDataChannelOrError("owned-negotiation", &init);
            Require(channel.ok(), "Owned negotiation channel failed");
        });
        std::future<NegotiationResult> operation;
        execute([&] { operation = context->host.Negotiation().CreateLocal(context->host.lifecycle.generation(), true); });
        auto offer = operation.get();
        Require(offer.error == NegotiationError::None, "Owned offer failed");
        execute([&] { operation = context->viewer.Negotiation().ApplyRemote(context->viewer.lifecycle.generation(), true, offer.sdp); });
        Require(operation.get().error == NegotiationError::None, "Owned remote offer failed");
        execute([&] { operation = context->viewer.Negotiation().CreateLocal(context->viewer.lifecycle.generation(), false); });
        auto answer = operation.get();
        Require(answer.error == NegotiationError::None, "Owned answer failed");
        execute([&] { operation = context->host.Negotiation().ApplyRemote(context->host.lifecycle.generation(), false, answer.sdp); });
        Require(operation.get().error == NegotiationError::None, "Owned remote answer failed");
        execute([&] { context.reset(); });
    } catch (...) {
        execute([&] { context.reset(); });
        throw;
    }
}
}
