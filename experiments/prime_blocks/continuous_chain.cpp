#include "continuous_chain.hpp"
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
namespace fs = std::filesystem;
constexpr std::array<std::uint8_t, 8> kChainMagic{'P', 'C', 'N', 'X', 'C', 'H', '0', '1'};
constexpr std::size_t kMaxBlocks = 10000;

protocol::Bytes read(const std::string& path) {
    const auto size = fs::file_size(path);
    if (size > kExperimentMaxBytes) throw std::runtime_error("chain file exceeds experiment limit");
    std::ifstream in(path, std::ios::binary);
    protocol::Bytes bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!in) throw std::runtime_error("cannot read chain file");
    return bytes;
}

PrimeBlock readBlock(const std::string& path) {
    PrimeBlock block;
    std::string error;
    if (!decodeBlock(read(path), block, error)) throw std::runtime_error(error);
    return block;
}

void publish(const std::string& path, const protocol::Bytes& bytes) {
    if (bytes.size() > kExperimentMaxBytes) throw std::runtime_error("chain file exceeds experiment limit");
    const auto temp = storage::detail::uniqueAtomicTempPath(path);
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out.close();
        if (!out) throw std::runtime_error("cannot write chain temporary file");
        std::string error;
        if (!storage::detail::commitAtomicTemp(temp, path, "experimental chain", error))
            throw std::runtime_error(error);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}

void syncDirectory(const fs::path& path) {
    const auto parent = path.empty() ? fs::path(".") : path;
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open chain parent directory for sync");
    const bool ok = fsync(fd) == 0;
    ::close(fd);
    if (!ok) throw std::runtime_error("cannot sync chain parent directory");
}
} // namespace

ContinuousChain::ContinuousChain(std::string directory, PrimeBlock genesis, std::vector<Address> validators,
                                 Address local_validator, RangePolicy policy)
    : directory_(std::move(directory)), genesis_(std::move(genesis)), history_(std::move(validators)),
      local_validator_(std::move(local_validator)), policy_(policy) {}

ContinuousChain::~ContinuousChain() {
    journal_.reset();
    if (writer_fd_ >= 0) ::close(writer_fd_);
}

std::string ContinuousChain::blockPath(std::uint64_t height) const {
    return (fs::path(directory_) / ("block-" + std::to_string(height) + ".dat")).string();
}

std::string ContinuousChain::intervalPath() const {
    return (fs::path(directory_) / ("interval-" + std::to_string(history_.blockCount()) + ".journal")).string();
}

Hash256 ContinuousChain::configurationHash() const {
    protocol::Bytes bytes(kChainMagic.begin(), kChainMagic.end());
    const auto genesis = blockSubjectHash(genesis_);
    bytes.insert(bytes.end(), genesis.begin(), genesis.end());
    appendWord(bytes, policy_.max_members);
    for (const auto& address : history_.validatorSet()) {
        appendWord(bytes, address.size());
        bytes.insert(bytes.end(), address.begin(), address.end());
    }
    appendWord(bytes, local_validator_.size());
    bytes.insert(bytes.end(), local_validator_.begin(), local_validator_.end());
    return crypto::sha3_256(bytes);
}

void ContinuousChain::checkpoint(const PrimeBlockReplay& history, bool active) {
    protocol::Bytes bytes(kChainMagic.begin(), kChainMagic.end());
    const auto configuration = configurationHash();
    bytes.insert(bytes.end(), configuration.begin(), configuration.end());
    appendWord(bytes, history.blockCount());
    bytes.insert(bytes.end(), history.tip().begin(), history.tip().end());
    appendWord(bytes, active);
    const auto checksum = crypto::sha3_256(bytes);
    bytes.insert(bytes.end(), checksum.begin(), checksum.end());
    publish((fs::path(directory_) / "checkpoint.dat").string(), bytes);
}

void ContinuousChain::failAt(TransitionFailure point) {
    if (failure_ != point) return;
    failure_ = TransitionFailure::None;
    throw std::runtime_error("injected interruption at chain transition " + std::to_string(static_cast<int>(point)));
}

