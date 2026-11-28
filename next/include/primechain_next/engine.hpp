#pragma once
// In-memory main-chain engine (A2.1 + A2.2). One authoritative state, updated incrementally;
// nothing is reloaded per request (see docs/v1-baseline-measurements.md). Optional persistence
// (D19) appends every sealed block to a Store and snapshots the state every K blocks.
#include "primechain_next/assets.hpp"
#include "primechain_next/model.hpp"
#include "primechain_next/parameters.hpp"
#include "primechain_next/registry.hpp"
#include "primechain_next/store.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace primechain::next {

struct Policy {
    std::string network_id{"primechain-next-dev"};
    Limits limits;
    // Genesis starting values of some parameters (D20); they seed the parameter table and are
    // changed afterwards only by vote. Other parameters start from `parameters`, else their defaults.
    std::uint64_t reward_micro_units{1000000};  // D8
    std::uint64_t min_fee_micro_units{1};       // D16 (v1: 1 micro-unit)
    std::uint64_t max_outputs{1000};            // D16 bulk-send cap
    std::uint64_t epoch_length{100};            // D17: the fee pool is distributed every N blocks
    std::uint64_t governance_notice{100};       // A3-2: minimum blocks between quorum and effect
    std::uint64_t proposer_skip_after{3};       // A3-3: skip a proposer after M consecutive missed turns
    ParameterValues parameters;                 // overrides of other starting values, by parameter id
    std::uint64_t snapshot_interval{1000};      // D19: local setting, not a protocol parameter
    ParameterValues genesisParameters() const;
    Hash256 digest() const;                     // genesis configuration: network, limits, starting table
};

// D18: the genesis validator set (a trusted configuration, like v1's validators.txt).
struct Genesis {
    std::vector<ValidatorRecord> validators;
};


class Engine {
public:
    // Throws std::invalid_argument if a genesis validator binding is invalid.
    explicit Engine(Policy policy = {}, Genesis genesis = {});

    const Policy& policy() const { return policy_; }
    std::uint64_t height() const { return height_; }
    const Integer& chainFrontier() const { return chain_frontier_; }
    const Integer& acceptedFrontier() const { return accepted_frontier_; }
    const Hash256& tip() const { return tip_; }
    // Blocks held in memory: the whole chain for an in-memory engine, or the blocks since the
    // loaded snapshot for an opened one (the full history is in the Store).
    const std::vector<Block>& blocks() const { return blocks_; }
    std::size_t pendingCount() const { return pending_.size(); }
    std::size_t pendingTransferCount() const { return pending_transfers_.size(); }
    Amount balance(const Address& address, const Integer& asset) const;
    Amount feePoolBalance(const Integer& asset) const;  // validator fee pool (D16)
    // B3.3: balances are kept in the asset ledger, with an ownership history (b3-plan item 5),
    // so main-chain primes and ring primes follow one rule.
    const AssetLedger& assets() const { return ledger_; }
    // R6, rule 4: the terms a ring opened in block `height` gets from the parameters in force.
    RingTerms ringTermsAt(std::uint64_t height) const;
    std::uint64_t nonce(const Address& address) const; // last used nonce (0 = none yet)
    const std::vector<ValidatorRecord>& validators() const { return validators_; }
    Hash256 validatorSetDigest() const { return validatorEpochDigest(); }
    Hash256 stateRoot() const;
    static std::size_t quorum(std::size_t n) { return n == 0 ? 0 : (2 * n + 2) / 3; }  // ceil(2n/3)

    // Governance (A3-2): votes and ready messages are tallied on chain.
    bool submitGovernance(const GovernanceTx& tx, std::string& error);  // form and signature only
    std::uint64_t minFee() const { return static_cast<std::uint64_t>(parameter(Param::MinFee)); }
    // D20: the value in force for the next block (height() + 1), or for any block height.
    u128 parameter(Param id) const { return params_.value(id, height_ + 1); }
    u128 parameterAt(Param id, std::uint64_t height) const { return params_.value(id, height); }
    const ParameterTable& parameters() const { return params_; }
    std::size_t pendingGovernanceCount() const { return pending_governance_.size(); }
    std::size_t scheduledChanges() const { return scheduled_.size(); }
    std::size_t readyCount() const { return ready_.size(); }  // recorded "ready" messages
    std::size_t votesFor(const Hash256& proposal_id) const;

