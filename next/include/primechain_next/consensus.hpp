#pragma once
// Consensus core (A3.1): two-phase rounds per instance (h, k), following the tested model
// (experiments/prime_blocks/two_phase_model_tests.cpp, Tendermint Algorithm 1) and
// docs/main-chain-design.md sections A3-1..A3-3. In-process only: messages are values handed to
// and returned by Node; the transport (A3.2) and catch-up over the network (A3.3) come later.
#include "primechain_next/engine.hpp"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <variant>
#include <vector>

namespace primechain::next::consensus {

// Instance (h, k): decision number k within open block h (A3-1).
struct InstanceId {
    std::uint64_t h{0};
    std::uint64_t k{0};
    auto operator<=>(const InstanceId&) const = default;
};

enum class Phase : std::uint8_t { Prevote = 1, Precommit = 2 };

// A3-3: precommit signatures of a quorum, in validator order, with a signer bitmap. It is not part
// of any block's identity (D9).
struct Certificate {
    InstanceId id;
    std::uint64_t round{0};
    Hash256 value{};
    std::vector<std::uint8_t> signers;  // bitmap over the validator set active at the instance
    std::vector<Bytes> signatures;      // one per set bit, in validator order
};

// A proposed value. `previous` is the certificate of the previous instance; embedding it makes
// the proposer-skip counters part of agreed data, so every validator computes the same proposer
// (like Tendermint's LastCommit). Absent only for the very first instance of a chain.
struct Value {
    enum class Kind : std::uint8_t { Range = 1, Prime = 2 } kind{Kind::Range};
    RangeSubmission range;            // Kind::Range
    PrimeSubmission prime;            // Kind::Prime
    Engine::PrimeContents contents;   // Kind::Prime: transfers and governance, in order
    std::optional<Certificate> previous;
};

struct Proposal {
    InstanceId id;
    std::uint64_t round{0};
    std::int64_t valid_round{-1};
    Value value;
    std::uint32_t proposer{0};  // validator index
    Bytes signature;
};

struct Vote {
    InstanceId id;
    std::uint64_t round{0};
    Phase phase{Phase::Prevote};
    std::optional<Hash256> value;  // nullopt = nil
    std::uint32_t signer{0};       // validator index
    Bytes signature;
};

struct Decision {  // a decided value with its certificate; also the unit of catch-up
    InstanceId id;
    Value value;
    Certificate certificate;
};

struct SyncRequest {  // "send me your decisions from instance `from` onward"
    InstanceId from;
};

struct Evidence {  // F15: two different signed votes by one signer at one (h, k, r, phase)
    Vote first;
    Vote second;
};

using Message = std::variant<Proposal, Vote, Decision, SyncRequest, RangeSubmission, PrimeSubmission, Transfer,
                             GovernanceTx>;

// Signature scheme used for consensus messages. Production: ML-DSA (the validator's consensus
// key, D10). Tests may use the forgeable fast mock for large randomized runs without forgers.
struct Crypto {
    std::function<Bytes(const Bytes& secret, const Bytes& payload)> sign;
    std::function<bool(const Bytes& public_key, const Bytes& payload, const Bytes& signature)> verify;
};
Crypto mldsa();
Crypto fastMock();  // signature = sha3(public key || payload); for honest-fault simulations only

struct Timing {  // D11: base 1 s, doubling per round, capped at 60 s; logical milliseconds
    std::uint64_t base_ms{1000};
    std::uint64_t cap_ms{60000};
    std::uint64_t timeout(std::uint64_t round) const;
};

struct Retention {  // A3-3: permanent checkpoints every K blocks, window of W recent blocks
    std::uint64_t checkpoint_interval{1000};
    std::uint64_t window{1000};
};

Bytes encode(const Certificate& certificate);
Bytes encode(const Value& value);
Hash256 valueHash(const Value& value);
Bytes voteSigningPayload(const std::string& network_id, const Hash256& set_digest, const Vote& vote);
Bytes proposalSigningPayload(const std::string& network_id, const Hash256& set_digest, const Proposal& proposal);
// A certificate verifies against the consensus keys (in validator order) and digest of the set
// active at its instance: a quorum of valid precommit signatures for its value.
bool verifyCertificate(const Certificate& certificate, const std::vector<Bytes>& keys, const Hash256& set_digest,
                       const std::string& network_id, const Crypto& crypto);

// Durable consensus state (persist-before-send). A node is restartable from this plus its
// engine's Store; the harness saves it after every event, before outgoing messages leave, and a
// crash happens only between events (a production node needs the same ordering on disk).
struct Durable {
    std::map<InstanceId, Decision> decisions;
    std::map<InstanceId, Certificate> certificates;
    std::map<Hash256, std::uint64_t> missed;
    std::optional<InstanceId> previous_id;
    Hash256 previous_value{};
    std::vector<Bytes> previous_keys;
    Hash256 previous_digest{};
    std::vector<std::uint32_t> previous_candidates;
    std::set<std::uint64_t> set_change_heights;
    InstanceId id;
    std::uint64_t round{0};
    std::optional<Value> locked;
    std::int64_t locked_round{-1};
    std::optional<Value> valid;
    std::int64_t valid_round{-1};
    std::vector<Vote> sent;           // votes signed in the current instance: re-sent, never re-signed
    std::vector<Proposal> proposed;   // proposals signed in the current instance
};

struct Outgoing {
    std::optional<std::uint32_t> to;  // node id; nullopt = broadcast to all peers
    Message message;
};

class Node {
public:
    Node(Bytes consensus_public, Bytes consensus_secret, Engine engine, Crypto crypto, Timing timing = {},
         Retention retention = {});
    // Restart from durable state: the engine (reopened from its Store) is advanced through the
    // logged decisions it has not applied yet, and the instance state is restored.
    static Node restore(Bytes consensus_public, Bytes consensus_secret, Engine engine, const Durable& durable,
                        Crypto crypto, Timing timing = {}, Retention retention = {});

