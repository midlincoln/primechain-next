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
    std::size_t max_integer_bytes{64};            // main-chain integers (D14)
    std::size_t max_certificate_integer_bytes{4096};  // certificate fields: rings, curves, landmarks
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

// A miner key registered for the first time in a block. Each key is stored once in the chain's
// lifetime; later submissions by the same address may omit the key.
struct KeyRecord {
    Address address;
    Bytes public_key;
};

// The accepted closing prime, as stored in a block. The miner's signature is journal data: the
// block keeps the evidence (p, witness), the attribution and the hash of the full signed
// submission, so an archived signature can always be matched against it.
struct AcceptedPrime {
    std::uint64_t version{kVersion1};
    Integer prime;
    Integer witness;
    Address provider;
    Hash256 submission_hash{};
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

// D16: a transfer of labeled micro-units. User transactions keep their signatures in blocks
// (needed to replay balances); miner submissions do not (D15).
struct TransferInput {
    Integer asset;
    Amount amount;
};

struct TransferOutput {
    Integer asset;
    Amount amount;
    Address receiver;
};

struct Transfer {
    std::uint64_t version{kVersion1};
    std::string network_id;
    Address sender;
    std::vector<TransferInput> inputs;    // strictly increasing asset order, one entry per asset
    std::vector<TransferOutput> outputs;  // several outputs allowed (bulk send), up to a policy cap
    Integer fee_asset;
    Amount fee;
    std::uint64_t nonce{0};               // per sender, exactly previous + 1 (first nonce is 1)
    Bytes public_key;                     // optional once the sender's key is registered (D15)
    Bytes signature;
};

// D10/D18: a validator binds a hot consensus key to a cold payout address; both keys sign.
struct ValidatorRecord {
    std::uint64_t version{kVersion1};
    Bytes consensus_key;
    Address payout_address;
    Bytes payout_key;
    Bytes consensus_signature;
    Bytes payout_signature;
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
    Hash256 finance_root{};           // reserved (monetary layer)
};

struct Block {
    std::uint64_t version{kVersion1};
    BlockHeader header;
    std::vector<AcceptedRange> ranges;
    std::vector<KeyRecord> new_keys;     // keys first registered in this block
    std::vector<Transfer> transfers;     // D6/D16: included with the closing prime, in order
    std::optional<AcceptedPrime> prime;  // absent only for the genesis seed blocks
};

Bytes rangeSigningPayload(const RangeSubmission& range);
Bytes transferSigningPayload(const Transfer& transfer);
Bytes validatorBindingPayload(const std::string& network_id, const ValidatorRecord& record);
Bytes primeSigningPayload(const PrimeSubmission& prime);
Bytes encode(const RangeSubmission& range);
Bytes encode(const PrimeSubmission& prime);
Bytes encode(const AcceptedRange& range);
Bytes encode(const AcceptedPrime& prime);
Bytes encode(const KeyRecord& key);
Bytes encode(const Transfer& transfer);
Bytes encode(const ValidatorRecord& record);
Bytes encode(const BlockHeader& header);
Bytes encode(const Block& block);

bool decode(const Bytes& data, RangeSubmission& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, PrimeSubmission& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, AcceptedRange& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, AcceptedPrime& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, KeyRecord& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, Transfer& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, ValidatorRecord& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, Block& out, const Limits& limits, std::string& error);

Hash256 submissionHash(const RangeSubmission& range);   // covers the signature bytes
Hash256 submissionHash(const PrimeSubmission& prime);   // covers the signature bytes
Hash256 blockHash(const BlockHeader& header);

// Leaves of ranges_root: each accepted range, each new key, then the accepted prime (type-tagged).
std::vector<Bytes> rangesRootLeaves(const Block& block);

}  // namespace primechain::next
