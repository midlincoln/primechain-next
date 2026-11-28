#include "primechain_next/model.hpp"

#include "primechain_next/merkle.hpp"

namespace primechain::next {
namespace {

enum Tag : std::uint64_t {
    kRangeTag = 1, kPrimeTag = 2, kAcceptedTag = 3, kHeaderTag = 4, kBlockTag = 5, kAcceptedPrimeTag = 6, kKeyTag = 7,
    kTransferTag = 8, kValidatorTag = 9, kProposalTag = 10, kGovernanceTag = 11, kRingCommitTag = 12,
    kRingRevealTag = 13, kRingPayeeTag = 14
};

void writeRangeFields(Writer& w, const RangeSubmission& r) {
    w.varint(kRangeTag);
    w.varint(r.version);
    w.text(r.network_id);
    w.hash(r.anchor);
    w.integer(r.start);
    w.integer(r.end);
    w.varint(r.divisors.size());
    for (const auto& d : r.divisors) w.integer(d);
    w.text(r.provider);
    w.bytes(r.public_key);
}

void writePrimeFields(Writer& w, const PrimeSubmission& p) {
    w.varint(kPrimeTag);
    w.varint(p.version);
    w.text(p.network_id);
    w.hash(p.anchor);
    w.integer(p.prime);
    w.integer(p.witness);
    w.text(p.provider);
    w.bytes(p.public_key);
}

void writeTransferFields(Writer& w, const Transfer& t) {
    w.varint(kTransferTag);
    w.varint(t.version);
    w.text(t.network_id);
    w.text(t.sender);
    w.varint(t.inputs.size());
    for (const auto& in : t.inputs) {
        w.integer(in.asset);
        w.amount(in.amount);
    }
    w.varint(t.outputs.size());
    for (const auto& out : t.outputs) {
        w.integer(out.asset);
        w.amount(out.amount);
        w.text(out.receiver);
    }
    w.integer(t.fee_asset);
    w.amount(t.fee);
    w.varint(t.nonce);
    w.bytes(t.public_key);
}

void writeValidatorFields(Writer& w, const ValidatorRecord& v) {
    w.varint(kValidatorTag);
    w.varint(v.version);
    w.bytes(v.consensus_key);
    w.text(v.payout_address);
    w.bytes(v.payout_key);
}

void writeRingCommitFields(Writer& w, const RingCommit& c) {
    w.varint(kRingCommitTag);
    w.varint(c.version);
    w.text(c.network_id);
    w.text(c.discoverer);
    w.hash(c.commitment);
    w.integer(c.deposit_asset);
    w.amount(c.deposit);
    w.varint(c.nonce);
    w.bytes(c.public_key);
}

void writeRingPayeeFields(Writer& w, const RingPayee& p) {
    w.varint(kRingPayeeTag);
    w.varint(p.version);
    w.text(p.network_id);
    w.varint(p.ring);
    w.text(p.payee);
    w.varint(p.nonce);
    w.bytes(p.public_key);
}

void writeHeader(Writer& w, const BlockHeader& h) {
    w.varint(kHeaderTag);
    w.varint(h.version);
    w.text(h.network_id);
    w.varint(h.height);
    w.hash(h.previous_hash);
    w.integer(h.prime);
    w.hash(h.policy_digest);
    w.hash(h.validator_epoch_digest);
    w.hash(h.ranges_root);
    w.hash(h.transactions_root);
    w.hash(h.state_root);
    w.hash(h.extension_root);
    w.hash(h.privacy_root);
    w.hash(h.finance_root);
}

bool expectTag(Reader& r, std::uint64_t tag, std::uint64_t& version) {
    std::uint64_t got = 0;
    if (!r.varint(got) || got != tag) return false;
    return r.varint(version) && version == kVersion1;  // unknown versions are rejected (D12)
}

bool readRange(Reader& r, RangeSubmission& out, const Limits& l, bool with_signature) {
    std::uint64_t count = 0;
    if (!expectTag(r, kRangeTag, out.version) || !r.text(out.network_id, l.max_text) || !r.hash(out.anchor) ||
        !r.integer(out.start) || !r.integer(out.end) || !r.varint(count) || count > kMaxRangeComposites)
        return false;
    out.divisors.resize(count);
    for (auto& d : out.divisors) if (!r.integer(d)) return false;
    if (!r.text(out.provider, l.max_text) || !r.bytes(out.public_key, l.max_key_bytes)) return false;
    return !with_signature || r.bytes(out.signature, l.max_signature_bytes);
}

bool readPrime(Reader& r, PrimeSubmission& out, const Limits& l) {
    return expectTag(r, kPrimeTag, out.version) && r.text(out.network_id, l.max_text) && r.hash(out.anchor) &&
           r.integer(out.prime) && r.integer(out.witness) && r.text(out.provider, l.max_text) &&
           r.bytes(out.public_key, l.max_key_bytes) && r.bytes(out.signature, l.max_signature_bytes);
}

bool readAccepted(Reader& r, AcceptedRange& out, const Limits& l) {
    std::uint64_t count = 0;
    if (!expectTag(r, kAcceptedTag, out.version) || !r.text(out.provider, l.max_text) ||
        !r.integer(out.accepted_start) || !r.integer(out.end) || !r.hash(out.submission_hash) ||
        !r.varint(count) || count > kMaxRangeComposites)
        return false;
    out.divisors.resize(count);
    for (auto& d : out.divisors) if (!r.integer(d)) return false;
    return true;
}

bool readTransfer(Reader& r, Transfer& t, const Limits& l) {
    std::uint64_t n_in = 0, n_out = 0;
    if (!expectTag(r, kTransferTag, t.version) || !r.text(t.network_id, l.max_text) || !r.text(t.sender, l.max_text) ||
        !r.varint(n_in) || n_in > l.max_list)
        return false;
    t.inputs.resize(n_in);
    for (auto& in : t.inputs) if (!r.integer(in.asset) || !r.amount(in.amount)) return false;
    if (!r.varint(n_out) || n_out > l.max_list) return false;
    t.outputs.resize(n_out);
    for (auto& out : t.outputs)
        if (!r.integer(out.asset) || !r.amount(out.amount) || !r.text(out.receiver, l.max_text)) return false;
    return r.integer(t.fee_asset) && r.amount(t.fee) && r.varint(t.nonce) && r.bytes(t.public_key, l.max_key_bytes) &&
           r.bytes(t.signature, l.max_signature_bytes);
}

bool readValidator(Reader& r, ValidatorRecord& v, const Limits& l) {
    return expectTag(r, kValidatorTag, v.version) && r.bytes(v.consensus_key, l.max_key_bytes) &&
           r.text(v.payout_address, l.max_text) && r.bytes(v.payout_key, l.max_key_bytes) &&
           r.bytes(v.consensus_signature, l.max_signature_bytes) && r.bytes(v.payout_signature, l.max_signature_bytes);
}

bool readAcceptedPrime(Reader& r, AcceptedPrime& out, const Limits& l) {
    return expectTag(r, kAcceptedPrimeTag, out.version) && r.integer(out.prime) && r.integer(out.witness) &&
           r.text(out.provider, l.max_text) && r.hash(out.submission_hash);
}

bool readKey(Reader& r, KeyRecord& out, const Limits& l) {
    std::uint64_t version = 0;
    return expectTag(r, kKeyTag, version) && r.text(out.address, l.max_text) && r.bytes(out.public_key, l.max_key_bytes);
}

bool readHeader(Reader& r, BlockHeader& h, const Limits& l) {
    return expectTag(r, kHeaderTag, h.version) && r.text(h.network_id, l.max_text) && r.varint(h.height) &&
           r.hash(h.previous_hash) && r.integer(h.prime) && r.hash(h.policy_digest) &&
           r.hash(h.validator_epoch_digest) && r.hash(h.ranges_root) && r.hash(h.transactions_root) &&
           r.hash(h.state_root) && r.hash(h.extension_root) && r.hash(h.privacy_root) &&
           r.hash(h.finance_root);
}

bool readProposal(Reader& r, GovernanceProposal& p, const Limits& l) {
    std::uint8_t kind = 0;
    if (!expectTag(r, kProposalTag, p.version) || !r.text(p.network_id, l.max_text) || !r.u8(kind) || kind < 1 || kind > 3)
        return false;
    p.kind = static_cast<GovernanceKind>(kind);
    if (p.kind == GovernanceKind::ParamChange) {
        std::uint64_t count = 0;
        if (!r.varint(count) || count == 0 || count > 256) return false;
        p.changes.resize(count);
        for (auto& c : p.changes) {
            std::uint64_t id = 0;
            Amount old_value, new_value;
            if (!r.varint(id) || id > UINT32_MAX || !r.amount(old_value) || !r.amount(new_value)) return false;
            c = {static_cast<std::uint32_t>(id), old_value.raw(), new_value.raw()};
        }
    }
    if (p.kind == GovernanceKind::ValidatorAdd && !readValidator(r, p.validator, l)) return false;
    if (p.kind == GovernanceKind::ValidatorRemove && !r.hash(p.remove_key_hash)) return false;
    return r.varint(p.effective_height) && r.varint(p.expiry_height) && r.varint(p.sequence) &&
           r.hash(p.validator_set_digest);
}

bool readGovernance(Reader& r, GovernanceTx& t, const Limits& l) {
    std::uint8_t kind = 0;
    if (!expectTag(r, kGovernanceTag, t.version) || !r.u8(kind) || kind < 1 || kind > 2) return false;
    t.kind = static_cast<GovernanceTxKind>(kind);
    return readProposal(r, t.proposal, l) && r.bytes(t.signer_key, l.max_key_bytes) &&
           r.bytes(t.signature, l.max_signature_bytes);
}

bool readRingCommit(Reader& r, RingCommit& c, const Limits& l) {
    return expectTag(r, kRingCommitTag, c.version) && r.text(c.network_id, l.max_text) && r.text(c.discoverer, l.max_text) &&
           r.hash(c.commitment) && r.integer(c.deposit_asset) && r.amount(c.deposit) && r.varint(c.nonce) &&
           r.bytes(c.public_key, l.max_key_bytes) && r.bytes(c.signature, l.max_signature_bytes);
}

bool readRingReveal(Reader& r, RingReveal& v) {
    return expectTag(r, kRingRevealTag, v.version) && r.varint(v.commit_height) && r.varint(v.commit_index) &&
           r.bytes(v.salt, kRingSaltBytes) && r.varint(v.certificate_type) && r.varint(v.certificate_version) &&
           r.bytes(v.certificate, kMaxCertificateBytes);
}

bool readRingPayee(Reader& r, RingPayee& p, const Limits& l) {
    return expectTag(r, kRingPayeeTag, p.version) && r.text(p.network_id, l.max_text) && r.varint(p.ring) &&
           r.text(p.payee, l.max_text) && r.varint(p.nonce) && r.bytes(p.public_key, l.max_key_bytes) &&
           r.bytes(p.signature, l.max_signature_bytes);
}

template <typename T, typename F>
bool decodeStrict(const Bytes& data, T& out, const Limits& limits, std::string& error, F read) {
    Reader r(data, limits.max_integer_bytes);
    if (!read(r, out) || !r.done()) {
        error = r.error().empty() ? (r.done() ? "invalid record" : "trailing bytes") : r.error();
        return false;
    }
    return true;
}

}  // namespace

Bytes rangeSigningPayload(const RangeSubmission& range) {
    Writer w;
    w.text("primechain-next/range-submission");
    writeRangeFields(w, range);
    return w.data();
}

Bytes primeSigningPayload(const PrimeSubmission& prime) {
    Writer w;
    w.text("primechain-next/prime-submission");
    writePrimeFields(w, prime);
    return w.data();
}

Bytes encode(const RangeSubmission& range) {
    Writer w;
    writeRangeFields(w, range);
    w.bytes(range.signature);
    return w.data();
}

Bytes encode(const PrimeSubmission& prime) {
    Writer w;
    writePrimeFields(w, prime);
    w.bytes(prime.signature);
    return w.data();
}

Bytes encode(const AcceptedRange& range) {
    Writer w;
    w.varint(kAcceptedTag);
    w.varint(range.version);
    w.text(range.provider);
    w.integer(range.accepted_start);
    w.integer(range.end);
    w.hash(range.submission_hash);
    w.varint(range.divisors.size());
    for (const auto& d : range.divisors) w.integer(d);
    return w.data();
}

Bytes transferSigningPayload(const Transfer& transfer) {
    Writer w;
    w.text("primechain-next/transfer");
    writeTransferFields(w, transfer);
    return w.data();
}

Bytes validatorBindingPayload(const std::string& network_id, const ValidatorRecord& record) {
    Writer w;
    w.text("primechain-next/validator-binding");
    w.text(network_id);
    writeValidatorFields(w, record);
    return w.data();
}

Bytes encode(const Transfer& transfer) {
    Writer w;
    writeTransferFields(w, transfer);
    w.bytes(transfer.signature);
    return w.data();
}

Bytes encode(const ValidatorRecord& record) {
    Writer w;
    writeValidatorFields(w, record);
    w.bytes(record.consensus_signature);
    w.bytes(record.payout_signature);
    return w.data();
}

Bytes encode(const AcceptedPrime& prime) {
    Writer w;
    w.varint(kAcceptedPrimeTag);
    w.varint(prime.version);
    w.integer(prime.prime);
    w.integer(prime.witness);
    w.text(prime.provider);
    w.hash(prime.submission_hash);
    return w.data();
}

Bytes encode(const KeyRecord& key) {
    Writer w;
    w.varint(kKeyTag);
    w.varint(kVersion1);
    w.text(key.address);
    w.bytes(key.public_key);
    return w.data();
}

Bytes encode(const GovernanceProposal& p) {
    Writer w;
    w.varint(kProposalTag);
    w.varint(p.version);
    w.text(p.network_id);
    w.u8(static_cast<std::uint8_t>(p.kind));
    if (p.kind == GovernanceKind::ParamChange) {
        w.varint(p.changes.size());
        for (const auto& c : p.changes) {
            w.varint(c.id);
            w.amount(Amount::fromRaw(c.old_value));
            w.amount(Amount::fromRaw(c.new_value));
        }
    }
    if (p.kind == GovernanceKind::ValidatorAdd) {
        const Bytes v = encode(p.validator);
        w.raw(v);
    }
    if (p.kind == GovernanceKind::ValidatorRemove) w.hash(p.remove_key_hash);
    w.varint(p.effective_height);
    w.varint(p.expiry_height);
    w.varint(p.sequence);
    w.hash(p.validator_set_digest);
    return w.data();
}

Bytes encode(const GovernanceTx& t) {
    Writer w;
    w.varint(kGovernanceTag);
    w.varint(t.version);
    w.u8(static_cast<std::uint8_t>(t.kind));
    w.raw(encode(t.proposal));
    w.bytes(t.signer_key);
    w.bytes(t.signature);
    return w.data();
}

Hash256 proposalId(const GovernanceProposal& proposal) {
    Writer w;
    w.text("primechain-next/governance-proposal-id");
    w.bytes(encode(proposal));
    return sha3(w.data());
}

Bytes governanceSigningPayload(const GovernanceTx& tx) {
    Writer w;
    w.text(tx.kind == GovernanceTxKind::Vote ? "primechain-next/governance-vote" : "primechain-next/governance-ready");
    w.text(tx.proposal.network_id);
    w.hash(proposalId(tx.proposal));
    return w.data();
}

bool decode(const Bytes& data, GovernanceProposal& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, GovernanceProposal& o) { return readProposal(r, o, limits); });
}

