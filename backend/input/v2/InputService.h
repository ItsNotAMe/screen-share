#pragma once
#include "InputProtocol.h"
#include <memory>
#include <vector>

namespace screenshare::input {
enum class Reason { None, Unavailable, Revoked, Disconnected, Watchdog, Backpressure, Backend, SourceChanged, Ownership };
struct Status {
    std::string peer;
    uint8_t requested = 0, granted = 0;
    uint64_t permission = 0, applied = 0, rejected = 0, coalesced = 0;
    bool ready = false;
    bool grantPending = false;
    bool revokePending = false; // Viewer waits for host permission acknowledgement.
    Reason reason = Reason::None;
    // Local service observations only, never network or input-to-photon latency.
    // Timings expire after one second and reset on permission/connection changes.
    std::optional<uint64_t> queueWaitUs, backendApplyUs;
    unsigned reliableQueued = 0, stateQueued = 0;
    bool transportBlocked = false;
};
// Invoked exclusively on the service's owned input thread, outside the public
// mailbox mutex. Driver operations must return promptly; hung native calls cannot
// be preempted. Late grant completion is revalidated against the permission epoch.
// Release must neutralize every held device for this peer.
// No OS injection exists in the portable service. Missing sink means no grants.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool Grant(const std::string& peer, uint8_t capabilities, int padSlot) = 0;
    virtual bool Apply(const std::string& peer, const Event&) = 0;
    virtual void Release(const std::string& peer) noexcept = 0;
    virtual bool Healthy(const std::string&) { return true; }
};
// Thread-safe high-frequency port. Kept separate from RoomSession status and HTTP.
// A true return means bounded local acceptance, never host permission or injection.
class Port {
public:
    virtual ~Port() = default;
    virtual bool Request(const std::string&, uint8_t) = 0;
    virtual bool Grant(const std::string&, uint8_t) = 0;
    virtual void Revoke(const std::string& peer = {}) = 0;
    virtual bool Submit(const std::string&, Event) = 0;
    // Polling owners are tied to one grant. A late read/cleanup from an old
    // owner must neither submit into nor revoke a newer permission epoch.
    virtual bool SubmitIfCurrent(const std::string&, uint64_t permission, Event) = 0;
    virtual void RevokeIfCurrent(const std::string&, uint64_t permission) = 0;
    virtual std::vector<Status> Read() const = 0;
};
struct Packet { bool reliable; std::vector<uint8_t> bytes; };
class Service final : public Port {
public:
    explicit Service(bool host, std::shared_ptr<Sink> = {});
    ~Service() override;
    // Native transport binds the authenticated peer to each negotiated connection.
    // Binding changes and ready=false invalidate grants; no implicit regrant.
    void Bind(const std::string& peer, const std::string& connection, bool ready);
    void Remove(const std::string& peer);
    void Receive(const std::string& peer, bool reliable, std::span<const uint8_t>);
    std::vector<Packet> Drain(const std::string& peer, bool reliableWritable, bool stateWritable);
    void TransportFailed(const std::string& peer);
    // Revoke before changing source/coordinate geometry or controller allocation.
    void Configure(uint8_t allowedCapabilities, unsigned localGamepads);
    // Runtime-owner only (not concurrent with another Close/destruction).
    // Invalidates immediately; joins the input owner and releases sinks.
    void Close();
    bool Request(const std::string&, uint8_t) override;
    bool Grant(const std::string&, uint8_t) override;
    void Revoke(const std::string& peer = {}) override;
    bool Submit(const std::string&, Event) override;
    bool SubmitIfCurrent(const std::string&, uint64_t permission, Event) override;
    void RevokeIfCurrent(const std::string&, uint64_t permission) override;
    std::vector<Status> Read() const override;
private:
    bool SubmitImpl(const std::string&, Event, std::optional<uint64_t> permission);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
