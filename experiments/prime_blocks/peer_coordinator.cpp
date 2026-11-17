#include "peer_coordinator.hpp"
#include "binary.hpp"

#include <filesystem>
#include <fstream>
#include "primechain/crypto/hash.hpp"
#include "primechain/storage/atomic_file.hpp"

namespace primechain::experiments {
namespace {
constexpr std::array<std::uint8_t, 8> kMagic{'P', 'C', 'N', 'X', 'P', 'Q', '0', '1'};
void blob(protocol::Bytes& bytes, const protocol::Bytes& value) {
    appendWord(bytes, value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
}
protocol::Bytes read(const std::string& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > kExperimentMaxBytes) throw std::runtime_error("peer outbox too large");
    std::ifstream in(path, std::ios::binary);
    protocol::Bytes bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) throw std::runtime_error("cannot read peer outbox");
    return bytes;
}
CompositeRange rangeFrom(const protocol::Bytes& bytes) {
    CompositeRange range;
    std::string error;
    if (!decodeRange(bytes, range, error)) throw std::runtime_error(error);
    return range;
}
PrimeBlock blockFrom(const protocol::Bytes& bytes) {
    PrimeBlock block;
    std::string error;
    if (!decodeBlock(bytes, block, error)) throw std::runtime_error(error);
    return block;
}
bool isRange(RequestKind kind) {
    return kind == RequestKind::VoteRange || kind == RequestKind::DeliverRange;
}
} // namespace

PeerCoordinator::PeerCoordinator(std::string path, ContinuousChain& chain, crypto::SignatureKeyPair identity,
                                 PeerExchange exchange)
    : path_(std::move(path)), chain_(chain), identity_(std::move(identity)),
      local_(crypto::addressFromProtocolPublicKey(identity_.public_key)), exchange_(std::move(exchange)) {
    for (const auto& address : chain.history().validatorSet())
        if (address != local_) peers_.push_back({address, 0});
    cursors_.resize(peers_.size());
    flights_.resize(peers_.size());
    next_attempt_.resize(peers_.size());
    prefer_delivery_.resize(peers_.size());
}

Hash256 PeerCoordinator::configurationHash() const {
    protocol::Bytes bytes(kMagic.begin(), kMagic.end());
    const auto genesis = chain_.installedBlock(0);
    if (!genesis) throw std::runtime_error("peer coordinator requires installed genesis");
    const auto hash = blockSubjectHash(*genesis);
    bytes.insert(bytes.end(), hash.begin(), hash.end());
    appendWord(bytes, chain_.policy().max_members);
    for (const auto& address : chain_.history().validatorSet()) blob(bytes, {address.begin(), address.end()});
    blob(bytes, {local_.begin(), local_.end()});
    return crypto::sha3_256(bytes);
}

Hash256 PeerCoordinator::subject(const Event& event) const {
    return isRange(event.kind) ? rangeSubjectHash(rangeFrom(event.payload)) : blockSubjectHash(blockFrom(event.payload));
}

bool PeerCoordinator::verifyCertificate(const Event& event, std::string& error) const {
    protocol::FinalizationProofV0 proof;
    proof.rule = "fixed-2-of-3-mldsa65-v2";
    Hash256 anchor{};
    PrimeValue integer{};
    if (isRange(event.kind)) {
        const auto range = rangeFrom(event.payload);
        proof.votes = range.votes;
        anchor = range.previous_block;
        integer = range.start;
    } else {
        const auto block = blockFrom(event.payload);
        proof.votes = block.votes;
        anchor = block.previous_hash;
        integer = block.prime.p;
    }
    return protocol::verifyRecordFinalization(proof, subject(event), anchor, integer, chain_.history().validatorSet(), error);
}

std::optional<PeerCoordinator::Event> PeerCoordinator::known(const Event& event) const {
    if (isRange(event.kind)) {
        const auto range = rangeFrom(event.payload);
        const auto receipt = chain_.rangeReceipt(range.previous_block, rangeSubjectHash(range));
        if (receipt) return Event{RequestKind::DeliverRange, encodeRange(*receipt), true};
    } else {
        const auto block = blockFrom(event.payload);
        const auto installed = chain_.installedBlock(block.height);
        if (installed && blockSubjectHash(*installed) == blockSubjectHash(block))
            return Event{RequestKind::DeliverBlock, encodeBlock(*installed), true};
    }
    return std::nullopt;
}

ValidatorReply PeerCoordinator::failure(const std::string& error) const {
    return {ready() ? ReplyKind::Rejected : ReplyKind::RecoveryRequired, {}, error.substr(0, 512)};
}