    // Hooks for consensus (A3-1): the proposer builds a value from its queues; every validator
    // checks a proposed value objectively against its own state, and applies a decided value
    // identically, so engines stay bit-identical.
    struct PrimeContents {
        std::vector<Transfer> transfers;
        std::vector<GovernanceTx> governance;
        std::vector<RingCommit> ring_commits;  // B3.4
        std::vector<RingPayee> ring_payees;
        std::vector<RingReveal> ring_reveals;  // carried by the main block until ring batches (B3.6)
        Hash256 header_hash{};  // hash of the sealed header, so a certificate binds the block (F7)
    };
    std::optional<RangeSubmission> bestPendingRange() const;  // highest end, ties by arrival
    bool applyDecidedRange(const RangeSubmission& range, std::string& error);
    bool submitPrime(const PrimeSubmission& prime, std::string& error);  // form and signature only
    std::optional<PrimeSubmission> bestPendingPrime() const;           // first one valid now
    std::optional<PrimeContents> buildPrimeContents(const PrimeSubmission& prime, std::string& error) const;
    bool checkPrimeValue(const PrimeSubmission& prime, const PrimeContents& contents, std::string& error) const;
    std::optional<Block> applyDecidedPrime(const PrimeSubmission& prime, const PrimeContents& contents,
                                           std::string& error);

    // Ranges (D1, D2, D4).
    bool validateRange(const RangeSubmission& range, std::string& error) const;
    bool submitRange(const RangeSubmission& range, std::string& error);  // validate, then queue
    // Decide the next queued batch: highest end wins, ties by earliest arrival in this node's
    // queue; its tail [accepted frontier + 1, end] is accepted. Batches that end at or below
    // the new accepted frontier become stale and are dropped.
    std::optional<AcceptedRange> decideNextRange();

    // Transfers (D16). Submission checks form, signature, cap, fee minimum and that the nonce is
    // ahead of the sender's last nonce; balances and exact nonce order are checked at inclusion.
    bool submitTransfer(const Transfer& transfer, std::string& error);

    // Closing prime (D5) and block construction: transfers in submission order (invalid ones
    // are dropped), then rewards (D8), then the epoch distribution (D17).
    bool validatePrime(const PrimeSubmission& prime, std::string& error) const;
    std::optional<Block> closeBlock(const PrimeSubmission& prime, std::string& error);

    // B3.4: opening rings (ring-records.md section 2, R2, R3, R5, R6). Submission checks form and
    // signature (and, for a commit, that gate 1 is open now); state is checked at inclusion.
    // A reveal is accepted for the queue only if it matches an unexpired sealed commit, so
    // unmatched reveals cost nothing.
    bool submitRingCommit(const RingCommit& commit, std::string& error);
    bool submitRingPayee(const RingPayee& payee, std::string& error);
    bool submitRingReveal(const RingReveal& reveal, std::string& error);
    const RingRegistry& rings() const { return registry_; }
    const std::vector<RingOutcome>& lastRingOutcomes() const { return ring_outcomes_; }  // of the last block
    std::size_t pendingRingCount() const {
        return pending_ring_commits_.size() + pending_ring_payees_.size() + pending_ring_reveals_.size();
    }

    // B3.5: mining rings (ring-records.md section 3; rings-design.md R7-R10, R13; rule A4).
    // A RING_WORK is checked in full at submission (form, signature, size, segments per signature,
    // every row against the ring record) and queued; it stays queued until every segment in it is
    // recorded. Each segment competes on its own (D4): the proposer takes, at each ring's next
    // prime, the queued work that reaches furthest (ties by arrival) and accepts its segments from
    // there on. A ring batch is for the next main block h = height() + 1 (its rows count from h,
    // R13) and is applied before that block's prime; at most one per main block (proposed).
    struct RingBatchValue {
        RingBatch batch;
        std::vector<RingWork> works;  // the signed messages its groups name (journal data, D15)
    };
    bool submitRingWork(const RingWork& work, std::string& error);
    std::size_t pendingRingWorkCount() const { return pending_works_.size(); }
    std::optional<RingBatchValue> buildRingBatch() const;                     // the proposer
    bool checkRingBatch(const RingBatchValue& value, std::string& error) const;  // every validator, on a copy
    // The decided batch: the works must match the groups (hash, miner, segments), then every ring
    // block is checked and applied; with persistence it is appended to the rings file and its rows
    // are written to the row index before returning.
    bool applyDecidedRingBatch(const RingBatchValue& value, std::string& error);
    // Ring batches applied by this engine (since the loaded snapshot for an opened one).
    const std::vector<RingBatch>& ringBatches() const { return ring_batches_; }
    std::uint64_t lastRingBatchHeight() const { return last_ring_batch_; }
    struct StoreBytes { std::uint64_t blocks{0}, rings{0}, rows{0}; };
    StoreBytes storeBytes() const;  // file sizes, for measurement (zero without persistence)

