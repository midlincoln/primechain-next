#include "primechain_next/registry.hpp"

#include "primechain_next/merkle.hpp"

#include <stdexcept>

namespace primechain::next {
namespace {

// Records are written as byte strings and read with their own bound, because identities and
// units may be longer than main-chain integers (the snapshot reader's bound).
constexpr std::size_t kRecordIntegerBytes = 1u << 20;
constexpr std::size_t kMaxRecordBytes = 4u << 20;

void signedInteger(Writer& w, const Integer& v) {
    w.u8(v < 0 ? 1 : 0);
    w.integer(v < 0 ? Integer(-v) : v);
}
bool signedInteger(Reader& r, Integer& v) {
    std::uint8_t sign = 0;
    if (!r.u8(sign) || sign > 1 || !r.integer(v)) return false;
    if (sign == 1 && v == 0) return false;
    if (sign == 1) v = -v;
    return true;
}
void writeUnit(Writer& w, const std::optional<QuadElement>& u) {
    w.u8(u ? 1 : 0);
    if (u) { signedInteger(w, u->c0); signedInteger(w, u->c1); }
}
bool readUnit(Reader& r, std::optional<QuadElement>& u) {
    std::uint8_t has = 0;
    if (!r.u8(has) || has > 1) return false;
    u.reset();
    if (!has) return true;
    QuadElement e;
    if (!signedInteger(r, e.c0) || !signedInteger(r, e.c1)) return false;
    u = e;
    return true;
}
void writeTerms(Writer& w, const RingTerms& t) {
    w.amount(Amount::fromRaw(t.prime_value));
    w.amount(Amount::fromRaw(t.upstream_bp));
    w.amount(Amount::fromRaw(t.founder_bp));
    w.amount(Amount::fromRaw(t.kept_bp));
    w.amount(Amount::fromRaw(t.backlog_bp));
}
bool readTerms(Reader& r, RingTerms& t) {
    Amount a, b, c, d, e;
    if (!r.amount(a) || !r.amount(b) || !r.amount(c) || !r.amount(d) || !r.amount(e)) return false;
    t = {a.raw(), b.raw(), c.raw(), d.raw(), e.raw()};
    return true;
}

}  // namespace

Hash256 ringGenesisHash(std::uint64_t number, const Hash256& ring_id, const Hash256& certificate_hash,
                        std::uint64_t opened) {
    Writer w;
    w.text("primechain-next/ring-genesis");
    w.varint(number);
    w.hash(ring_id);
    w.hash(certificate_hash);
    w.varint(opened);
    return sha3(w.data());
}

OpenCommit* RingRegistry::commit(std::uint64_t height, std::uint64_t index) {
    const auto it = commits_.find({height, index});
    return it == commits_.end() ? nullptr : &it->second;
}

const OpenCommit* RingRegistry::commit(std::uint64_t height, std::uint64_t index) const {
    const auto it = commits_.find({height, index});
    return it == commits_.end() ? nullptr : &it->second;
}

void RingRegistry::addCommit(OpenCommit c) {
    const Key key{c.height, c.index};
    if (!commits_.emplace(key, std::move(c)).second) throw std::logic_error("registry: commit recorded twice");
}

void RingRegistry::eraseCommit(std::uint64_t height, std::uint64_t index) { commits_.erase({height, index}); }

const RingEntry* RingRegistry::ring(std::uint64_t number) const {
    const auto it = rings_.find(number);
    return it == rings_.end() ? nullptr : &it->second;
}

const RingEntry* RingRegistry::byId(const Hash256& ring_id) const {
    const auto it = by_id_.find(ring_id);
    return it == by_id_.end() ? nullptr : ring(it->second);
}

std::size_t RingRegistry::openedSince(std::uint64_t height) const {
    std::size_t n = 0;
    for (auto it = rings_.rbegin(); it != rings_.rend() && it->second.opened >= height; ++it) ++n;
    return n;
}

const RingEntry& RingRegistry::open(RingEntry entry) {
    if (entry.number != nextNumber() || by_id_.count(entry.ring_id)) throw std::logic_error("registry: bad ring opening");
    by_id_.emplace(entry.ring_id, entry.number);
    return rings_.emplace(entry.number, std::move(entry)).first->second;
}

void RingRegistry::advance(std::uint64_t number, const Integer& frontier, const Hash256& tip) {
    const auto it = rings_.find(number);
    if (it == rings_.end() || frontier <= it->second.frontier) throw std::logic_error("registry: bad ring advance");
    it->second.frontier = frontier;
    it->second.tip = tip;
}

bool RingRegistry::setPaused(std::uint64_t number, bool paused) {
    const auto it = rings_.find(number);
    if (it == rings_.end()) return false;
    it->second.paused = paused;
    return true;
}

// Same rule as AssetLedger::setPayee: the last change in a block counts.
bool RingRegistry::setPayee(std::uint64_t number, std::uint64_t height, const Address& payee, std::string& error) {
    const auto it = rings_.find(number);
    if (it == rings_.end()) { error = "unknown ring"; return false; }
    auto& payees = it->second.payees;
    if (payee.empty() || height < payees.back().first) { error = "invalid payee change"; return false; }
    if (payees.back().first == height) payees.back().second = payee;
    else payees.emplace_back(height, payee);
    return true;
}

Bytes RingRegistry::encodeEntry(const RingEntry& r) const {
    Writer w;
    w.text("primechain-next/ring-record");
    w.varint(kVersion1);
    w.varint(r.number);
    w.hash(r.ring_id);
    w.varint(r.certificate_type);
    w.varint(r.certificate_version);
    w.hash(r.certificate_hash);
    signedInteger(w, r.d);
    writeUnit(w, r.unit);
    w.text(r.founder);
    w.varint(r.payees.size());
    for (const auto& [from, a] : r.payees) { w.varint(from); w.text(a); }
    writeTerms(w, r.terms);
    w.varint(r.opened);
    w.varint(r.commit_height);
    w.varint(r.commit_index);
    w.integer(r.frontier);
    w.hash(r.tip);
    w.u8(r.paused ? 1 : 0);
    return w.data();
}

Bytes RingRegistry::encodeCommit(const OpenCommit& c) const {
    Writer w;
    w.text("primechain-next/open-commit");
    w.varint(kVersion1);
    w.varint(c.height);
    w.varint(c.index);
    w.text(c.discoverer);
    w.hash(c.commitment);
    w.integer(c.deposit_asset);
    w.amount(c.deposit);
    w.varint(c.expires_after);
    w.u8(c.revealed ? 1 : 0);
    if (c.revealed) {
        w.varint(c.revealed_in);
        w.hash(c.ring_id);
        w.varint(c.certificate_type);
        w.varint(c.certificate_version);
        w.hash(c.certificate_hash);
        signedInteger(w, c.d);
        writeUnit(w, c.unit);
    }
    return w.data();
}

std::vector<Bytes> RingRegistry::leaves() const {
    std::vector<Bytes> out;
    for (const auto& [n, r] : rings_) out.push_back(encodeEntry(r));
    for (const auto& [k, c] : commits_) out.push_back(encodeCommit(c));
    return out;
}

std::vector<Bytes> extensionLeaves(const RingRegistry& registry, const Hash256& batch_hash) {
    std::vector<Bytes> out = registry.leaves();
    if (batch_hash != Hash256{}) {
        Writer w;
        w.text("primechain-next/extension-ring-batch");
        w.hash(batch_hash);
        out.push_back(w.data());
    }
    return out;
}

Hash256 RingRegistry::digest() const {
    Writer w;
    w.text("primechain-next/ring-registry");
    w.varint(rings_.size());
    w.varint(commits_.size());
    w.hash(merkleRoot(leaves()));
    return sha3(w.data());
}

void RingRegistry::encode(Writer& w) const {
    w.text("primechain-next/ring-registry");
    w.varint(kVersion1);
    w.varint(rings_.size());
    for (const auto& [n, r] : rings_) w.bytes(encodeEntry(r));
    w.varint(commits_.size());
    for (const auto& [k, c] : commits_) w.bytes(encodeCommit(c));
}

bool RingRegistry::decode(Reader& r, std::size_t max_text, std::string& error) {
    std::string tag;
    std::uint64_t version = 0, count = 0;
    commits_.clear();
    rings_.clear();
    by_id_.clear();
    auto fail = [&](const std::string& m) { error = "ring registry: " + m; return false; };
    if (!r.text(tag, 64) || tag != "primechain-next/ring-registry" || !r.varint(version) || version != kVersion1 ||
        !r.varint(count))
        return fail("bad header");
    for (std::uint64_t i = 0; i < count; ++i) {
        Bytes raw;
        if (!r.bytes(raw, kMaxRecordBytes)) return fail("bad record");
        Reader in(raw, kRecordIntegerBytes);
        RingEntry e;
        std::uint64_t v = 0, payees = 0;
        std::uint8_t paused = 0;
        if (!in.text(tag, 64) || tag != "primechain-next/ring-record" || !in.varint(v) || v != kVersion1 ||
            !in.varint(e.number) || !in.hash(e.ring_id) || !in.varint(e.certificate_type) ||
            !in.varint(e.certificate_version) || !in.hash(e.certificate_hash) || !signedInteger(in, e.d) ||
            !readUnit(in, e.unit) || !in.text(e.founder, max_text) || !in.varint(payees) || payees == 0 || payees > (1u << 24))
            return fail("bad record");
        for (std::uint64_t j = 0; j < payees; ++j) {
            std::uint64_t from = 0;
            Address a;
            if (!in.varint(from) || !in.text(a, max_text)) return fail("bad payee");
            e.payees.emplace_back(from, a);
        }
        if (!readTerms(in, e.terms) || !in.varint(e.opened) || !in.varint(e.commit_height) || !in.varint(e.commit_index) ||
            !in.integer(e.frontier) || !in.hash(e.tip) || !in.u8(paused) || paused > 1 || !in.done())
            return fail("bad record");
        e.paused = paused == 1;
        if (e.number != nextNumber() || by_id_.count(e.ring_id)) return fail("ring numbers out of order");
        open(std::move(e));
    }
    if (!r.varint(count)) return fail("bad commit count");
    for (std::uint64_t i = 0; i < count; ++i) {
        Bytes raw;
        if (!r.bytes(raw, kMaxRecordBytes)) return fail("bad commit");
        Reader in(raw, kRecordIntegerBytes);
        OpenCommit c;
        std::uint64_t v = 0;
        std::uint8_t revealed = 0;
        if (!in.text(tag, 64) || tag != "primechain-next/open-commit" || !in.varint(v) || v != kVersion1 ||
            !in.varint(c.height) || !in.varint(c.index) || !in.text(c.discoverer, max_text) || !in.hash(c.commitment) ||
            !in.integer(c.deposit_asset) || !in.amount(c.deposit) || !in.varint(c.expires_after) || !in.u8(revealed) ||
            revealed > 1)
            return fail("bad commit");
        c.revealed = revealed == 1;
        if (c.revealed && (!in.varint(c.revealed_in) || !in.hash(c.ring_id) || !in.varint(c.certificate_type) ||
                           !in.varint(c.certificate_version) || !in.hash(c.certificate_hash) || !signedInteger(in, c.d) ||
                           !readUnit(in, c.unit)))
            return fail("bad commit");
        if (!in.done() || commits_.count({c.height, c.index})) return fail("bad commit");
        addCommit(std::move(c));
    }
    return true;
}

}  // namespace primechain::next