ValidatorReply PeerCoordinator::apply(const Event& event) {
    std::string error;
    if (!verifyCertificate(event, error)) return failure(error);
    if (const auto receipt = known(event))
        return {isRange(event.kind) ? ReplyKind::AlreadyAccepted : ReplyKind::AlreadyInstalled, {}, {}};
    return handleValidatorRequest(encodeRequest(isRange(event.kind) ? RequestKind::AcceptRange : RequestKind::InstallBlock,
                                                event.payload), chain_, identity_);
}

void PeerCoordinator::failAt(PeerFailure point) {
    if (failure_ == point) {
        failure_ = PeerFailure::None;
        throw std::runtime_error("injected peer certificate publication failure");
    }
}

void PeerCoordinator::persist(bool certificate) {
    protocol::Bytes bytes(kMagic.begin(), kMagic.end());
    const auto configuration = configurationHash();
    bytes.insert(bytes.end(), configuration.begin(), configuration.end());
    appendWord(bytes, outbox_.size());
    for (const auto& event : outbox_) {
        appendWord(bytes, static_cast<std::uint64_t>(event.kind));
        appendWord(bytes, event.applied);
        blob(bytes, event.payload);
    }
    appendWord(bytes, intent_ ? 1 : 0);
    if (intent_) {
        appendWord(bytes, static_cast<std::uint64_t>(intent_->kind));
        blob(bytes, intent_->payload);
    }
    for (const auto cursor : cursors_) appendWord(bytes, cursor);
    const auto checksum = crypto::sha3_256(bytes);
    bytes.insert(bytes.end(), checksum.begin(), checksum.end());
    if (bytes.size() > kExperimentMaxBytes || outbox_.size() > kMaxPeerCertificates)
        throw std::runtime_error("peer outbox experiment limit");
    const auto temp = storage::detail::uniqueAtomicTempPath(path_);
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out.close();
        if (!out) throw std::runtime_error("cannot write peer outbox");
        if (certificate) failAt(PeerFailure::BeforeCertificatePublish);
        std::string error;
        if (!storage::detail::commitAtomicTemp(temp, path_, "peer outbox", error)) throw std::runtime_error(error);
        if (certificate) failAt(PeerFailure::AfterCertificatePublish);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw;
    }
}

bool PeerCoordinator::open(bool create, std::string& error) {
    error.clear();
    if (opened_) { error = "recreate peer coordinator to reopen"; return false; }
    opened_ = true;
    try {
        if (!chain_.ready() || peers_.size() != 2 || !exchange_)
            throw std::runtime_error("invalid peer coordinator configuration");
        if (create) {
            if (std::filesystem::exists(path_)) throw std::runtime_error("peer outbox already exists");
            persist();
        }
        auto bytes = read(path_);
        if (bytes.size() < 32) throw std::runtime_error("truncated peer outbox");
        Hash256 checksum{};
        std::copy(bytes.end() - 32, bytes.end(), checksum.begin());
        bytes.resize(bytes.size() - 32);
        if (crypto::sha3_256(bytes) != checksum) throw std::runtime_error("peer outbox checksum mismatch");
        detail::Reader reader(bytes);
        reader.magic(kMagic);
        if (reader.hash() != configurationHash()) throw std::runtime_error("peer outbox configuration mismatch");
        const auto count = reader.count(kMaxPeerCertificates);
        for (std::size_t i = 0; i < count; ++i) {
            Event event{static_cast<RequestKind>(reader.integer()), {}, reader.count(1) != 0};
            if (event.kind != RequestKind::DeliverRange && event.kind != RequestKind::DeliverBlock)
                throw std::runtime_error("invalid peer outbox event type");
            if (!event.applied && i + 1 != count) throw std::runtime_error("unapplied event must be last");
            event.payload = reader.blob(kExperimentMaxBytes);
            if (!verifyCertificate(event, error)) throw std::runtime_error(error);
            if (event.applied && !known(event)) throw std::runtime_error("applied outbox event missing from chain");
            outbox_.push_back(std::move(event));
        }
        if (reader.count(1)) {
            const auto kind = static_cast<RequestKind>(reader.integer());
            if (kind != RequestKind::VoteRange && kind != RequestKind::VoteBlock)
                throw std::runtime_error("invalid peer proposal type");
            intent_ = Event{kind, reader.blob(kExperimentMaxBytes), false};
            (void)subject(*intent_);
        }
        for (auto& cursor : cursors_) cursor = reader.count(count - (count && !outbox_.back().applied ? 1 : 0));
        reader.finish();
        ready_ = true;
        if (!outbox_.empty() && !outbox_.back().applied) {
            const auto result = apply(outbox_.back());
            if (result.kind != ReplyKind::Accepted && result.kind != ReplyKind::AlreadyAccepted &&
                result.kind != ReplyKind::Installed && result.kind != ReplyKind::AlreadyInstalled)
                throw std::runtime_error("outbox intent recovery: " + result.error);
            outbox_.back().applied = true;
            persist();
        }
        synchronizeJob();
        return true;
    } catch (const std::exception& exception) {
        ready_ = false;
        error_ = error = exception.what();
        return false;
    }
}

