#include "primechain_next/model.hpp"

#include "primechain_next/merkle.hpp"

namespace primechain::next {
namespace {

enum Tag : std::uint64_t { kRangeTag = 1, kPrimeTag = 2, kAcceptedTag = 3, kHeaderTag = 4, kBlockTag = 5 };

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
}

bool expectTag(Reader& r, std::uint64_t tag, std::uint64_t& version) {
    std::uint64_t got = 0;
    if (!r.varint(got) || got != tag) return false;
    return r.varint(version) && version == kVersion1;  // unknown versions are rejected (D12)
}

bool readRange(Reader& r, RangeSubmission& out, const Limits& l, bool with_signature) {
    std::uint64_t count = 0;
    if (!expectTag(r, kRangeTag, out.version) || !r.text(out.network_id, l.max_text) || !r.hash(out.anchor) ||
        !r.integer(out.start) || !r.integer(out.end) || !r.varint(count) || count > l.max_list)
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
        !r.varint(count) || count > l.max_list)
        return false;
    out.divisors.resize(count);
    for (auto& d : out.divisors) if (!r.integer(d)) return false;
    return true;
}

bool readHeader(Reader& r, BlockHeader& h, const Limits& l) {
    return expectTag(r, kHeaderTag, h.version) && r.text(h.network_id, l.max_text) && r.varint(h.height) &&
           r.hash(h.previous_hash) && r.integer(h.prime) && r.hash(h.policy_digest) &&
           r.hash(h.validator_epoch_digest) && r.hash(h.ranges_root) && r.hash(h.transactions_root) &&
           r.hash(h.state_root) && r.hash(h.extension_root) && r.hash(h.privacy_root);
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
    w.u8(block.prime_submission ? 1 : 0);
    if (block.prime_submission) w.bytes(encode(*block.prime_submission));
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
        std::uint8_t has_prime = 0;
        if (!r.u8(has_prime) || has_prime > 1) return false;
        b.prime_submission.reset();
        if (has_prime) {
            Bytes raw;
            std::string inner;
            PrimeSubmission prime;
            if (!r.bytes(raw, limits.max_key_bytes + limits.max_signature_bytes + 1024) || !decode(raw, prime, limits, inner))
                return false;
            b.prime_submission = prime;
        }
        return true;
    });
}

Hash256 submissionHash(const RangeSubmission& range) {
    Writer w;
    w.text("primechain-next/range-submission-id");
    const Bytes body = encode(range);
    w.bytes(body);
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
    leaves.reserve(block.ranges.size() + 1);
    for (const auto& range : block.ranges) leaves.push_back(encode(range));
    if (block.prime_submission) leaves.push_back(encode(*block.prime_submission));
    return leaves;
}

}  // namespace primechain::next
