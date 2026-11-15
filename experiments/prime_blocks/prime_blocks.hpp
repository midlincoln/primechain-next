#pragma once

#include <map>
#include <string>
#include <vector>

#include "primechain/math/number_theory.hpp"
#include "primechain/protocol/records.hpp"

namespace primechain::experiments {

// Offline format only. Never accepted by the node or the network protocol.
constexpr PrimeValue kExperimentMaxInteger = 100000;
constexpr std::size_t kExperimentMaxBytes = 16 * 1024 * 1024;

struct PrimeBlock {
    std::uint64_t height{0};
    Hash256 previous_hash{};
    PrimeValue previous_prime{0};
    protocol::PrattPrimeProofV0 prime;
    std::vector<CompositeProof> composites;
    std::vector<protocol::ValidatorVoteV0> votes;
};

protocol::Bytes encodeBlock(const PrimeBlock& block, bool with_votes = true);
void appendWord(protocol::Bytes& bytes, std::uint64_t value);
bool decodeBlock(const protocol::Bytes& bytes, PrimeBlock& block, std::string& error);
Hash256 blockSubjectHash(const PrimeBlock& block);

class PrimeBlockReplay final : public math::CompositeProofIndex {
public:
    explicit PrimeBlockReplay(std::vector<Address> validators);
    bool validateProposal(const PrimeBlock& block, std::string& error) const;
    bool append(const PrimeBlock& block, std::string& error);
    std::optional<CompositeProof> findCompositeProof(PrimeValue n) const override;
    std::uint64_t balance(const Address& address, PrimeValue prime) const;
    std::uint64_t supply(PrimeValue prime) const;
    PrimeValue frontier() const { return frontier_; }
    std::uint64_t blockCount() const { return block_count_; }
    const Hash256& tip() const { return tip_; }
    const std::vector<Address>& validatorSet() const { return validators_; }

private:
    std::vector<Address> validators_;
    std::map<PrimeValue, CompositeProof> proofs_;
    std::map<std::pair<Address, PrimeValue>, std::uint64_t> balances_;
    std::map<PrimeValue, std::uint64_t> supplies_;
    PrimeValue frontier_{0};
    std::uint64_t block_count_{0};
    Hash256 tip_{};
};

// Current-interval evidence must be visible before its closing prime is finalized.
class OpenProofIndex final : public math::CompositeProofIndex {
public:
    OpenProofIndex(const math::CompositeProofIndex& history,
                   const std::vector<CompositeProof>& pending)
        : history_(history), pending_(pending) {}
    std::optional<CompositeProof> findCompositeProof(PrimeValue n) const override;

private:
    const math::CompositeProofIndex& history_;
    const std::vector<CompositeProof>& pending_;
};

} // namespace primechain::experiments