bool decode(const Bytes& data, GovernanceTx& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, GovernanceTx& o) { return readGovernance(r, o, limits); });
}

Bytes encode(const BlockHeader& header) {
    Writer w;
    writeHeader(w, header);
    return w.data();
}

Bytes encode(const Block& block) {
    Writer w;
    w.varint(kBlockTag);
    w.varint(block.version);
    writeHeader(w, block.header);
    w.varint(block.ranges.size());
    for (const auto& range : block.ranges) w.bytes(encode(range));
    w.varint(block.new_keys.size());
    for (const auto& key : block.new_keys) w.bytes(encode(key));
    w.varint(block.transfers.size());
    for (const auto& t : block.transfers) w.bytes(encode(t));
    w.varint(block.governance.size());
    for (const auto& g : block.governance) w.bytes(encode(g));
    w.u8(block.prime ? 1 : 0);
    if (block.prime) w.bytes(encode(*block.prime));
    w.varint(block.ring_commits.size());
    for (const auto& c : block.ring_commits) w.bytes(encode(c));
    w.varint(block.ring_payees.size());
    for (const auto& p : block.ring_payees) w.bytes(encode(p));
    w.varint(block.ring_reveals.size());
    for (const auto& v : block.ring_reveals) w.bytes(encode(v));
    return w.data();
}

