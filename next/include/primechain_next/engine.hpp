#pragma once
// In-memory main-chain engine (A2.1). One authoritative state, updated incrementally;
// nothing is reloaded per request (see docs/v1-baseline-measurements.md).
#include "primechain_next/model.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace primechain::next {

struct Policy {
    std::string network_id{"primechain-next-dev"};
    Limits limits;
    std::uint64_t reward_micro_units{1000000};  // D8: reward(p) returns this for every p at launch
    Hash256 digest() const;
};

// D8 reward function. A later schedule changes only this function.
std::uint64_t reward(const Policy& policy, const Integer& prime);

class Engine {
public:
    explicit Engine(Policy policy = {});

    const Policy& policy() const { return policy_; }
    std::uint64_t height() const { return height_; }
    const Integer& chainFrontier() const { return chain_frontier_; }
    const Integer& acceptedFrontier() const { return accepted_frontier_; }
    const Hash256& tip() const { return tip_; }
    const std::vector<Block>& blocks() const { return blocks_; }
    std::size_t pendingCount() const { return pending_.size(); }
    Amount balance(const Address& address, const Integer& asset) const;
    Amount poolBalance(const Integer& asset) const;
    Hash256 stateRoot() const;

    // Ranges (D1, D2, D4).
    bool validateRange(const RangeSubmission& range, std::string& error) const;
    bool submitRange(const RangeSubmission& range, std::string& error);  // validate, then queue
    // Decide the next queued batch: highest end wins, ties by earliest arrival; its tail
    // [accepted frontier + 1, end] is accepted. Batches that end at or below the new
    // accepted frontier become stale and are dropped.
    std::optional<AcceptedRange> decideNextRange();

    // Closing prime (D5) and block construction with rewards (D8).
    bool validatePrime(const PrimeSubmission& prime, std::string& error) const;
    std::optional<Block> closeBlock(const PrimeSubmission& prime, std::string& error);

    // Rebuild from genesis and verify every block again; the result must match bit for bit.
    static std::optional<Engine> replay(const Policy& policy, const std::vector<Block>& blocks, std::string& error);

private:
    struct Pending {
        std::uint64_t arrival;
        RangeSubmission range;
        Hash256 hash;
    };

    bool checkSigned(const Bytes& payload, const Address& provider, const Bytes& key, const Bytes& signature,
                     std::string& error) const;
    bool checkDivisors(const Integer& start, const Integer& end, const std::vector<Integer>& divisors,
                       std::string& error) const;
    bool distinctPrimeFactors(const Integer& n, std::vector<Integer>& out, std::string& error) const;
    void applyAccepted(const AcceptedRange& range);
    void credit(const Address* address, const Integer& asset, const Amount& amount);
    void refreshAssetHash(const Integer& asset);
    Block sealBlock(const Integer& prime, const std::optional<PrimeSubmission>& submission);

    Policy policy_;
    std::uint64_t height_{0};
    Integer chain_frontier_;
    Integer accepted_frontier_;
    Hash256 tip_{};
    std::vector<Block> blocks_;
    std::vector<AcceptedRange> open_ranges_;
    std::vector<Pending> pending_;
    std::uint64_t arrivals_{0};
    std::set<Integer> primes_;
    std::map<Integer, Integer> divisors_;                     // composite -> recorded divisor
    std::map<Integer, std::map<Address, Amount>> balances_;   // asset (prime) -> address -> amount
    std::map<Integer, Amount> pool_;                          // asset -> validator pool amount
    std::map<Integer, Hash256> asset_hashes_;
};

}  // namespace primechain::next
