#pragma once

#include <memory>
#include "atomic_ranges.hpp"

namespace primechain::experiments {

enum class BlockInstallation { Rejected, Installed, AlreadyInstalled, RecoveryRequired };
enum class TransitionFailure {
    None, AfterSeal, BeforeBlockPublish, AfterBlockPublish,
    BeforeCheckpoint, AfterCheckpoint, BeforeJournalOpen, AfterJournalOpen,
    BeforeActivation, AfterActivation
};

// Bounded, single-threaded experiment. Retains every interval journal; no pruning or peer sync.
class ContinuousChain {
public:
    ContinuousChain(std::string directory, PrimeBlock genesis, std::vector<Address> validators,
                    Address local_validator, RangePolicy policy = {});
    ~ContinuousChain();
    ContinuousChain(const ContinuousChain&) = delete;
    ContinuousChain& operator=(const ContinuousChain&) = delete;

    bool open(bool create, std::string& error);
    std::optional<protocol::ValidatorVoteV0> voteForRange(
        const CompositeRange& proposal, const crypto::SignatureKeyPair& identity, std::string& error);
    RangeAcceptance accept(const CompositeRange& range, std::string& error);
    std::optional<protocol::ValidatorVoteV0> voteForBlock(
        const PrimeBlock& proposal, const crypto::SignatureKeyPair& identity, std::string& error);
    BlockInstallation install(const PrimeBlock& block, std::string& error);

    const PrimeBlockReplay& history() const { return history_; }
    const RangePolicy& policy() const { return policy_; }
    bool ready() const { return ready_; }
    PrimeValue acceptedFrontier() const;
    Hash256 acceptedRangeTip() const;
    std::vector<CompositeProof> composites() const;
    std::optional<CompositeRange> pendingRange() const;
    std::optional<PrimeBlock> pendingBlock() const;
    std::optional<CompositeRange> rangeReceipt(const Hash256& anchor, const Hash256& subject) const;
    std::optional<PrimeBlock> installedBlock(std::uint64_t height) const;
    void failNextTransitionForTest(TransitionFailure point) { failure_ = point; }

private:
    std::string blockPath(std::uint64_t height) const;
    std::string intervalPath() const;
    Hash256 configurationHash() const;
    void checkpoint(const PrimeBlockReplay& history, bool active);
    void openInterval(bool active);
    void advance(const PrimeBlock& block);
    void failAt(TransitionFailure point);
    bool available(std::string& error) const;

    std::string directory_;
    PrimeBlock genesis_;
    PrimeBlockReplay history_;
    Address local_validator_;
    RangePolicy policy_;
    std::unique_ptr<RangeJournal> journal_;
    int writer_fd_{-1};
    bool attempted_open_{false};
    bool ready_{false};
    TransitionFailure failure_{TransitionFailure::None};
};

} // namespace primechain::experiments