bool decode(const Bytes& data, RangeSubmission& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, RangeSubmission& o) { return readRange(r, o, limits, true); });
}

bool decode(const Bytes& data, PrimeSubmission& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, PrimeSubmission& o) { return readPrime(r, o, limits); });
}

bool decode(const Bytes& data, AcceptedRange& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, AcceptedRange& o) { return readAccepted(r, o, limits); });
}

bool decode(const Bytes& data, Block& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, Block& b) {
        std::uint64_t count = 0;
        if (!expectTag(r, kBlockTag, b.version) || !readHeader(r, b.header, limits) || !r.varint(count) ||
            count > limits.max_list)
            return false;
        b.ranges.resize(count);
        for (auto& range : b.ranges) {
            Bytes raw;
            std::string inner;
            if (!r.bytes(raw, limits.max_list * (limits.max_integer_bytes + 2)) || !decode(raw, range, limits, inner)) return false;
        }
        std::uint64_t key_count = 0;
        if (!r.varint(key_count) || key_count > limits.max_list) return false;
        b.new_keys.resize(key_count);
        for (auto& key : b.new_keys) {
            Bytes raw;
            std::string inner;
            if (!r.bytes(raw, limits.max_key_bytes + 1024) || !decode(raw, key, limits, inner)) return false;
        }
        std::uint64_t transfer_count = 0;
        if (!r.varint(transfer_count) || transfer_count > limits.max_list) return false;
        b.transfers.resize(transfer_count);
        for (auto& t : b.transfers) {
            Bytes raw;
            std::string inner;
            if (!r.bytes(raw, limits.max_list * 64 + limits.max_key_bytes + limits.max_signature_bytes) ||
                !decode(raw, t, limits, inner))
                return false;
        }
        std::uint64_t governance_count = 0;
        if (!r.varint(governance_count) || governance_count > limits.max_list) return false;
        b.governance.resize(governance_count);
        for (auto& g : b.governance) {
            Bytes raw;
            std::string inner;
            if (!r.bytes(raw, 4 * limits.max_key_bytes + 2 * limits.max_signature_bytes + 1024) || !decode(raw, g, limits, inner))
                return false;
        }
        std::uint8_t has_prime = 0;
        if (!r.u8(has_prime) || has_prime > 1) return false;
        b.prime.reset();
        if (has_prime) {
            Bytes raw;
            std::string inner;
            AcceptedPrime prime;
            if (!r.bytes(raw, 4 * limits.max_integer_bytes + limits.max_text + 256) || !decode(raw, prime, limits, inner))
                return false;
            b.prime = prime;
        }
        auto readList = [&](auto& list, std::size_t max_bytes) {
            std::uint64_t n = 0;
            if (!r.varint(n) || n > limits.max_list) return false;
            list.resize(n);
            for (auto& item : list) {
                Bytes raw;
                std::string inner;
                if (!r.bytes(raw, max_bytes) || !decode(raw, item, limits, inner)) return false;
            }
            return true;
        };
        const std::size_t tx_bytes = 4 * limits.max_text + limits.max_key_bytes + limits.max_signature_bytes + 256;
        return readList(b.ring_commits, tx_bytes) && readList(b.ring_payees, tx_bytes) &&
               readList(b.ring_reveals, kMaxCertificateBytes + 256);
    });
}

