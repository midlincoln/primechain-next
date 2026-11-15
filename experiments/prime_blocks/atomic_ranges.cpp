#include "atomic_ranges.hpp"
#include "binary.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/storage/atomic_file.hpp"

namespace primechain::experiments {
namespace {
constexpr std::array<std::uint8_t, 8> kRangeMagic{'P', 'C', 'N', 'X', 'R', 'N', 'G', '2'};
constexpr std::array<std::uint8_t, 8> kJournalMagic{'P', 'C', 'N', 'X', 'R', 'J', '0', '2'};
constexpr std::size_t kMaxRangeBytes = 65536;

bool reject(std::string& error, const std::string& reason) { error = reason; return false; }

void blob(protocol::Bytes& out, const protocol::Bytes& value) {
    appendWord(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void address(protocol::Bytes& out, const Address& value) {
    if (value.size() > 128) throw std::runtime_error("address exceeds experiment limit");
    blob(out, protocol::Bytes(value.begin(), value.end()));
}

protocol::Bytes read(const std::string& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > kExperimentMaxBytes) throw std::runtime_error("file exceeds experiment limit");
    std::ifstream in(path, std::ios::binary);
    protocol::Bytes bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!in) throw std::runtime_error("cannot read experiment file");
    return bytes;
}
} // namespace

protocol::Bytes rangeSigningPayload(const CompositeRange& range) {
    if (range.factors.size() > kMaxRangeMembers) throw std::runtime_error("range member limit exceeded");
    protocol::Bytes bytes(kRangeMagic.begin(), kRangeMagic.end());
    bytes.insert(bytes.end(), range.previous_block.begin(), range.previous_block.end());
    bytes.insert(bytes.end(), range.previous_range.begin(), range.previous_range.end());
    appendWord(bytes, range.start);
    appendWord(bytes, range.end);
    appendWord(bytes, range.max_range_members);
    address(bytes, range.provider);
    appendWord(bytes, range.factors.size());
    for (const auto& factor : range.factors) {
        appendWord(bytes, factor.divisor);
        appendWord(bytes, factor.cofactor);
    }
    return bytes;
}

Hash256 rangeSubjectHash(const CompositeRange& range) {
    return crypto::sha3_256(rangeSigningPayload(range));
}

protocol::Bytes encodeRange(const CompositeRange& range) {
    if (range.votes.size() > 3 || range.public_key.size() > 4096 || range.signature.size() > 4096)
        throw std::runtime_error("range authentication limit exceeded");
    auto bytes = rangeSigningPayload(range);
    blob(bytes, range.public_key);
    blob(bytes, range.signature);
    appendWord(bytes, range.votes.size());
    for (const auto& vote : range.votes) {
        if (vote.public_key.size() > 4096 || vote.signature.size() > 4096)
            throw std::runtime_error("vote limit exceeded");
        address(bytes, vote.validator_address);
        blob(bytes, vote.public_key);
        bytes.insert(bytes.end(), vote.record_hash.begin(), vote.record_hash.end());
        appendWord(bytes, vote.round);
        blob(bytes, vote.signature);
    }
    if (bytes.size() > kMaxRangeBytes) throw std::runtime_error("range byte limit exceeded");
    return bytes;
}

bool decodeRange(const protocol::Bytes& bytes, CompositeRange& range, std::string& error) {
    error.clear();
    try {
        if (bytes.size() > kMaxRangeBytes) throw std::runtime_error("range byte limit exceeded");
        detail::Reader reader(bytes);
        reader.magic(kRangeMagic);
        CompositeRange result;
        result.previous_block = reader.hash();
        result.previous_range = reader.hash();
        result.start = reader.integer();
        result.end = reader.integer();
        result.max_range_members = reader.integer();
        result.provider = reader.address();
        const auto count = reader.count(kMaxRangeMembers);
        for (std::size_t i = 0; i < count; ++i) {
            const auto divisor = reader.integer();
            const auto cofactor = reader.integer();
            result.factors.push_back({divisor, cofactor});
        }
        result.public_key = reader.blob(4096);
        result.signature = reader.blob(4096);
        const auto votes = reader.count(3);
        for (std::size_t i = 0; i < votes; ++i) {
            protocol::ValidatorVoteV0 vote;
            vote.validator_address = reader.address();
            vote.public_key = reader.blob(4096);
            vote.record_hash = reader.hash();
            vote.round = reader.integer();
            vote.signature = reader.blob(4096);
            result.votes.push_back(std::move(vote));
        }
        reader.finish();
        range = std::move(result);
        return true;
    } catch (const std::exception& exception) { return reject(error, exception.what()); }
}

RangeJournal::RangeJournal(std::string path, const PrimeBlockReplay& finalized,
                           std::vector<Address> validators, Address local_validator, RangePolicy policy)
    : path_(std::move(path)), finalized_(finalized), validators_(std::move(validators)),
      local_validator_(std::move(local_validator)), policy_(policy) {
    std::sort(validators_.begin(), validators_.end());
}

RangeJournal::~RangeJournal() { if (writer_fd_ >= 0) ::close(writer_fd_); }

Hash256 RangeJournal::configurationHash() const {
    protocol::Bytes bytes(kJournalMagic.begin(), kJournalMagic.end());
    bytes.insert(bytes.end(), finalized_.tip().begin(), finalized_.tip().end());
    appendWord(bytes, finalized_.frontier());
    appendWord(bytes, policy_.max_members);
    for (const auto& validator : validators_) address(bytes, validator);
    address(bytes, local_validator_);
    return crypto::sha3_256(bytes);
}

bool RangeJournal::validate(const CompositeRange& range, bool certificate, std::string& error) const {
    if (range.max_range_members != policy_.max_members || range.factors.size() > policy_.max_members)
        return reject(error, "range exceeds or disagrees with active range policy");
    if (range.start < 2 || range.end < range.start || range.end > kExperimentMaxInteger ||
        range.factors.empty() || range.factors.size() > kMaxRangeMembers ||
        range.factors.size() != range.end - range.start + 1)
        return reject(error, "range bounds or member count invalid");
    if (range.previous_block != finalized_.tip()) return reject(error, "wrong finalized block anchor");
    if (range.provider.size() > 128 || !crypto::isProtocolSignatureAddress(range.provider) ||
        range.public_key.size() != crypto::signaturePublicKeySize(crypto::kProtocolSignatureAlgorithm) ||
        range.signature.size() != crypto::signatureSize(crypto::kProtocolSignatureAlgorithm) ||
        crypto::addressFromProtocolPublicKey(range.public_key) != range.provider)
        return reject(error, "invalid range provider authentication");
    for (std::size_t i = 0; i < range.factors.size(); ++i) {
        CompositeProof proof;
        proof.m = range.start + i;
        proof.d = range.factors[i].divisor;
        proof.e = range.factors[i].cofactor;
        if (!math::verifyCompositeProof(proof)) return reject(error, "invalid composite member; entire range rejected");
    }
    if (!crypto::verifyProtocolMessageSignature(range.public_key, rangeSigningPayload(range), range.signature, error)) return false;
    if (certificate) {
        protocol::FinalizationProofV0 proof;
        proof.rule = "fixed-2-of-3-mldsa65-v2";
        proof.votes = range.votes;
        if (!protocol::verifyRecordFinalization(proof, rangeSubjectHash(range), range.previous_block,
                range.start, validators_, error)) return false;
    }
    return true;
}

bool RangeJournal::next(const CompositeRange& range, const std::vector<CompositeRange>& prefix,
                         std::string& error) const {
    const auto frontier = prefix.empty() ? finalized_.frontier() : prefix.back().end;
    const auto tip = prefix.empty() ? Hash256{} : rangeSubjectHash(prefix.back());
    if (range.start != frontier + 1 || range.previous_range != tip)
        return reject(error, "range is not the next contiguous interval");
    if (prefix.size() >= kMaxJournalRanges || range.end - finalized_.frontier() > kMaxOpenComposites)
        return reject(error, "open interval experiment limit exceeded");
    return true;
}

protocol::Bytes RangeJournal::stateBytes(const std::vector<CompositeRange>& ranges,
                                        const std::optional<CompositeRange>& locked,
                                        const std::optional<PrimeBlock>& closing) const {
    protocol::Bytes bytes(kJournalMagic.begin(), kJournalMagic.end());
    const auto configuration = configurationHash();
    bytes.insert(bytes.end(), configuration.begin(), configuration.end());
    appendWord(bytes, ranges.size());
    for (const auto& range : ranges) blob(bytes, encodeRange(range));
    appendWord(bytes, locked ? 1 : 0);
    if (locked) blob(bytes, encodeRange(*locked));
    appendWord(bytes, closing ? 1 : 0);
    if (closing) blob(bytes, encodeBlock(*closing));
    const auto checksum = crypto::sha3_256(bytes);
    bytes.insert(bytes.end(), checksum.begin(), checksum.end());
    if (bytes.size() > kExperimentMaxBytes) throw std::runtime_error("journal byte limit exceeded");
    return bytes;
}

bool RangeJournal::write(const std::string& path, const protocol::Bytes& bytes, std::string& error) {
    const auto temporary = storage::detail::uniqueAtomicTempPath(path);
    const auto fault = failure_;
    failure_ = TestWriteFailure::None;
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out.close();
        if (!out) throw std::runtime_error("cannot write temporary range state");
        if (fault == TestWriteFailure::BeforePublish) throw std::runtime_error("injected failure before publication");
        if (!storage::detail::commitAtomicTemp(temporary, path, "range experiment state", error))
            throw std::runtime_error(error);
        if (fault == TestWriteFailure::AfterPublish) throw std::runtime_error("injected acknowledgement loss after publication");
        return true;
    } catch (const std::exception& exception) {
        // A failed write may have renamed already. Never vote/accept again until reloaded.
        ready_ = false;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return reject(error, exception.what());
    }
}