void PeerCoordinator::setPeers(std::vector<LocalPeer> peers) {
    std::sort(peers.begin(), peers.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
    if (peers.size() != peers_.size()) throw std::runtime_error("expected exactly two configured peers");
    for (std::size_t i = 0; i < peers.size(); ++i)
        if (peers[i].address != peers_[i].address || !peers[i].port)
            throw std::runtime_error("peer identity or port mismatch");
    if (peers[0].port == peers[1].port) throw std::runtime_error("duplicate peer endpoints");
    peers_ = std::move(peers);
    for (auto& next : next_attempt_) next = {};
}

void PeerCoordinator::synchronizeJob() {
    if (!intent_) { job_.reset(); return; }
    std::optional<Event> locked;
    if (const auto range = chain_.pendingRange()) locked = Event{RequestKind::VoteRange, encodeRange(*range), false};
    else if (const auto block = chain_.pendingBlock()) locked = Event{RequestKind::VoteBlock, encodeBlock(*block), false};
    if (!locked || locked->kind != intent_->kind || subject(*locked) != subject(*intent_)) {
        intent_.reset();
        job_.reset();
        persist();
        return;
    }
    if (job_ && job_->subject == subject(*locked)) return;
    const auto reply = handleValidatorRequest(encodeRequest(locked->kind, locked->payload), chain_, identity_);
    if (reply.kind != ReplyKind::Vote) throw std::runtime_error("resume durable proposal: " + reply.error);
    const auto own = isRange(locked->kind) ? rangeFrom(reply.payload).votes.at(0) : blockFrom(reply.payload).votes.at(0);
    job_ = Job{*locked, subject(*locked), {{local_, own}}};
}

ValidatorReply PeerCoordinator::handle(const protocol::Bytes& request) {
    if (!ready()) return failure(error_.empty() ? "peer coordinator unavailable" : error_);
    ValidatorRequest decoded;
    std::string error;
    if (!decodeRequest(request, decoded, error)) return failure(error);
    try {
        if (decoded.kind == RequestKind::SubmitRange || decoded.kind == RequestKind::DeliverRange)
            (void)rangeFrom(decoded.payload);
        else if (decoded.kind == RequestKind::SubmitBlock || decoded.kind == RequestKind::DeliverBlock)
            (void)blockFrom(decoded.payload);
    } catch (const std::exception& exception) { return failure(exception.what()); }
    try {
        const auto kind = decoded.kind;
        if (kind == RequestKind::DeliverRange || kind == RequestKind::DeliverBlock) {
            const auto reply = apply({kind, decoded.payload, false});
            if (!chain_.ready()) ready_ = false;
            synchronizeJob();
            return reply;
        }
        if (kind != RequestKind::SubmitRange && kind != RequestKind::SubmitBlock) {
            if (kind == RequestKind::AcceptRange || kind == RequestKind::InstallBlock)
                return failure("peer mode requires certified delivery operation");
            return handleValidatorRequest(request, chain_, identity_);
        }
        Event proposal{kind == RequestKind::SubmitRange ? RequestKind::VoteRange : RequestKind::VoteBlock, decoded.payload, false};
        if (isRange(proposal.kind) ? !rangeFrom(proposal.payload).votes.empty() : !blockFrom(proposal.payload).votes.empty())
            return failure("miner submission must not include validator votes");
        if (const auto receipt = known(proposal))
            return {isRange(proposal.kind) ? ReplyKind::AlreadyAccepted : ReplyKind::AlreadyInstalled, receipt->payload, {}};
        if (outbox_.size() >= kMaxPeerCertificates) return failure("peer outbox experiment capacity reached");
        const auto reply = handleValidatorRequest(encodeRequest(proposal.kind, proposal.payload), chain_, identity_);
        if (reply.kind != ReplyKind::Vote) return reply;
        const auto id = subject(proposal);
        if (!intent_ || intent_->kind != proposal.kind || subject(*intent_) != id) {
            intent_ = proposal;
            persist();
        }
        synchronizeJob();
        return {ReplyKind::Pending, protocol::Bytes(id.begin(), id.end()), {}};
    } catch (const std::exception& exception) {
        // Envelope/payload parsing was checked above. Storage uncertainty requires recovery.
        ready_ = false;
        error_ = exception.what();
        return failure(error_);
    }
}

void PeerCoordinator::publishCertificate() {
    if (!job_ || job_->votes.size() < 2) return;
    Event event{isRange(job_->proposal.kind) ? RequestKind::DeliverRange : RequestKind::DeliverBlock, {}, false};
    std::vector<protocol::ValidatorVoteV0> votes;
    for (const auto& [address, vote] : job_->votes) { (void)address; votes.push_back(vote); }
    if (isRange(event.kind)) {
        auto range = rangeFrom(job_->proposal.payload);
        range.votes = votes;
        event.payload = encodeRange(range);
    } else {
        auto block = blockFrom(job_->proposal.payload);
        block.votes = votes;
        event.payload = encodeBlock(block);
    }
    std::string error;
    if (!verifyCertificate(event, error)) throw std::runtime_error(error);
    outbox_.push_back(event);
    persist(true);
    const auto result = apply(event);
    if (result.kind != ReplyKind::Accepted && result.kind != ReplyKind::AlreadyAccepted &&
        result.kind != ReplyKind::Installed && result.kind != ReplyKind::AlreadyInstalled)
        throw std::runtime_error("cannot apply assembled certificate: " + result.error);
    failAt(PeerFailure::AfterLocalApply);
    outbox_.back().applied = true;
    intent_.reset();
    job_.reset();
    persist();
}

void PeerCoordinator::finish(std::size_t peer) {
    auto& flight = flights_[peer];
    if (!flight || flight->reply.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::optional<ValidatorReply> reply;
    try { reply = flight->reply.get(); } catch (...) {}
    const bool delivery = flight->delivery;
    const auto index = flight->index;
    const auto hash = flight->subject;
    flight.reset();
    next_attempt_[peer] = std::chrono::steady_clock::now() + std::chrono::milliseconds(reply ? 10 : 250);
    if (!reply) return;
    if (delivery) {
        const auto kind = outbox_.at(index).kind;
        const bool accepted = isRange(kind)
            ? reply->kind == ReplyKind::Accepted || reply->kind == ReplyKind::AlreadyAccepted
            : reply->kind == ReplyKind::Installed || reply->kind == ReplyKind::AlreadyInstalled;
        if (accepted && cursors_[peer] == index) { ++cursors_[peer]; persist(); }
        return;
    }
    if (!job_ || job_->subject != hash || reply->kind != ReplyKind::Vote) return;
    try {
        const Event response{job_->proposal.kind, reply->payload, false};
        if (subject(response) != hash) return;
        const auto votes = isRange(response.kind) ? rangeFrom(response.payload).votes : blockFrom(response.payload).votes;
        if (votes.size() != 1) return;
        const auto& vote = votes[0];
        std::string error;
        if (vote.validator_address != peers_[peer].address || vote.record_hash != hash || vote.round != 1 ||
            crypto::addressFromProtocolPublicKey(vote.public_key) != vote.validator_address ||
            !crypto::verifyProtocolMessageSignature(vote.public_key,
                crypto::recordFinalizationVoteSigningPayload(hash, vote.round, vote.validator_address), vote.signature, error)) return;
        job_->votes.emplace(vote.validator_address, vote);
    } catch (const std::exception&) { /* Malformed peer replies do not count toward quorum. */ }
}

void PeerCoordinator::tick() {
    if (!ready()) return;
    try {
        synchronizeJob();
        for (std::size_t i = 0; i < peers_.size(); ++i) finish(i);
        publishCertificate();
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            if (flights_[i] || !peers_[i].port || std::chrono::steady_clock::now() < next_attempt_[i]) continue;
            const bool deliver = cursors_[i] < outbox_.size() && outbox_[cursors_[i]].applied;
            const bool vote = job_ && !job_->votes.count(peers_[i].address);
            if (!deliver && !vote) continue;
            const bool delivery = deliver && (!vote || prefer_delivery_[i]);
            prefer_delivery_[i] = !delivery;
            const auto event = delivery ? outbox_[cursors_[i]] : job_->proposal;
            const auto bytes = encodeRequest(event.kind, event.payload);
            const auto peer = peers_[i];
            const auto exchange = exchange_;
            flights_[i] = Flight{delivery, cursors_[i], subject(event),
                std::async(std::launch::async, [exchange, peer, bytes] { return exchange(peer, bytes); })};
        }
    } catch (const std::exception& exception) {
        ready_ = false;
        error_ = exception.what();
    }
}

} // namespace primechain::experiments
