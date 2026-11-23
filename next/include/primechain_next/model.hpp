#pragma once
// Data model for the PrimeChain Next main chain (design decisions D2, D4, D9, D10, D12).
#include "primechain_next/codec.hpp"

#include <optional>
#include <string>
#include <vector>

namespace primechain::next {

constexpr std::uint64_t kVersion1 = 1;

// Decode limits; every decoder is bounded.
struct Limits {
    std::size_t max_integer_bytes{64};
    std::size_t max_list{1u << 20};
    std::size_t max_text{256};
    std::size_t max_key_bytes{4096};
    std::size_t max_signature_bytes{8192};
};

// A miner's claim that every integer in [start, end] is composite (D1, D2).
// Signature semantics (D4): signing [start, end] claims every composite in it, so any tail
// [s, end] with s >= start may be accepted and credited to the provider.
struct RangeSubmission {
    std::uint64_t version{kVersion1};
    std::string network_id;
    Hash256 anchor{};                 // hash of the chain tip the miner built on
    Integer start;
    Integer end;
    std::vector<Integer> divisors;    // one per integer in [start, end]
    Address provider;
    Bytes public_key;
    Bytes signature;
};

// The closing prime of a block (D2, D5): p and a Pratt witness a.
struct PrimeSubmission {
    std::uint64_t version{kVersion1};
    std::string network_id;
    Hash256 anchor{};
    Integer prime;
    Integer witness;
    Address provider;
    Bytes public_key;
    Bytes signature;
};

// The accepted part of a range submission, as stored in a block. Range signatures are
// journal data; the block keeps the evidence, the attribution and the submission hash.
struct AcceptedRange {
    std::uint64_t version{kVersion1};
    Address provider;
    Integer accepted_start;
    Integer end;
    Hash256 submission_hash{};
    std::vector<Integer> divisors;    // one per integer in [accepted_start, end]
};

struct BlockHeader {                  // D9 (no previous prime)
    std::uint64_t version{kVersion1};
    std::string network_id;
    std::uint64_t height{0};
    Hash256 previous_hash{};
    Integer prime;
    Hash256 policy_digest{};
    Hash256 validator_epoch_digest{};
    Hash256 ranges_root{};            // commits to the accepted ranges and the prime submission
    Hash256 transactions_root{};
    Hash256 state_root{};
    Hash256 extension_root{};
    Hash256 privacy_root{};           // reserved
};

struct Block {
    std::uint64_t version{kVersion1};
    BlockHeader header;
    std::vector<AcceptedRange> ranges;
    std::optional<PrimeSubmission> prime_submission;  // absent only for the genesis seed blocks
};

Bytes rangeSigningPayload(const RangeSubmission& range);
Bytes primeSigningPayload(const PrimeSubmission& prime);
Bytes encode(const RangeSubmission& range);
Bytes encode(const PrimeSubmission& prime);
Bytes encode(const AcceptedRange& range);
Bytes encode(const BlockHeader& header);
Bytes encode(const Block& block);

bool decode(const Bytes& data, RangeSubmission& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, PrimeSubmission& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, AcceptedRange& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, Block& out, const Limits& limits, std::string& error);

Hash256 submissionHash(const RangeSubmission& range);
Hash256 blockHash(const BlockHeader& header);

// Leaves of ranges_root: each accepted range, then the prime submission (type-tagged).
std::vector<Bytes> rangesRootLeaves(const Block& block);

}  // namespace primechain::next