bool RangeJournal::open(bool create, std::string& error) {
    error.clear();
    if (writer_fd_ >= 0) return reject(error, "journal already opened; recreate object to recover");
    if (policy_.max_members == 0 || policy_.max_members > kMaxRangeMembers)
        return reject(error, "range policy outside experimental safety ceiling");
    if (validators_ != finalized_.validatorSet() || validators_.size() != 3 ||
        std::adjacent_find(validators_.begin(), validators_.end()) != validators_.end() ||
        !std::all_of(validators_.begin(), validators_.end(), crypto::isProtocolSignatureAddress) ||
        !std::binary_search(validators_.begin(), validators_.end(), local_validator_) || finalized_.frontier() < 2)
        return reject(error, "invalid fixture configuration");
    writer_fd_ = ::open((path_ + ".writer-lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (writer_fd_ < 0) return reject(error, "cannot open journal writer lock");
    if (flock(writer_fd_, LOCK_EX | LOCK_NB) != 0) return reject(error, "another writer owns this journal");
    try {
        if (create) {
            if (std::filesystem::exists(path_) || std::filesystem::exists(path_ + ".block"))
                return reject(error, "refusing to initialize existing journal or checkpoint");
            if (!write(path_, stateBytes({}, std::nullopt, std::nullopt), error)) return false;
        }
        auto bytes = read(path_);
        if (bytes.size() < 32) return reject(error, "truncated journal checksum");
        Hash256 checksum{};
        std::copy(bytes.end() - 32, bytes.end(), checksum.begin());
        bytes.resize(bytes.size() - 32);
        if (crypto::sha3_256(bytes) != checksum) return reject(error, "journal checksum mismatch; recovery required");
        detail::Reader reader(bytes);
        reader.magic(kJournalMagic);
        if (reader.hash() != configurationHash()) return reject(error, "journal anchor, identity, or validator set mismatch");
        const auto count = reader.count(kMaxJournalRanges);
        std::vector<CompositeRange> ranges;
        for (std::size_t i = 0; i < count; ++i) {
            CompositeRange range;
            if (!decodeRange(reader.blob(kMaxRangeBytes), range, error) ||
                !validate(range, true, error) || !next(range, ranges, error)) return false;
            ranges.push_back(std::move(range));
        }
        std::optional<CompositeRange> locked;
        if (reader.count(1)) {
            CompositeRange proposal;
            if (!decodeRange(reader.blob(kMaxRangeBytes), proposal, error) || !proposal.votes.empty() ||
                !validate(proposal, false, error) || !next(proposal, ranges, error))
                return reject(error, "invalid persisted vote lock: " + error);
            locked = std::move(proposal);
        }
        std::optional<PrimeBlock> closing;
        if (reader.count(1)) {
            PrimeBlock proposal;
            if (locked || !decodeBlock(reader.blob(kExperimentMaxBytes), proposal, error) || !proposal.votes.empty())
                return reject(error, "invalid persisted closing vote lock: " + error);
            closing = std::move(proposal);
        }
        reader.finish();
        accepted_ = std::move(ranges);
        locked_ = std::move(locked);
        if (closing && !validateClosure(*closing, false, error)) return false;
        closing_ = std::move(closing);
        if (std::filesystem::exists(path_ + ".block")) {
            PrimeBlock block;
            if (!decodeBlock(read(path_ + ".block"), block, error) || !validateClosure(block, true, error)) return false;
            closed_ = true;
        }
        ready_ = true;
        return true;
    } catch (const std::exception& exception) { return reject(error, exception.what()); }
}

std::optional<protocol::ValidatorVoteV0> RangeJournal::voteFor(
    const CompositeRange& proposal, const crypto::SignatureKeyPair& identity, std::string& error) {
    error.clear();
    try {
        if (!ready_ || closed_ || closing_) { reject(error, "journal unavailable, closing, or closed"); return std::nullopt; }
        if (crypto::addressFromProtocolPublicKey(identity.public_key) != local_validator_) {
            reject(error, "wrong local validator key"); return std::nullopt;
        }
        if (!validate(proposal, false, error) || !next(proposal, accepted_, error)) return std::nullopt;
        const auto subject = rangeSubjectHash(proposal);
        if (locked_ && rangeSubjectHash(*locked_) != subject) {
            reject(error, "conflicting range already locked at this position"); return std::nullopt;
        }
        if (!locked_) {
            auto locked = proposal;
            locked.votes.clear();
            if (!write(path_, stateBytes(accepted_, locked, std::nullopt), error)) return std::nullopt;
            locked_ = std::move(locked);
        }
        auto vote = protocol::makeSignedValidatorVote(local_validator_, identity.public_key, identity.private_key,
            subject, 1, error);
        if (vote.signature.empty()) return std::nullopt;
        return vote;
    } catch (const std::exception& exception) { reject(error, exception.what()); return std::nullopt; }
}

RangeAcceptance RangeJournal::accept(const CompositeRange& range, std::string& error) {
    error.clear();
    try {
        if (!ready_) { reject(error, "journal unavailable; reload required"); return RangeAcceptance::RecoveryRequired; }
        if (!validate(range, true, error)) return RangeAcceptance::Rejected;
        const auto subject = rangeSubjectHash(range);
        for (const auto& previous : accepted_)
            if (rangeSubjectHash(previous) == subject) return RangeAcceptance::AlreadyAccepted;
        if (closed_ || closing_) { reject(error, "block closing or closed"); return RangeAcceptance::Rejected; }
        if (!next(range, accepted_, error)) return RangeAcceptance::Rejected;
        auto updated = accepted_;
        updated.push_back(range);
        // A quorum certificate resolves a minority local vote without signing a conflicting vote.
        if (!write(path_, stateBytes(updated, std::nullopt, std::nullopt), error)) return RangeAcceptance::RecoveryRequired;
        accepted_ = std::move(updated);
        locked_.reset();
        return RangeAcceptance::Accepted;
    } catch (const std::exception& exception) { reject(error, exception.what()); return RangeAcceptance::Rejected; }
}

PrimeValue RangeJournal::acceptedFrontier() const {
    return accepted_.empty() ? finalized_.frontier() : accepted_.back().end;
}

Hash256 RangeJournal::acceptedRangeTip() const {
    return accepted_.empty() ? Hash256{} : rangeSubjectHash(accepted_.back());
}

std::vector<CompositeProof> RangeJournal::composites() const {
    std::vector<CompositeProof> proofs;
    for (const auto& range : accepted_) {
        for (std::size_t i = 0; i < range.factors.size(); ++i) {
            CompositeProof proof;
            proof.m = range.start + i;
            proof.d = range.factors[i].divisor;
            proof.e = range.factors[i].cofactor;
            proof.provider_address = range.provider;
            proofs.push_back(std::move(proof));
        }
    }
    return proofs;
}

std::optional<protocol::ValidatorVoteV0> RangeJournal::voteForBlock(
    const PrimeBlock& proposal, const crypto::SignatureKeyPair& identity, std::string& error) {
    error.clear();
    try {
        if (!ready_ || closed_ || locked_) {
            reject(error, "journal unavailable, closed, or range vote outstanding"); return std::nullopt;
        }
        if (crypto::addressFromProtocolPublicKey(identity.public_key) != local_validator_) {
            reject(error, "wrong local validator key"); return std::nullopt;
        }
        if (!validateClosure(proposal, false, error)) return std::nullopt;
        const auto subject = blockSubjectHash(proposal);
        if (closing_ && blockSubjectHash(*closing_) != subject) {
            reject(error, "conflicting closing block already locked"); return std::nullopt;
        }
        if (!closing_) {
            auto closing = proposal;
            closing.votes.clear();
            if (!write(path_, stateBytes(accepted_, std::nullopt, closing), error)) return std::nullopt;
            closing_ = std::move(closing);
        }
        auto vote = protocol::makeSignedValidatorVote(local_validator_, identity.public_key, identity.private_key,
            subject, 1, error);
        if (vote.signature.empty()) return std::nullopt;
        return vote;
    } catch (const std::exception& exception) { reject(error, exception.what()); return std::nullopt; }
}

bool RangeJournal::validateClosure(const PrimeBlock& block, bool certificate, std::string& error) const {
    const auto expected = composites();
    if (block.prime.p != acceptedFrontier() + 1 || block.composites.size() != expected.size())
        return reject(error, "block does not close the accepted interval");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& got = block.composites[i];
        const auto& want = expected[i];
        if (got.m != want.m || got.d != want.d || got.e != want.e || got.provider_address != want.provider_address)
            return reject(error, "block differs from accepted range evidence or attribution");
    }
    if (!certificate) return finalized_.validateProposal(block, error);
    auto replay = finalized_;
    return replay.append(block, error);
}

bool RangeJournal::seal(const PrimeBlock& block, std::string& error) {
    error.clear();
    try {
        if (!ready_) return reject(error, "journal unavailable; reload required");
        if (!validateClosure(block, true, error)) return false;
        if (closed_) {
            PrimeBlock existing;
            if (!decodeBlock(read(path_ + ".block"), existing, error)) return false;
            return blockSubjectHash(existing) == blockSubjectHash(block) || reject(error, "different block already sealed");
        }
        if (!write(path_ + ".block", encodeBlock(block), error)) return false;
        closed_ = true;
        return true;
    } catch (const std::exception& exception) { return reject(error, exception.what()); }
}

} // namespace primechain::experiments