Hash256 submissionHash(const RangeSubmission& range) {
    Writer w;
    w.text("primechain-next/range-submission-id");
    const Bytes body = encode(range);
    w.bytes(body);
    return sha3(w.data());
}

bool decode(const Bytes& data, AcceptedPrime& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, AcceptedPrime& o) { return readAcceptedPrime(r, o, limits); });
}

bool decode(const Bytes& data, KeyRecord& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, KeyRecord& o) { return readKey(r, o, limits); });
}

bool decode(const Bytes& data, Transfer& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, Transfer& o) { return readTransfer(r, o, limits); });
}

bool decode(const Bytes& data, ValidatorRecord& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, ValidatorRecord& o) { return readValidator(r, o, limits); });
}

Hash256 submissionHash(const PrimeSubmission& prime) {
    Writer w;
    w.text("primechain-next/prime-submission-id");
    const Bytes body = encode(prime);
    w.bytes(body);
    return sha3(w.data());
}

Bytes ringCommitSigningPayload(const RingCommit& commit) {
    Writer w;
    w.text("primechain-next/ring-commit");
    writeRingCommitFields(w, commit);
    return w.data();
}

Bytes ringPayeeSigningPayload(const RingPayee& payee) {
    Writer w;
    w.text("primechain-next/ring-payee");
    writeRingPayeeFields(w, payee);
    return w.data();
}

