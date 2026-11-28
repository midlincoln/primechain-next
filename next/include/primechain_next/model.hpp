#pragma once
// Data model for the PrimeChain Next main chain (design decisions D2, D4, D9, D10, D12).
#include "primechain_next/codec.hpp"

#include <optional>
#include <string>
#include <vector>

namespace primechain::next {

constexpr std::uint64_t kVersion1 = 1;
constexpr std::uint64_t kMaxRangeComposites = 1024;  // D4.6 hard bound (decoders); the voted limit is a parameter (D20)

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
// B3.5: a transfer of version 2 may name ring primes: the asset is (ring, asset, index), where
// asset is the main-chain prime q below (ring-records section 1). Version 1 names main-chain primes
// only (ring and index 0) and keeps its encoding.
constexpr std::uint64_t kTransferVersionRings = 2;

struct TransferInput {
    Integer asset;
    Amount amount;
    std::uint64_t ring{0};    // version 2
    std::uint32_t index{0};   // version 2
};

struct TransferOutput {
    Integer asset;
    Amount amount;
    Address receiver;
    std::uint64_t ring{0};    // version 2
    std::uint32_t index{0};   // version 2
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
    std::uint64_t fee_ring{0};            // version 2: the fee asset is (fee_ring, fee_asset, fee_index)
    std::uint32_t fee_index{0};
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

// A3-2: governance. A proposal is position-independent: it names its content, the heights at
// which it takes effect and expires, a sequence number and the validator set that votes on it,
// but nothing about the chain tip, so votes stay valid while the chain moves.
// B3.6 (R2): RingPause pauses or unpauses one ring after the notice; EmergencyPause pauses one ring
// (ring >= 1) or all rings (ring = 0) from the block after the one that completes the quorum.
enum class GovernanceKind : std::uint8_t {
    ParamChange = 1,
    ValidatorAdd = 2,
    ValidatorRemove = 3,
    RingPause = 4,
    EmergencyPause = 5
};

// D20: one parameter changed from `old_value` (must be the value in force) to `new_value`.
struct ParameterChange {
    std::uint32_t id{0};
    u128 old_value{0};
    u128 new_value{0};
};

struct GovernanceProposal {
    std::uint64_t version{kVersion1};
    std::string network_id;
    GovernanceKind kind{GovernanceKind::ParamChange};
    std::vector<ParameterChange> changes;  // ParamChange: increasing parameter ids
    ValidatorRecord validator;           // ValidatorAdd (binding signed by both keys, D10)
    Hash256 remove_key_hash{};           // ValidatorRemove: sha3 of the consensus key
    std::uint64_t ring{0};               // RingPause (>= 1); EmergencyPause (0 = all rings)
    bool pause{true};                    // RingPause: pause (true) or unpause (false)
    Hash256 evidence{};                  // EmergencyPause: hash of the fault report (nonzero), on chain
    std::uint64_t effective_height{0};
    std::uint64_t expiry_height{0};      // votes are accepted only in blocks up to this height
    std::uint64_t sequence{0};
    Hash256 validator_set_digest{};      // the set whose votes count
};

// On-chain governance transaction: a validator's vote (signed with its consensus key), or the
// "ready" message of a validator being added (signed with its new consensus key, A3-3).
enum class GovernanceTxKind : std::uint8_t { Vote = 1, Ready = 2 };

struct GovernanceTx {
    std::uint64_t version{kVersion1};
    GovernanceTxKind kind{GovernanceTxKind::Vote};
    GovernanceProposal proposal;
    Bytes signer_key;
    Bytes signature;
};

// B3.4: opening rings (docs/ring-records.md section 2, rings-design.md R5). Hard decoder bound for
// the certificate bytes of a reveal; the voted limit is parameter CertificateBytes ([29]).
constexpr std::size_t kMaxCertificateBytes = 64u << 20;
constexpr std::size_t kRingSaltBytes = 32;  // proposed: a fixed 32-byte salt

// RING_COMMIT (main-block transaction): a sealed claim on a ring not yet named. The deposit
// leaves the discoverer and is held by nobody until the reveal (refund) or expiry (fee pool).
// commitment = ringCommitment(network, discoverer, salt, certificateHash(type, version, bytes)).
struct RingCommit {
    std::uint64_t version{kVersion1};
    std::string network_id;
    Address discoverer;
    Hash256 commitment{};
    Integer deposit_asset;                // a main-chain prime
    Amount deposit;                       // exactly parameter RingDeposit in force (0 allowed)
    std::uint64_t nonce{0};               // the discoverer's nonce sequence, shared with transfers
    Bytes public_key;                     // optional once the key is registered (D15)
    Bytes signature;
};

// RING_REVEAL (carried by a ring batch, B3.6). No signature: only the discoverer knows the salt.
// commit_index is the commit's position in the ring_commits list of block commit_height.
struct RingReveal {
    std::uint64_t version{kVersion1};
    std::uint64_t commit_height{0};
    std::uint64_t commit_index{0};
    Bytes salt;                           // kRingSaltBytes
    std::uint64_t certificate_type{1};    // 1 = quadratic (R4)
    std::uint64_t certificate_version{kVersion1};
    Bytes certificate;                    // encode(QuadCertificate) for type 1
    bool operator==(const RingReveal&) const = default;
};

// RING_PAYEE (main-block transaction): the current payee of a ring names the next one (R6).
struct RingPayee {
    std::uint64_t version{kVersion1};
    std::string network_id;
    std::uint64_t ring{0};                // ring number
    Address payee;                        // the new payee
    std::uint64_t nonce{0};               // the current payee's nonce sequence
    Bytes public_key;                     // of the current payee; optional once registered
    Bytes signature;                      // by the current payee
};

Hash256 certificateHash(std::uint64_t type, std::uint64_t version, const Bytes& certificate);
Hash256 ringCommitment(const std::string& network_id, const Address& discoverer, const Bytes& salt,
                       const Hash256& certificate_hash);

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
    std::vector<GovernanceTx> governance;  // A3-2: votes and ready messages, after the transfers
    std::optional<AcceptedPrime> prime;  // absent only for the genesis seed blocks
    // B3.4. Order of application: transfers, ring commits, ring payees, governance, then the
    // ring step (expiry, openings). Reveals are in ring batches (B3.6), applied before the block.
    std::vector<RingCommit> ring_commits;
    std::vector<RingPayee> ring_payees;
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
Bytes encode(const GovernanceProposal& proposal);
Bytes encode(const GovernanceTx& tx);
Bytes encode(const BlockHeader& header);
Bytes encode(const Block& block);
Bytes ringCommitSigningPayload(const RingCommit& commit);
Bytes ringPayeeSigningPayload(const RingPayee& payee);
Bytes encode(const RingCommit& commit);
Bytes encode(const RingReveal& reveal);
Bytes encode(const RingPayee& payee);
bool decode(const Bytes& data, RingCommit& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, RingReveal& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, RingPayee& out, const Limits& limits, std::string& error);

bool decode(const Bytes& data, RangeSubmission& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, PrimeSubmission& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, AcceptedRange& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, AcceptedPrime& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, KeyRecord& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, Transfer& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, ValidatorRecord& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, GovernanceProposal& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, GovernanceTx& out, const Limits& limits, std::string& error);
bool decode(const Bytes& data, Block& out, const Limits& limits, std::string& error);

Hash256 proposalId(const GovernanceProposal& proposal);
Bytes governanceSigningPayload(const GovernanceTx& tx);  // kind + proposal id, domain-separated

Hash256 submissionHash(const RangeSubmission& range);   // covers the signature bytes
Hash256 submissionHash(const PrimeSubmission& prime);   // covers the signature bytes
Hash256 blockHash(const BlockHeader& header);

// Leaves of ranges_root: each accepted range, each new key, then the accepted prime (type-tagged).
std::vector<Bytes> rangesRootLeaves(const Block& block);

}  // namespace primechain::next