    void start(std::uint64_t now, std::vector<Outgoing>& out);
    void receive(const Message& message, std::uint32_t from, std::uint64_t now, std::vector<Outgoing>& out);
    void tick(std::uint64_t now, std::vector<Outgoing>& out);
    std::optional<std::uint64_t> nextDeadline() const;

    Engine& engine() { return engine_; }
    const Engine& engine() const { return engine_; }
    const InstanceId& instance() const { return id_; }
    std::uint64_t round() const { return round_; }
    std::optional<std::uint32_t> selfIndex() const;
    std::uint32_t proposer(std::uint64_t round) const;  // for the current instance
    const std::map<InstanceId, Decision>& decisions() const { return decisions_; }
    const std::map<InstanceId, Certificate>& certificates() const { return certificates_; }
    const std::vector<Evidence>& evidence() const { return evidence_; }
    const std::map<Hash256, std::uint64_t>& missedTurns() const { return missed_; }
    std::uint64_t timeoutsFired() const { return timeouts_fired_; }
    const std::set<std::uint64_t>& setChangeHeights() const { return set_change_heights_; }
    Durable durable() const;

    void setRelay(bool relay) { relay_ = relay; }  // re-broadcast newly accepted votes and proposals

    // Test hooks for Byzantine behaviour: sign arbitrary votes and proposals with this node's key.
    Vote signVote(const InstanceId& id, std::uint64_t round, Phase phase, const std::optional<Hash256>& value) const;
    void signProposal(Proposal& proposal) const;

private:
    enum class Step : std::uint8_t { Propose, Prevote, Precommit };
    struct Timer {
        std::uint64_t due;
        std::uint8_t kind;  // 0 propose, 1 prevote, 2 precommit
        std::uint64_t round;
    };
    struct RoundState {
        std::optional<Proposal> proposal;
        std::map<std::uint32_t, Vote> prevotes;
        std::map<std::uint32_t, Vote> precommits;
    };