    // Key registry: each key is stored once (in the block where it is first accepted).
    bool hasKey(const Address& address) const { return keys_.count(address) != 0; }

    // Rebuild from genesis and verify every block again; the result must match bit for bit.
    static std::optional<Engine> replay(const Policy& policy, const Genesis& genesis,
                                        const std::vector<Block>& blocks, std::string& error);
    static std::optional<Engine> replay(const Policy& policy, const std::vector<Block>& blocks, std::string& error) {
        return replay(policy, Genesis{}, blocks, error);
    }
    // B3.5: replay from the two files: before main block h, the ring batch with open height h.
    static std::optional<Engine> replay(const Policy& policy, const Genesis& genesis, const std::vector<Block>& blocks,
                                        const std::vector<RingBatch>& batches, std::string& error);

    // D19: open (or create) a persistent chain in `directory`: load the latest snapshot and
    // replay the blocks after it. Refuses to open on corruption or a policy/genesis mismatch.
    static std::optional<Engine> open(const std::string& directory, const Policy& policy, const Genesis& genesis,
                                      std::string& error);

private:
    struct Pending {
        std::uint64_t arrival;
        RangeSubmission range;
        Hash256 hash;
    };

    bool checkSigned(const Bytes& payload, const Address& provider, const Bytes& key, const Bytes& signature,
                     std::string& error) const;
    bool checkPrimeArithmetic(const Integer& prime, const Integer& witness, std::string& error) const;
    bool checkTransferForm(const Transfer& t, std::string& error) const;
    bool applyTransfer(const Transfer& t, std::string& error);  // state checks, then apply
    bool registerKey(const Address& address, const Bytes& key, std::string& error);
    std::optional<Block> closeAccepted(const AcceptedPrime& prime, const PrimeContents& contents, bool strict,
                                       std::string& error);
    // B3.4 (engine_rings.cpp), all for the block being sealed (height_ + 1).
    bool checkRingCommitForm(const RingCommit& c, std::string& error) const;
    bool applyRingCommit(const RingCommit& c, std::uint64_t index, std::string& error);
    bool checkRingPayeeForm(const RingPayee& p, std::string& error) const;
    bool applyRingPayee(const RingPayee& p, std::string& error);
    const OpenCommit* matchReveal(const RingReveal& v, std::uint64_t height, std::string& error) const;
    bool applyRingReveal(const RingReveal& v, std::string& error);  // false: unmatched (not includable)
    void expireCommits();
    void openRings();
    bool chainPrime(const Integer& n) const;  // the PrimeOracle for certificates (exact, R3)
    // B3.5 (engine_mining.cpp).
    struct PendingWork {
        std::uint64_t arrival{0};
        RingWork work;
        Hash256 hash{};
        std::uint64_t ring{0};
        std::vector<Integer> starts;  // of each segment
        std::vector<Integer> lasts;   // the last row of each segment
    };
    SegmentLimits segmentLimitsAt(std::uint64_t height) const;
    Integer nextChainPrime(const Integer& q) const;  // 2 for q = 0; 0 if not on chain yet
    std::vector<Integer> chainPrimesFrom(const Integer& start, std::size_t n) const;
    bool checkSegments(const RingEntry& ring, const std::vector<Segment>& segments, const Integer& start,
                       const SegmentLimits& limits, std::vector<Integer>& lasts, std::string& error) const;
    bool checkRingWork(const RingWork& work, PendingWork& out, std::string& error) const;
    bool applyRingBatch(const RingBatch& batch, bool append, std::string& error);  // verified; replay too
    bool checkGovernanceForm(const GovernanceTx& tx, std::string& error) const;
    bool applyGovernance(const GovernanceTx& tx, std::string& error);
    bool isScheduled(const Hash256& proposal_id) const;
    void activateScheduled(std::uint64_t height);  // changes effective at `height` take effect now
    Hash256 governanceDigest() const;
    Engine scratch() const;                          // detached copy for dry runs (no store)
    static AcceptedPrime acceptedFrom(const PrimeSubmission& prime);
    bool applyBlock(const Block& block, std::string& error);  // one verified replay step
    bool checkDivisors(const Integer& start, const Integer& end, const std::vector<Integer>& divisors,
                       std::string& error) const;
    bool distinctPrimeFactors(const Integer& n, std::vector<Integer>& out, std::string& error) const;
    void applyAccepted(const AcceptedRange& range);
    void distributePools();
    void refreshAssetHash(const Integer& asset);
    void refreshAsset(const AssetId& x);  // main or ring prime (B3.5)
    Hash256 validatorEpochDigest() const;
    Bytes encodeState() const;
    bool decodeState(const Bytes& data, std::string& error);
    Block sealBlock(const Integer& prime, const std::optional<AcceptedPrime>& accepted);

