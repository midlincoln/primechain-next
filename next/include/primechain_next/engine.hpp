#pragma once
// In-memory main-chain engine (A2.1 + A2.2). One authoritative state, updated incrementally;
// nothing is reloaded per request (see docs/v1-baseline-measurements.md). Optional persistence
// (D19) appends every sealed block to a Store and snapshots the state every K blocks.
#include "primechain_next/model.hpp"
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
    std::uint64_t reward_micro_units{1000000};  // D8: reward(p) returns this for every p at launch
    std::uint64_t min_fee_micro_units{1};       // D16 (v1: 1 micro-unit)
    std::uint64_t max_outputs{1000};            // D16 bulk-send cap
    std::uint64_t epoch_length{100};            // D17: pools are distributed every N blocks
    std::uint64_t snapshot_interval{1000};      // D19: state snapshot every K blocks
    Hash256 digest() const;
};

// D18: the genesis validator set (a trusted configuration, like v1's validators.txt).
struct Genesis {
    std::vector<ValidatorRecord> validators;
};

// D8 reward function. A later schedule changes only this function.
std::uint64_t reward(const Policy& policy, const Integer& prime);

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
    Amount poolBalance(const Integer& asset) const;     // validator reward pool (D8)
    Amount feePoolBalance(const Integer& asset) const;  // validator fee pool (D16)
    std::uint64_t nonce(const Address& address) const; // last used nonce (0 = none yet)
    const std::vector<ValidatorRecord>& validators() const { return validators_; }
    Hash256 stateRoot() const;

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

    // Key registry: each key is stored once (in the block where it is first accepted).
    bool hasKey(const Address& address) const { return keys_.count(address) != 0; }

    // Rebuild from genesis and verify every block again; the result must match bit for bit.
    static std::optional<Engine> replay(const Policy& policy, const Genesis& genesis,
                                        const std::vector<Block>& blocks, std::string& error);
    static std::optional<Engine> replay(const Policy& policy, const std::vector<Block>& blocks, std::string& error) {
        return replay(policy, Genesis{}, blocks, error);
    }

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
    std::optional<Block> closeAccepted(const AcceptedPrime& prime, const std::vector<Transfer>& transfers, bool strict,
                                       std::string& error);
    bool applyBlock(const Block& block, std::string& error);  // one verified replay step
    bool checkDivisors(const Integer& start, const Integer& end, const std::vector<Integer>& divisors,
                       std::string& error) const;
    bool distinctPrimeFactors(const Integer& n, std::vector<Integer>& out, std::string& error) const;
    void applyAccepted(const AcceptedRange& range);
    void credit(std::map<Address, Amount>& holders, const Address& address, const Amount& amount);
    void creditPool(std::map<Integer, Amount>& pool, const Integer& asset, const Amount& amount);
    void distributePools();
    void refreshAssetHash(const Integer& asset);
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
    std::uint64_t arrivals_{0};
    std::set<Integer> primes_;
    std::map<Integer, Integer> divisors_;                     // composite -> recorded divisor
    std::map<Integer, std::map<Address, Amount>> balances_;   // asset (prime) -> address -> amount
    std::map<Integer, Amount> pool_;                          // asset -> validator reward pool
    std::map<Integer, Amount> fee_pool_;                      // asset -> validator fee pool
    std::map<Integer, Hash256> asset_hashes_;
    std::map<Address, Bytes> keys_;                           // address -> public key (registered once)
    std::map<Address, std::uint64_t> nonces_;                 // sender -> last used nonce
    std::vector<KeyRecord> new_keys_;                         // registered since the last block
    std::shared_ptr<Store> store_;                            // set by open(); copies share it
};

}  // namespace primechain::next
