#include "media/IceCandidateHandoff.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace screenshare::media;
void Check(bool ok) { if (!ok) throw std::runtime_error("ICE handoff invariant failed"); }
int main() try {
    std::vector<std::string> received;
    IceCandidateHandoff handoff(2, [&](const auto& item) { received.push_back(item.candidate); return true; });
    Check(handoff.Push(1, {"old", "0", 0}) == IceHandoffError::Stale);
    Check(handoff.Push(2, {"first", "0", 0}) == IceHandoffError::None);
    handoff.RemoteDescriptionReady(1);
    handoff.LocalDescriptionReady(2);
    Check(received.empty() && handoff.pending() == 1);
    handoff.RemoteDescriptionReady(2);
    handoff.Push(2, {"second", "0", 0});
    Check(received == std::vector<std::string>{"first", "second"});
    handoff.Close();
    Check(handoff.Push(2, {"late", "0", 0}) == IceHandoffError::Closed && received.size() == 2);
    IceCandidateHandoff bounded(3, [](const auto&) { return true; });
    for (int i = 0; i < 64; ++i) Check(bounded.Push(3, {"candidate", "0", 0}) == IceHandoffError::None);
    Check(bounded.pending() == 64);
    Check(bounded.Push(3, {"overflow", "0", 0}) == IceHandoffError::Capacity && bounded.pending() == 0);
    bounded.RemoteDescriptionReady(3); bounded.LocalDescriptionReady(3);
    Check(bounded.delivered() == 0);
    IceCandidateHandoff failed(4, [](const auto&) -> bool { throw std::runtime_error("failure"); });
    failed.LocalDescriptionReady(4); failed.RemoteDescriptionReady(4);
    Check(failed.Push(4, {"candidate", "0", 0}) == IceHandoffError::Delivery);
    for (const auto& bad : std::vector<IceCandidateMessage>{{std::string(4097, 'x'), "0", 0}, {"x", "", 0},
             {"x", "0", 32}, {std::string("x\0x", 3), "0", 0}}) {
        IceCandidateHandoff invalid(5, [](const auto&) { return true; });
        Check(invalid.Push(5, bad) == IceHandoffError::Invalid);
    }
    std::cout << "{\"passed\":true,\"mode\":\"ice-candidate-handoff\",\"candidate_limit\":64}\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