void ContinuousChain::openInterval(bool active) {
    failAt(TransitionFailure::BeforeJournalOpen);
    journal_ = std::make_unique<RangeJournal>(intervalPath(), history_, history_.validatorSet(), local_validator_, policy_);
    // Only a durable preparing checkpoint authorizes creation. Missing active state may contain lost locks.
    const bool create = !active && !fs::exists(intervalPath());
    std::string error;
    if (!journal_->open(create, error)) throw std::runtime_error("open next interval: " + error);
    failAt(TransitionFailure::AfterJournalOpen);
    if (!active) {
        failAt(TransitionFailure::BeforeActivation);
        checkpoint(history_, true);
        failAt(TransitionFailure::AfterActivation);
    }
}

void ContinuousChain::advance(const PrimeBlock& block) {
    auto next = history_;
    std::string error;
    if (next.blockCount() >= kMaxBlocks || !next.append(block, error))
        throw std::runtime_error("cannot install closing block: " + error);
    const auto path = blockPath(block.height);
    failAt(TransitionFailure::BeforeBlockPublish);
    if (fs::exists(path)) {
        const auto existing = readBlock(path);
        auto check = history_;
        if (!check.append(existing, error) || blockSubjectHash(existing) != blockSubjectHash(block))
            throw std::runtime_error("existing block conflicts with sealed interval");
    } else publish(path, encodeBlock(block));
    failAt(TransitionFailure::AfterBlockPublish);
    failAt(TransitionFailure::BeforeCheckpoint);
    checkpoint(next, false);
    failAt(TransitionFailure::AfterCheckpoint);
    history_ = std::move(next);
    journal_.reset();
    openInterval(false);
}