Bytes encode(const RingCommit& commit) {
    Writer w;
    writeRingCommitFields(w, commit);
    w.bytes(commit.signature);
    return w.data();
}

Bytes encode(const RingPayee& payee) {
    Writer w;
    writeRingPayeeFields(w, payee);
    w.bytes(payee.signature);
    return w.data();
}

Bytes encode(const RingReveal& v) {
    Writer w;
    w.varint(kRingRevealTag);
    w.varint(v.version);
    w.varint(v.commit_height);
    w.varint(v.commit_index);
    w.bytes(v.salt);
    w.varint(v.certificate_type);
    w.varint(v.certificate_version);
    w.bytes(v.certificate);
    return w.data();
}

bool decode(const Bytes& data, RingCommit& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, RingCommit& o) { return readRingCommit(r, o, limits); });
}

bool decode(const Bytes& data, RingReveal& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, RingReveal& o) { return readRingReveal(r, o); });
}

bool decode(const Bytes& data, RingPayee& out, const Limits& limits, std::string& error) {
    return decodeStrict(data, out, limits, error, [&](Reader& r, RingPayee& o) { return readRingPayee(r, o, limits); });
}

Hash256 certificateHash(std::uint64_t type, std::uint64_t version, const Bytes& certificate) {
    Writer w;
    w.text("primechain-next/certificate");
    w.varint(type);
    w.varint(version);
    w.bytes(certificate);
    return sha3(w.data());
}

// ring-records section 2: hash(tag || network || discoverer || salt || hash(certificate)).
Hash256 ringCommitment(const std::string& network_id, const Address& discoverer, const Bytes& salt,
                       const Hash256& certificate_hash) {
    Writer w;
    w.text("primechain-next/ring-commitment");
    w.text(network_id);
    w.text(discoverer);
    w.bytes(salt);
    w.hash(certificate_hash);
    return sha3(w.data());
}

Hash256 blockHash(const BlockHeader& header) {
    Writer w;
    w.text("primechain-next/block-header");
    const Bytes body = encode(header);
    w.bytes(body);
    return sha3(w.data());
}

std::vector<Bytes> rangesRootLeaves(const Block& block) {
    std::vector<Bytes> leaves;
    leaves.reserve(block.ranges.size() + block.new_keys.size() + 1);
    for (const auto& range : block.ranges) leaves.push_back(encode(range));
    for (const auto& key : block.new_keys) leaves.push_back(encode(key));
    if (block.prime) leaves.push_back(encode(*block.prime));
    return leaves;
}

}  // namespace primechain::next