    Policy policy_;
    std::vector<ValidatorRecord> validators_;
    std::uint64_t height_{0};
    Integer chain_frontier_;
    Integer accepted_frontier_;
    Hash256 tip_{};
    std::vector<Block> blocks_;
    std::vector<AcceptedRange> open_ranges_;
    std::vector<Pending> pending_;
    std::vector<Transfer> pending_transfers_;
    std::vector<Transfer> included_transfers_;              // applied for the block being closed
    std::vector<PrimeSubmission> pending_primes_;
    std::vector<GovernanceTx> pending_governance_;
    std::vector<GovernanceTx> included_governance_;
    struct Tally {
        GovernanceProposal proposal;
        std::set<Hash256> voters;                             // sha3 of voters' consensus keys
    };
    std::map<Hash256, Tally> tallies_;                        // proposal id -> votes so far
    struct Ready {
        std::uint64_t height{0};  // where the "ready" message was included
        std::uint64_t expiry{0};    // the proposal's expiry and sequence: the entry is pruned once the
        std::uint64_t sequence{0};  // proposal can no longer pass and is not scheduled
    };
    bool readyDead(const Hash256& id, std::uint64_t expiry, std::uint64_t sequence, std::uint64_t height) const;
    std::map<Hash256, Ready> ready_;                          // proposal id -> ready message
    std::vector<std::pair<std::uint64_t, GovernanceProposal>> scheduled_;  // (effective height, proposal)
    ParameterTable params_;                                   // D20, with history
    std::uint64_t last_sequence_{0};
    Hash256 genesis_digest_{};
    std::uint64_t arrivals_{0};
    std::set<Integer> primes_;
    std::map<Integer, Integer> divisors_;                     // composite -> recorded divisor
    AssetLedger ledger_;                                      // balances with history, fee pool (B3.3)
    RingRegistry registry_;                                   // ring records and open commits (B3.4)
    std::vector<RingOutcome> ring_outcomes_;                  // of the block being sealed (not state)
    std::vector<RingCommit> pending_ring_commits_, included_ring_commits_;
    std::vector<RingPayee> pending_ring_payees_, included_ring_payees_;
    std::vector<RingReveal> pending_ring_reveals_, included_ring_reveals_;
    std::map<Integer, Hash256> asset_hashes_;
    std::map<AssetId, Hash256> ring_asset_hashes_;            // B3.5: ring primes moved by transfers
    std::map<Address, Bytes> keys_;                           // address -> public key (registered once)
    std::map<Address, std::uint64_t> nonces_;                 // sender -> last used nonce
    std::vector<KeyRecord> new_keys_;                         // registered since the last block
    std::vector<PendingWork> pending_works_;                  // B3.5: queued RING_WORK messages
    std::uint64_t arrivals_works_{0};
    std::vector<RingBatch> ring_batches_;                     // applied (since the snapshot when opened)
    std::uint64_t last_ring_batch_{0};                        // open height of the last ring batch (state)
    bool persist_rows_{false};                                // write rows to the store's row index
    std::shared_ptr<Store> store_;                            // set by open(); copies share it
};

}  // namespace primechain::next