bool ContinuousChain::open(bool create, std::string& error) {
    error.clear();
    if (attempted_open_) { error = "recreate chain object to reopen"; return false; }
    attempted_open_ = true;
    try {
        const auto& validators = history_.validatorSet();
        if (validators.size() != 3 || policy_.max_members == 0 || policy_.max_members > kMaxRangeMembers ||
            std::adjacent_find(validators.begin(), validators.end()) != validators.end() ||
            !std::all_of(validators.begin(), validators.end(), crypto::isProtocolSignatureAddress) ||
            !std::binary_search(validators.begin(), validators.end(), local_validator_))
            throw std::runtime_error("invalid continuous-chain configuration");
        auto initial = history_;
        if (!initial.append(genesis_, error)) throw std::runtime_error("invalid trusted genesis: " + error);
        if (create) {
            if (!fs::create_directory(directory_)) throw std::runtime_error("refusing to initialize existing chain directory");
            fs::permissions(directory_, fs::perms::owner_all, fs::perm_options::replace);
            syncDirectory(fs::path(directory_).parent_path());
        } else if (!fs::is_directory(directory_)) throw std::runtime_error("chain directory missing");
        writer_fd_ = ::open((fs::path(directory_) / "writer.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (writer_fd_ < 0 || flock(writer_fd_, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("another writer owns chain or writer lock unavailable");
        if (create) {
            publish(blockPath(0), encodeBlock(genesis_));
            checkpoint(initial, false);
        }
        auto bytes = read((fs::path(directory_) / "checkpoint.dat").string());
        if (bytes.size() < 32) throw std::runtime_error("truncated chain checkpoint");
        Hash256 checksum{};
        std::copy(bytes.end() - 32, bytes.end(), checksum.begin());
        bytes.resize(bytes.size() - 32);
        if (crypto::sha3_256(bytes) != checksum) throw std::runtime_error("chain checkpoint checksum mismatch");
        detail::Reader reader(bytes);
        reader.magic(kChainMagic);
        if (reader.hash() != configurationHash()) throw std::runtime_error("chain identity, genesis, validators, or policy mismatch");
        const auto count = reader.count(kMaxBlocks);
        const auto tip = reader.hash();
        const bool active = reader.count(1) != 0;
        reader.finish();
        if (count == 0) throw std::runtime_error("checkpoint missing genesis");
        for (std::size_t height = 0; height < count; ++height) {
            const auto block = readBlock(blockPath(height));
            if (height == 0 && blockSubjectHash(block) != blockSubjectHash(genesis_))
                throw std::runtime_error("stored genesis differs from trusted genesis");
            if (!history_.append(block, error)) throw std::runtime_error("chain replay: " + error);
        }
        if (history_.tip() != tip) throw std::runtime_error("chain checkpoint tip mismatch");
        openInterval(active);
        // A sealed interval is a recoverable commit intent even before chain publication.
        if (journal_->closed()) advance(readBlock(intervalPath() + ".block"));
        ready_ = true;
        return true;
    } catch (const std::exception& exception) {
        ready_ = false;
        error = exception.what();
        return false;
    }
}

bool ContinuousChain::available(std::string& error) const {
    error.clear();
    if (ready_ && journal_ && journal_->ready() && !journal_->closed()) return true;
    error = "chain unavailable; reopen required";
    return false;
}

std::optional<protocol::ValidatorVoteV0> ContinuousChain::voteForRange(
    const CompositeRange& proposal, const crypto::SignatureKeyPair& identity, std::string& error) {
    if (!available(error)) return std::nullopt;
    const auto vote = journal_->voteFor(proposal, identity, error);
    if (!journal_->ready()) ready_ = false;
    return vote;
}

RangeAcceptance ContinuousChain::accept(const CompositeRange& range, std::string& error) {
    if (!available(error)) return RangeAcceptance::RecoveryRequired;
    const auto result = journal_->accept(range, error);
    if (!journal_->ready()) ready_ = false;
    return result;
}

std::optional<protocol::ValidatorVoteV0> ContinuousChain::voteForBlock(
    const PrimeBlock& proposal, const crypto::SignatureKeyPair& identity, std::string& error) {
    if (!available(error)) return std::nullopt;
    const auto vote = journal_->voteForBlock(proposal, identity, error);
    if (!journal_->ready()) ready_ = false;
    return vote;
}

BlockInstallation ContinuousChain::install(const PrimeBlock& block, std::string& error) {
    if (!available(error)) return BlockInstallation::RecoveryRequired;
    try {
        if (block.height < history_.blockCount()) {
            const auto existing = readBlock(blockPath(block.height));
            protocol::FinalizationProofV0 certificate;
            certificate.rule = "fixed-2-of-3-mldsa65-v2";
            certificate.votes = block.votes;
            const auto subject = blockSubjectHash(block);
            if (subject != blockSubjectHash(existing) ||
                !protocol::verifyRecordFinalization(certificate, subject, block.previous_hash, block.prime.p,
                                                    history_.validatorSet(), error)) {
                if (error.empty()) error = "retry conflicts with installed block";
                return BlockInstallation::Rejected;
            }
            return BlockInstallation::AlreadyInstalled;
        }
        if (history_.blockCount() >= kMaxBlocks) { error = "chain experiment block limit"; return BlockInstallation::Rejected; }
        if (!journal_->seal(block, error)) {
            if (journal_->ready()) return BlockInstallation::Rejected;
            ready_ = false;
            return BlockInstallation::RecoveryRequired;
        }
        failAt(TransitionFailure::AfterSeal);
        advance(block);
        return BlockInstallation::Installed;
    } catch (const std::exception& exception) {
        ready_ = false;
        error = exception.what();
        return BlockInstallation::RecoveryRequired;
    }
}

PrimeValue ContinuousChain::acceptedFrontier() const {
    return journal_ ? journal_->acceptedFrontier() : history_.frontier();
}

Hash256 ContinuousChain::acceptedRangeTip() const { return journal_ ? journal_->acceptedRangeTip() : Hash256{}; }

std::vector<CompositeProof> ContinuousChain::composites() const {
    return journal_ ? journal_->composites() : std::vector<CompositeProof>{};
}

} // namespace primechain::experiments
