#pragma once

#include "atomic_ranges.hpp"
#include "continuous_chain.hpp"

namespace primechain::experiments {

enum class RequestKind : std::uint64_t { Status, VoteRange, AcceptRange, VoteBlock, SealBlock, ChainStatus, InstallBlock };
enum class ReplyKind : std::uint64_t { Rejected, Status, Vote, Accepted, AlreadyAccepted, Sealed, RecoveryRequired, Installed, AlreadyInstalled };

struct ChainStatus {
    std::uint64_t max_range_members{0};
    PrimeValue finalized_frontier{0};
    PrimeValue accepted_frontier{0};
    std::uint64_t block_count{0};
    Hash256 block_tip{};
    Hash256 range_tip{};
    bool ready{false};
};

struct ValidatorReply {
    ReplyKind kind{ReplyKind::Rejected};
    protocol::Bytes payload;
    std::string error;
};

protocol::Bytes encodeRequest(RequestKind kind, const protocol::Bytes& payload = {});
protocol::Bytes encodeReply(const ValidatorReply& reply);
bool decodeReply(const protocol::Bytes& bytes, ValidatorReply& reply, std::string& error);
bool decodeChainStatus(const protocol::Bytes& bytes, ChainStatus& status, std::string& error);

// Experimental message boundary only; caller serializes access to the journal.
// No peer discovery, automatic quorum collection, governance, or public listener.
ValidatorReply handleValidatorRequest(const protocol::Bytes& request, RangeJournal& journal,
                                     const crypto::SignatureKeyPair& identity);
ValidatorReply handleValidatorRequest(const protocol::Bytes& request, ContinuousChain& chain,
                                     const crypto::SignatureKeyPair& identity);

} // namespace primechain::experiments
