#pragma once

#include "prime_blocks.hpp"
#include "primechain/crypto/signature.hpp"

namespace primechain::experiments {

// Parser/resource ceiling, distinct from the consensus policy chosen for an interval.
constexpr std::size_t kMaxRangeMembers = 256;
constexpr std::size_t kMaxJournalRanges = 256;
constexpr std::size_t kMaxOpenComposites = 4096;

struct RangePolicy {
    std::uint64_t max_members{256};
};

struct RangeFactor {
    PrimeValue divisor{0};
    PrimeValue cofactor{0};
};

struct CompositeRange {
    Hash256 previous_block{};
    Hash256 previous_range{};
    PrimeValue start{0};
    PrimeValue end{0};
    std::uint64_t max_range_members{RangePolicy{}.max_members};
    Address provider;
    std::vector<RangeFactor> factors;
    protocol::Bytes public_key;
    protocol::Bytes signature;
    std::vector<protocol::ValidatorVoteV0> votes;
};

protocol::Bytes rangeSigningPayload(const CompositeRange& range);
Hash256 rangeSubjectHash(const CompositeRange& range);
protocol::Bytes encodeRange(const CompositeRange& range);
bool decodeRange(const protocol::Bytes& bytes, CompositeRange& range, std::string& error);

enum class RangeAcceptance { Rejected, Accepted, AlreadyAccepted, RecoveryRequired };
enum class TestWriteFailure { None, BeforePublish, AfterPublish };

// Single-threaded, single-writer, fixed-validator offline journal. No network endpoints or pruning.
class RangeJournal {
public:
    RangeJournal(std::string path, const PrimeBlockReplay& finalized,
                 std::vector<Address> validators, Address local_validator, RangePolicy policy = {});
    ~RangeJournal();
    RangeJournal(const RangeJournal&) = delete;
    RangeJournal& operator=(const RangeJournal&) = delete;

    bool open(bool create, std::string& error);
    std::optional<protocol::ValidatorVoteV0> voteFor(
        const CompositeRange& proposal, const crypto::SignatureKeyPair& identity, std::string& error);
    RangeAcceptance accept(const CompositeRange& range, std::string& error);
    std::optional<protocol::ValidatorVoteV0> voteForBlock(
        const PrimeBlock& proposal, const crypto::SignatureKeyPair& identity, std::string& error);
    bool seal(const PrimeBlock& block, std::string& error);
    std::vector<CompositeProof> composites() const;
    PrimeValue acceptedFrontier() const;
    Hash256 acceptedRangeTip() const;
    std::size_t rangeCount() const { return accepted_.size(); }
    bool closed() const { return closed_; }
    bool ready() const { return ready_; }
    const RangePolicy& policy() const { return policy_; }
    std::optional<CompositeRange> pendingRange() const { return locked_; }
    std::optional<PrimeBlock> pendingBlock() const { return closing_; }
    std::optional<CompositeRange> receipt(const Hash256& subject) const;
    void failNextWriteForTest(TestWriteFailure point) { failure_ = point; }

private:
    bool validate(const CompositeRange& range, bool certificate, std::string& error) const;
    bool next(const CompositeRange& range, const std::vector<CompositeRange>& prefix, std::string& error) const;
    bool validateClosure(const PrimeBlock& block, bool certificate, std::string& error) const;
    protocol::Bytes stateBytes(const std::vector<CompositeRange>& ranges,
                               const std::optional<CompositeRange>& locked,
                               const std::optional<PrimeBlock>& closing) const;
    bool write(const std::string& path, const protocol::Bytes& bytes, std::string& error);
    Hash256 configurationHash() const;

    std::string path_;
    PrimeBlockReplay finalized_;
    std::vector<Address> validators_;
    Address local_validator_;
    RangePolicy policy_;
    std::vector<CompositeRange> accepted_;
    std::optional<CompositeRange> locked_;
    std::optional<PrimeBlock> closing_;
    int writer_fd_{-1};
    bool ready_{false};
    bool closed_{false};
    TestWriteFailure failure_{TestWriteFailure::None};
};

} // namespace primechain::experiments