    void loadInstanceContext();
    void resetInstanceState();
    void beginInstance(std::uint64_t now, std::vector<Outgoing>& out);
    void propose(std::uint64_t now, std::vector<Outgoing>& out);
    void startRound(std::uint64_t round, std::uint64_t now, std::vector<Outgoing>& out);
    void evaluate(std::uint64_t now, std::vector<Outgoing>& out);
    void castVote(Phase phase, const std::optional<Hash256>& value, std::vector<Outgoing>& out);
    bool acceptVote(const Vote& vote, bool relay, std::vector<Outgoing>& out);
    bool acceptProposal(const Proposal& proposal);
    bool valid(const Value& value);
    bool verifyCertificate(const Certificate& c, const std::vector<Bytes>& keys, const Hash256& set_digest) const;
    bool decide(const Value& value, const Certificate& certificate, std::uint64_t now, std::vector<Outgoing>& out);
    bool applyValue(const Value& value, std::string& error);
    void updateMissed(const Certificate& previous);
    std::optional<Certificate> buildCertificate(std::uint64_t round, const Hash256& value) const;
    std::optional<Value> getValue();
    void prune();
    void serve(std::uint32_t to, const InstanceId& from, std::uint64_t now, std::vector<Outgoing>& out);
    void requestSync(std::optional<std::uint32_t> to, std::uint64_t now, std::vector<Outgoing>& out);
    void buffer(const Message& message, const InstanceId& id, std::uint32_t from);
    void broadcast(Message message, std::vector<Outgoing>& out) { out.push_back({std::nullopt, std::move(message)}); }
    void schedule(std::uint8_t kind, std::uint64_t round, std::uint64_t now);
    std::size_t count(const std::map<std::uint32_t, Vote>& votes, const std::optional<Hash256>& value, bool any) const;

    Bytes public_;
    Bytes secret_;
    Engine engine_;
    Crypto crypto_;
    Timing timing_;
    Retention retention_;

    // Current instance.
    InstanceId id_;
    std::vector<Bytes> keys_;              // consensus keys of the active set, in validator order
    Hash256 set_digest_{};
    std::vector<std::uint32_t> candidates_;  // proposer rotation after skipping (A3-3)
    std::uint64_t round_{0};
    Step step_{Step::Propose};
    std::optional<Value> locked_;
    std::int64_t locked_round_{-1};
    std::optional<Value> valid_;
    std::int64_t valid_round_{-1};
    std::map<std::uint64_t, RoundState> rounds_;
    std::set<std::uint64_t> prevote_timer_started_, precommit_timer_started_, polka_done_;
    std::vector<Timer> timers_;
    std::vector<Vote> sent_;
    std::vector<Proposal> proposed_;
    std::map<Hash256, bool> valid_cache_;
    std::map<Hash256, Value> values_;     // values seen in proposals of this instance
    bool resume_{false};
    bool relay_{true};

    // Previous instance (for checking the embedded certificate and updating skip counters).
    std::optional<InstanceId> previous_id_;
    Hash256 previous_value_{};
    std::vector<Bytes> previous_keys_;
    Hash256 previous_digest_{};
    std::vector<std::uint32_t> previous_candidates_;

    std::map<InstanceId, Decision> decisions_;
    std::map<InstanceId, Certificate> certificates_;
    std::map<Hash256, std::uint64_t> missed_;  // consensus-key hash -> consecutive missed proposer turns
    std::vector<Evidence> evidence_;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t, std::uint32_t>, Vote> seen_;
    std::map<InstanceId, std::vector<std::pair<Message, std::uint32_t>>> future_;
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t, std::uint32_t>> evidenced_;
    std::map<std::pair<std::uint32_t, InstanceId>, std::uint64_t> last_request_, last_serve_;  // catch-up rate limits
    std::set<Hash256> submitted_;   // submissions already handed to the engine (cleared per block)
    std::size_t buffered_{0};
    std::set<std::uint64_t> set_change_heights_;
    std::uint64_t timeouts_fired_{0};
};

}  // namespace primechain::next::consensus
