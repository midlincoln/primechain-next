#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include "validator_messages.hpp"

namespace primechain::experiments {

struct LocalPeer {
    Address address;
    std::uint16_t port{0};
};
using PeerExchange = std::function<std::optional<ValidatorReply>(const LocalPeer&, const protocol::Bytes&)>;
enum class PeerFailure { None, BeforeCertificatePublish, AfterCertificatePublish, AfterLocalApply };
constexpr std::size_t kMaxPeerCertificates = 256;

// Main-thread state machine with at most one bounded transport task per peer.
// Fixed-set, single-round, local experiment; no round changes or peer discovery.
class PeerCoordinator {
public:
    PeerCoordinator(std::string path, ContinuousChain& chain, crypto::SignatureKeyPair identity, PeerExchange exchange);
    bool open(bool create, std::string& error);
    void setPeers(std::vector<LocalPeer> peers);
    ValidatorReply handle(const protocol::Bytes& request);
    void tick();
    bool ready() const { return ready_ && chain_.ready(); }
    const std::string& error() const { return error_; }
    void failNextCertificateForTest(PeerFailure point) { failure_ = point; }

private:
    struct Event {
        RequestKind kind;
        protocol::Bytes payload;
        bool applied{false};
    };
    struct Job {
        Event proposal;
        Hash256 subject;
        std::map<Address, protocol::ValidatorVoteV0> votes;
    };
    struct Flight {
        bool delivery;
        std::size_t index;
        Hash256 subject;
        std::future<std::optional<ValidatorReply>> reply;
    };
    Hash256 configurationHash() const;
    Hash256 subject(const Event& event) const;
    bool verifyCertificate(const Event& event, std::string& error) const;
    std::optional<Event> known(const Event& event) const;
    ValidatorReply apply(const Event& event);
    void persist(bool certificate = false);
    void synchronizeJob();
    void publishCertificate();
    void finish(std::size_t peer);
    void failAt(PeerFailure point);
    ValidatorReply failure(const std::string& error) const;

    std::string path_;
    ContinuousChain& chain_;
    crypto::SignatureKeyPair identity_;
    Address local_;
    PeerExchange exchange_;
    std::vector<LocalPeer> peers_;
    std::vector<std::size_t> cursors_;
    std::vector<std::optional<Flight>> flights_;
    std::vector<std::chrono::steady_clock::time_point> next_attempt_;
    std::vector<bool> prefer_delivery_;
    std::vector<Event> outbox_;
    std::optional<Event> intent_;
    std::optional<Job> job_;
    bool opened_{false};
    bool ready_{false};
    std::string error_;
    PeerFailure failure_{PeerFailure::None};
};

} // namespace primechain::experiments
