#pragma once
#include "ProofPeer.h"

namespace proofmedia {
void CheckNegotiation(webrtc::PeerConnectionFactoryInterface& factory) {
    using namespace screenshare::media;
    Peer peer;
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    auto created = factory.CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(&peer));
    Require(created.ok(), "Negotiation test peer creation failed");
    peer.connection = created.MoveValue();
    webrtc::DataChannelInit init;
    auto channel = peer.connection->CreateDataChannelOrError("negotiation-test", &init);
    Require(channel.ok(), "Negotiation test channel failed");
    const auto generation = peer.lifecycle.generation();
    for (int i = 0; i < 25; ++i) {
        auto negotiation = std::make_unique<PeerNegotiation>(peer.connection, generation);
        auto cancelled = negotiation->CreateLocal(generation, true);
        Require(cancelled.wait_for(std::chrono::seconds(0)) != std::future_status::ready,
                "Offer did not exercise pending cancellation");
        Require(negotiation->CreateLocal(generation, true).get().error == NegotiationError::Busy,
                "Overlapping negotiation was not bounded");
        Require(negotiation->ApplyRemote(generation + 1, true, "stale").get().error == NegotiationError::StaleGeneration,
                "Stale remote operation reached active negotiation");
        negotiation->Close();
        Require(cancelled.get().error == NegotiationError::Cancelled, "Pending offer was not cancelled");
        Require(negotiation->CreateLocal(generation, true).get().error == NegotiationError::Closed,
                "Closed negotiation accepted work");
        negotiation.reset(); // Pending native callbacks now hold only weak state.
    }
    PeerNegotiation replacement(peer.connection, generation);
    for (const auto& invalid : {std::string{}, std::string("not SDP"), std::string(60 * 1024 + 1, 'x'), std::string("x\0x", 3)})
        Require(replacement.ApplyRemote(generation, true, invalid).get().error == NegotiationError::Invalid,
                "Invalid SDP reached the native apply path");
    auto failedAnswer = replacement.CreateLocal(generation, false);
    Wait([&] { return failedAnswer.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Answer failure timed out");
    Require(failedAnswer.get().error == NegotiationError::CreateFailed, "Invalid answer creation was reported successful");
    Require(peer.connection->local_description() == nullptr, "Cancelled offer mutated the later negotiation");
    auto offer = replacement.CreateLocal(generation, true);
    Wait([&] { return offer.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Replacement offer timed out");
    const auto result = offer.get();
    Require(result.error == NegotiationError::None && !result.sdp.empty() && peer.connection->local_description(),
            "Replacement negotiation did not apply its local description");
    Require(result.generation == generation && !replacement.localUsername().empty(), "Negotiation lost its identity");
    // Parseable SDP applied to a closed native connection must report the
    // native signaling-state failure without echoing SDP/credentials.
    peer.connection->Close();
    auto failedApply = replacement.ApplyRemote(generation, true, result.sdp);
    Wait([&] { return failedApply.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Apply failure timed out");
    const auto rejected = failedApply.get();
    Require(rejected.error == NegotiationError::ApplyFailed && rejected.sdp.empty(), "Native SDP failure was hidden");
    replacement.Close();
}
}
