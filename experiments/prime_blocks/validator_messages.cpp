#include "validator_messages.hpp"
#include "binary.hpp"
#include <type_traits>

namespace primechain::experiments {
namespace {
constexpr std::array<std::uint8_t, 8> kRequestMagic{'P', 'C', 'N', 'X', 'R', 'P', 'Q', '1'};
constexpr std::array<std::uint8_t, 8> kReplyMagic{'P', 'C', 'N', 'X', 'R', 'P', 'S', '1'};
constexpr std::size_t kMaxErrorBytes = 512;
constexpr std::array<std::uint8_t, 8> kStatusMagic{'P', 'C', 'N', 'X', 'S', 'T', '0', '1'};

void blob(protocol::Bytes& out, const protocol::Bytes& value) {
    appendWord(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void bound(const protocol::Bytes& bytes) {
    if (bytes.size() > kExperimentMaxBytes) throw std::runtime_error("message exceeds experiment limit");
}
} // namespace

protocol::Bytes encodeRequest(RequestKind kind, const protocol::Bytes& payload) {
    bound(payload);
    if (kind > RequestKind::DeliverBlock) throw std::runtime_error("unknown request kind");
    protocol::Bytes bytes(kRequestMagic.begin(), kRequestMagic.end());
    appendWord(bytes, static_cast<std::uint64_t>(kind));
    blob(bytes, payload);
    bound(bytes);
    return bytes;
}

bool decodeRequest(const protocol::Bytes& bytes, ValidatorRequest& request, std::string& error) {
    error.clear();
    try {
        bound(bytes);
        detail::Reader reader(bytes);
        reader.magic(kRequestMagic);
        ValidatorRequest result;
        result.kind = static_cast<RequestKind>(reader.count(static_cast<std::size_t>(RequestKind::DeliverBlock)));
        result.payload = reader.blob(kExperimentMaxBytes);
        reader.finish();
        request = std::move(result);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

protocol::Bytes encodeReply(const ValidatorReply& reply) {
    bound(reply.payload);
    if (reply.kind > ReplyKind::Pending || reply.error.size() > kMaxErrorBytes)
        throw std::runtime_error("invalid reply kind or error length");
    protocol::Bytes bytes(kReplyMagic.begin(), kReplyMagic.end());
    appendWord(bytes, static_cast<std::uint64_t>(reply.kind));
    blob(bytes, reply.payload);
    blob(bytes, protocol::Bytes(reply.error.begin(), reply.error.end()));
    bound(bytes);
    return bytes;
}

bool decodeReply(const protocol::Bytes& bytes, ValidatorReply& reply, std::string& error) {
    error.clear();
    try {
        bound(bytes);
        detail::Reader reader(bytes);
        reader.magic(kReplyMagic);
        ValidatorReply result;
        result.kind = static_cast<ReplyKind>(reader.count(static_cast<std::size_t>(ReplyKind::Pending)));
        result.payload = reader.blob(kExperimentMaxBytes);
        const auto message = reader.blob(kMaxErrorBytes);
        result.error.assign(message.begin(), message.end());
        reader.finish();
        reply = std::move(result);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool decodeChainStatus(const protocol::Bytes& bytes, ChainStatus& status, std::string& error) {
    error.clear();
    try {
        bound(bytes);
        detail::Reader reader(bytes);
        reader.magic(kStatusMagic);
        ChainStatus result;
        result.max_range_members = reader.integer();
        result.finalized_frontier = reader.integer();
        result.accepted_frontier = reader.integer();
        result.block_count = reader.integer();
        result.block_tip = reader.hash();
        result.range_tip = reader.hash();
        result.ready = reader.count(1) != 0;
        reader.finish();
        status = result;
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

template<class State>
ValidatorReply handleRequest(const protocol::Bytes& request, State& journal,
                             const crypto::SignatureKeyPair& identity) {
    constexpr bool continuous = std::is_same_v<State, ContinuousChain>;
    auto failure = [&](const std::string& error) {
        return ValidatorReply{journal.ready() ? ReplyKind::Rejected : ReplyKind::RecoveryRequired,
                              {}, error.substr(0, kMaxErrorBytes)};
    };
    try {
        std::string error;
        ValidatorRequest decoded;
        if (!decodeRequest(request, decoded, error)) return failure(error);
        const auto kind = decoded.kind;
        const auto& payload = decoded.payload;
        if (kind > RequestKind::InstallBlock) return failure("operation requires peer coordinator");
        if (kind == RequestKind::Status || kind == RequestKind::ChainStatus) {
            if (!payload.empty()) return failure("status request must be empty");
            protocol::Bytes status;
            if constexpr (continuous) {
                if (kind != RequestKind::ChainStatus) return failure("continuous chain requires ChainStatus");
                status.assign(kStatusMagic.begin(), kStatusMagic.end());
                appendWord(status, journal.policy().max_members);
                appendWord(status, journal.history().frontier());
                appendWord(status, journal.acceptedFrontier());
                appendWord(status, journal.history().blockCount());
                const auto tip = journal.history().tip();
                status.insert(status.end(), tip.begin(), tip.end());
                const auto range_tip = journal.acceptedRangeTip();
                status.insert(status.end(), range_tip.begin(), range_tip.end());
                appendWord(status, journal.ready());
            } else {
                if (kind != RequestKind::Status) return failure("interval journal requires Status");
                appendWord(status, journal.policy().max_members);
                appendWord(status, journal.acceptedFrontier());
                appendWord(status, journal.rangeCount());
                appendWord(status, journal.closed());
                appendWord(status, journal.ready());
            }
            return {ReplyKind::Status, std::move(status), {}};
        }
        if (kind == RequestKind::VoteRange || kind == RequestKind::AcceptRange) {
            CompositeRange range;
            if (!decodeRange(payload, range, error)) return failure(error);
            if (kind == RequestKind::VoteRange) {
                if (!range.votes.empty()) return failure("range voting request must not contain votes");
                const auto vote = [&] {
                    if constexpr (continuous) return journal.voteForRange(range, identity, error);
                    else return journal.voteFor(range, identity, error);
                }();
                if (!vote) return failure(error);
                range.votes = {*vote};
                return {ReplyKind::Vote, encodeRange(range), {}};
            }
            switch (journal.accept(range, error)) {
                case RangeAcceptance::Accepted: return {ReplyKind::Accepted, {}, {}};
                case RangeAcceptance::AlreadyAccepted: return {ReplyKind::AlreadyAccepted, {}, {}};
                case RangeAcceptance::RecoveryRequired: return {ReplyKind::RecoveryRequired, {}, error.substr(0, kMaxErrorBytes)};
                case RangeAcceptance::Rejected: return failure(error);
            }
        }
        if constexpr (continuous) {
            if (kind == RequestKind::SealBlock) return failure("continuous chain requires InstallBlock");
        } else {
            if (kind == RequestKind::InstallBlock) return failure("interval journal requires SealBlock");
        }
        PrimeBlock block;
        if (!decodeBlock(payload, block, error)) return failure(error);
        if (kind == RequestKind::VoteBlock) {
            if (!block.votes.empty()) return failure("block voting request must not contain votes");
            const auto vote = journal.voteForBlock(block, identity, error);
            if (!vote) return failure(error);
            block.votes = {*vote};
            return {ReplyKind::Vote, encodeBlock(block), {}};
        }
        if constexpr (continuous) {
            switch (journal.install(block, error)) {
                case BlockInstallation::Installed: return {ReplyKind::Installed, {}, {}};
                case BlockInstallation::AlreadyInstalled: return {ReplyKind::AlreadyInstalled, {}, {}};
                case BlockInstallation::Rejected: return failure(error);
                case BlockInstallation::RecoveryRequired: return {ReplyKind::RecoveryRequired, {}, error.substr(0, kMaxErrorBytes)};
            }
            return failure("unknown installation result");
        } else {
            if (!journal.seal(block, error)) return failure(error);
            return {ReplyKind::Sealed, {}, {}};
        }
    } catch (const std::exception& exception) { return failure(exception.what()); }
}

ValidatorReply handleValidatorRequest(const protocol::Bytes& request, RangeJournal& journal,
                                     const crypto::SignatureKeyPair& identity) {
    return handleRequest(request, journal, identity);
}

ValidatorReply handleValidatorRequest(const protocol::Bytes& request, ContinuousChain& chain,
                                     const crypto::SignatureKeyPair& identity) {
    return handleRequest(request, chain, identity);
}

} // namespace primechain::experiments
