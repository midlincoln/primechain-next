// Tests for B3.5: mining rings (RING_WORK, segments, ring blocks and batches, issuance under rule
// A4 with the backlog share, awards through the asset ledger, the rings file and the row index).
// Specification: docs/ring-records.md sections 3-5, docs/rings-design.md R7-R13,
// docs/protocol-parameters.md (rule A4, parameter 65). Reference model and vectors:
// experiments/ring_issuance/a4_rows.py -> a4_vectors.txt.
// Usage: primechain-next-ring-mine-tests rows|compete|issuance|persistence <a4_vectors.txt>
#include "primechain/crypto/signature.hpp"
#include "primechain_next/engine.hpp"
#include "primechain_next/merkle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace primechain::next;
namespace crypto = primechain::crypto;
namespace fs = std::filesystem;

namespace {

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// ---- test helpers (independent of the engine)
std::uint64_t smallestFactor(std::uint64_t n) {
    for (std::uint64_t k = 2; k * k <= n; ++k) if (n % k == 0) return k;
    return n;
}
bool isPrime(std::uint64_t n) { return n >= 2 && smallestFactor(n) == n; }
std::uint64_t nextPrime(std::uint64_t n) { do ++n; while (!isPrime(n)); return n; }
std::uint64_t primitiveRoot(std::uint64_t p) {
    std::vector<std::uint64_t> qs;
    for (std::uint64_t m = p - 1; m > 1;) {
        const auto q = smallestFactor(m);
        if (qs.empty() || qs.back() != q) qs.push_back(q);
        m /= q;
    }
    for (std::uint64_t a = 2;; ++a) {
        bool ok = true;
        for (auto q : qs) {
            Integer r;
            mpz_powm_ui(r.get_mpz_t(), Integer(a).get_mpz_t(), (p - 1) / q, Integer(p).get_mpz_t());
            if (r == 1) { ok = false; break; }
        }
        if (ok) return a;
    }
}
std::uint32_t pid(Param p) { return static_cast<std::uint32_t>(p); }
std::uint64_t primeIndex(std::uint64_t q) {  // the main block of prime q: 2 is block 1
    std::uint64_t n = 0;
    for (std::uint64_t p = 2; p <= q; p = nextPrime(p)) ++n;
    return n;
}

struct Miner {
    crypto::SignatureKeyPair key;
    primechain::Address address;
};
Miner newMiner() {
    std::string error;
    auto key = crypto::generateProtocolSignatureKeyPair(error);
    require(key.has_value(), "key generation: " + error);
    return {*key, crypto::addressFromProtocolPublicKey(key->public_key)};
}
Bytes sign(const Miner& m, const Bytes& payload) {
    std::string error;
    auto s = crypto::signProtocolMessage(m.key.private_key, payload, error);
    require(s.has_value(), "signing: " + error);
    return *s;
}

// One block: one range by `ranger`, the prime by `finder`, plus whatever is queued.
Block closeNext(Engine& e, const Miner& ranger, const Miner& finder) {
    std::string error;
    const std::uint64_t f = e.chainFrontier().get_ui(), p = nextPrime(f);
    RangeSubmission r;
    r.network_id = e.policy().network_id;
    r.anchor = e.tip();
    r.start = f + 1;
    r.end = p - 1;
    for (auto n = f + 1; n < p; ++n) r.divisors.emplace_back(smallestFactor(n));
    r.provider = ranger.address;
    if (!e.hasKey(ranger.address)) r.public_key = ranger.key.public_key;
    r.signature = sign(ranger, rangeSigningPayload(r));
    require(e.submitRange(r, error), "range: " + error);
    while (e.decideNextRange()) {}
    PrimeSubmission s;
    s.network_id = e.policy().network_id;
    s.anchor = e.tip();
    s.prime = p;
    s.witness = primitiveRoot(p);
    s.provider = finder.address;
    if (!e.hasKey(finder.address)) s.public_key = finder.key.public_key;
    s.signature = sign(finder, primeSigningPayload(s));
    auto b = e.closeBlock(s, error);
    require(b.has_value(), "block: " + error);
    return *b;
}

// ---- the vectors of a4_rows.py
struct CertSpec { std::string factors, gens, unit; };
struct RingSpec { std::uint64_t k; long d; std::uint64_t commit; std::string who; long asset; };
struct WorkSpec { std::uint64_t h, k; std::string miner; long start; std::size_t n; };
struct Vectors {
    std::map<std::string, long> params;
    std::map<long, CertSpec> certs;
    std::map<std::pair<long, long>, QuadElement> gens;
    std::vector<RingSpec> rings;
    std::map<std::uint64_t, std::pair<std::string, std::string>> blocks;  // h -> (ranger, finder)
    std::vector<WorkSpec> works, accepts;
    std::map<std::uint64_t, long> frontier;
    std::map<std::uint64_t, std::vector<std::uint64_t>> issued;  // k -> rows, paid, backlog, total
    std::map<std::string, std::uint64_t> due;
    std::size_t final_lines{0};
    std::string final_digest;
    std::uint64_t final_total{0};
};

Vectors load(const std::string& path) {
    std::ifstream in(path);
    require(in.good(), "cannot open " + path);
    Vectors v;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "PARAM") { std::string n; long x; s >> n >> x; v.params[n] = x; }
        else if (tag == "CERT") { long d; CertSpec c; s >> d >> c.factors >> c.gens >> c.unit; v.certs[d] = c; }
        else if (tag == "RING") { RingSpec r; s >> r.k >> r.d >> r.commit >> r.who >> r.asset; v.rings.push_back(r); }
        else if (tag == "GEN") { long d, q; std::string c0, c1; s >> d >> q >> c0 >> c1; v.gens[{d, q}] = {Integer(c0), Integer(c1)}; }
        else if (tag == "BLOCK") { std::uint64_t h; std::string a, b; s >> h >> a >> b; v.blocks[h] = {a, b}; }
        else if (tag == "WORK" || tag == "ACCEPT") {
            WorkSpec w;
            s >> w.h >> w.k >> w.miner >> w.start >> w.n;
            (tag == "WORK" ? v.works : v.accepts).push_back(w);
        }
        else if (tag == "FRONTIER") { std::uint64_t k; long q; s >> k >> q; v.frontier[k] = q; }
        else if (tag == "ISSUED") { std::uint64_t k, a, b, c, d; s >> k >> a >> b >> c >> d; v.issued[k] = {a, b, c, d}; }
        else if (tag == "DUE") { std::string a; std::uint64_t x; s >> a >> x; v.due[a] = x; }
        else if (tag == "FINAL") s >> v.final_lines >> v.final_digest >> v.final_total;
    }
    require(!v.blocks.empty() && !v.rings.empty() && !v.final_digest.empty(), "vectors incomplete: " + path);
    return v;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

Bytes certificate(const Vectors& v, long d) {
    const CertSpec& c = v.certs.at(d);
    QuadCertificate cert;
    cert.d = Integer(d);
    for (const auto& f : split(c.factors, '*')) {
        const auto pe = split(f, '^');
        cert.factors.emplace_back(Integer(pe[0]), std::stoul(pe[1]));
    }
    if (c.gens != "-")
        for (const auto& g : split(c.gens, ',')) {
            const auto x = split(g, ':');  // q:r:c0:c1
            cert.generators.push_back({Integer(x[0]), {Integer(x[2]), Integer(x[3])}});
        }
    if (c.unit != "-") {
        const auto x = split(c.unit, ':');
        cert.unit = QuadElement{Integer(x[0]), Integer(x[1])};
    }
    return encode(cert);
}

std::optional<QuadElement> unitOf(const Vectors& v, long d) {
    const CertSpec& c = v.certs.at(d);
    if (c.unit == "-") return std::nullopt;
    const auto x = split(c.unit, ':');
    return QuadElement{Integer(x[0]), Integer(x[1])};
}

// R7 on the test side: the rows of the segment starting at `start` (mpz_kronecker for the types).
std::vector<std::uint64_t> segmentRows(long d, std::uint64_t start) {
    std::vector<std::uint64_t> rows;
    std::uint64_t paid = 0;
    for (std::uint64_t q = start;; q = nextPrime(q)) {
        rows.push_back(q);
        const int k = mpz_kronecker(Integer(d).get_mpz_t(), Integer(q).get_mpz_t());
        if (k != -1) {
            if (++paid == 32) return rows;
        } else if (paid > 0 || rows.size() == 1024) {
            return rows;
        }
    }
}

RowType typeOf(long d, std::uint64_t q) {
    const int k = mpz_kronecker(Integer(d).get_mpz_t(), Integer(q).get_mpz_t());
    return k == 0 ? RowType::Ramified : (k > 0 ? RowType::Split : RowType::Inert);
}

Segment makeSegment(const Vectors& v, long d, std::uint64_t start) {
    Segment s;
    s.ring_id = quadraticRingId(Integer(d));
    s.start = start;
    for (const auto q : segmentRows(d, start)) {
        s.rows.push_back(typeOf(d, q));
        if (s.rows.back() != RowType::Inert) s.generators.push_back(v.gens.at({d, static_cast<long>(q)}));
    }
    return s;
}

// n consecutive segments from `start`.
std::vector<Segment> makeSegments(const Vectors& v, long d, std::uint64_t start, std::size_t n) {
    std::vector<Segment> out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(makeSegment(v, d, start));
        start = nextPrime(segmentRows(d, start).back());
    }
    return out;
}

std::uint64_t lastOf(long d, const Segment& s) { return segmentRows(d, s.start.get_ui()).back(); }

RingWork makeWork(const Engine& e, const Miner& m, long d, std::vector<Segment> segments, bool with_key = true) {
    RingWork w;
    w.network_id = e.policy().network_id;
    w.ring_id = quadraticRingId(Integer(d));
    w.miner = m.address;
    if (with_key || !e.hasKey(m.address)) w.public_key = m.key.public_key;
    w.start = segments.front().start;
    w.segments = std::move(segments);
    w.signature = sign(m, ringWorkSigningPayload(w));
    return w;
}

void resign(RingWork& w, const Miner& m) { w.signature = sign(m, ringWorkSigningPayload(w)); }

// A transfer of one asset (ring, q, i) with the fee in the same asset; version 2 when it is a ring prime.
Transfer makeTransfer(const Engine& e, const Miner& from, const AssetId& x, const Amount& amount,
                      const primechain::Address& to, std::uint64_t version = 0) {
    Transfer t;
    t.version = version ? version : (x.isMain() ? kVersion1 : kTransferVersionRings);
    t.network_id = e.policy().network_id;
    t.sender = from.address;
    t.inputs = {{x.q, *amount.add(Amount(1)), x.ring, x.i}};
    t.outputs = {{x.q, amount, to, x.ring, x.i}};
    t.fee_asset = x.q;
    t.fee_ring = x.ring;
    t.fee_index = x.i;
    t.fee = Amount(1);
    t.nonce = e.nonce(from.address) + 1;
    if (!e.hasKey(from.address)) t.public_key = from.key.public_key;
    t.signature = sign(from, transferSigningPayload(t));
    return t;
}

// ---- opening rings (B3.4)
Bytes salt(std::uint8_t tag) { return Bytes(kRingSaltBytes, tag); }

RingCommit makeCommit(const Engine& e, const Miner& m, const Bytes& s, const Bytes& cert, const Integer& asset) {
    RingCommit c;
    c.network_id = e.policy().network_id;
    c.discoverer = m.address;
    c.commitment = ringCommitment(c.network_id, m.address, s, certificateHash(1, 1, cert));
    c.deposit_asset = asset;
    c.deposit = Amount::fromRaw(e.parameter(Param::RingDeposit));
    c.nonce = e.nonce(m.address) + 1;
    if (!e.hasKey(m.address)) c.public_key = m.key.public_key;
    c.signature = sign(m, ringCommitSigningPayload(c));
    return c;
}
RingReveal makeReveal(std::uint64_t height, std::uint64_t index, const Bytes& s, const Bytes& cert) {
    RingReveal v;
    v.commit_height = height;
    v.commit_index = index;
    v.salt = s;
    v.certificate = cert;
    return v;
}

Integer fundedAsset(const Engine& e, const Miner& m) {
    const Amount need = *Amount::fromRaw(e.parameter(Param::RingDeposit)).add(Amount::fromRaw(e.parameter(Param::MinFee)));
    for (const auto& x : e.assets().assets())
        if (x.isMain() && !(e.balance(m.address, x.q) < need)) return x.q;
    throw std::runtime_error("no funded asset");
}

// Commit and reveal a ring in two blocks; returns its number.
std::uint64_t openRing(Engine& e, const Miner& founder, const Miner& closer, const Bytes& cert, std::uint8_t tag) {
    std::string error;
    const Bytes s = salt(tag);
    require(e.submitRingCommit(makeCommit(e, founder, s, cert, fundedAsset(e, founder)), error), "commit: " + error);
    const Block b = closeNext(e, closer, closer);
    require(e.submitRingReveal(makeReveal(b.header.height, b.ring_commits.size() - 1, s, cert), error), "reveal: " + error);
    closeNext(e, closer, closer);
    const std::uint64_t n = e.rings().rings().rbegin()->first;
    require(e.rings().ring(n)->opened == e.height(), "ring opened");
    return n;
}

// Build, check and apply the proposer's batch.
Engine::RingBatchValue mineBatch(Engine& e) {
    std::string error;
    const auto v = e.buildRingBatch();
    require(v.has_value(), "a ring batch to propose");
    require(e.checkRingBatch(*v, error), "batch checks: " + error);
    require(e.applyDecidedRingBatch(*v, error), "batch applies: " + error);
    return *v;
}

// A value whose application must fail, leaving the engine untouched.
void refused(Engine& e, const Engine::RingBatchValue& v, const std::string& expect, const std::string& what) {
    std::string error;
    const Hash256 before = merkleRoot(e.rings().leaves());
    const Hash256 ledger = e.assets().digest();
    require(!e.checkRingBatch(v, error), what + ": refused by checkRingBatch");
    require(error.find(expect) != std::string::npos, what + ": expected '" + expect + "', got '" + error + "'");
    error.clear();
    require(!e.applyDecidedRingBatch(v, error), what + ": refused when applied");
    require(error.find(expect) != std::string::npos, what + ": expected '" + expect + "', got '" + error + "'");
    require(merkleRoot(e.rings().leaves()) == before && e.assets().digest() == ledger, what + ": state unchanged");
}

void refusedWork(Engine& e, const RingWork& w, const std::string& expect, const std::string& what) {
    std::string error;
    require(!e.submitRingWork(w, error), what + ": work refused");
    require(error.find(expect) != std::string::npos, what + ": expected '" + expect + "', got '" + error + "'");
}

// B3.6: the ring records, the open commits, then the hash of the block's ring batch (if any).
Hash256 extensionRootOf(const Engine& e, const Block& block) {
    Hash256 batch{};
    for (const auto& b : e.ringBatches())
        if (b.open_height == block.header.height) batch = ringBatchHash(b);
    return merkleRoot(extensionLeaves(e.rings(), batch));
}

// The value of a single hand-made group (to test refusals beyond what the proposer would build).
Engine::RingBatchValue handValue(const Engine& e, std::uint64_t ring, const RingWork& w, std::size_t from, std::size_t n) {
    Engine::RingBatchValue v;
    v.batch.network_id = e.policy().network_id;
    v.batch.open_height = e.height() + 1;
    v.batch.previous_main = e.tip();
    RingBlock b;
    b.ring = ring;
    b.previous = e.rings().ring(ring)->tip;
    b.start = w.segments[from].start;
    RingGroup g;
    g.miner = w.miner;
    g.work_hash = ringWorkHash(w);
    g.segments.assign(w.segments.begin() + static_cast<std::ptrdiff_t>(from),
                      w.segments.begin() + static_cast<std::ptrdiff_t>(from + n));
    b.groups.push_back(g);
    v.batch.blocks.push_back(b);
    v.works.push_back(w);
    return v;
}

struct Val { Miner consensus, payout; ValidatorRecord record; };
Val newVal(const std::string& network_id) {
    Val v{newMiner(), newMiner(), {}};
    v.record.consensus_key = v.consensus.key.public_key;
    v.record.payout_address = v.payout.address;
    v.record.payout_key = v.payout.key.public_key;
    const Bytes payload = validatorBindingPayload(network_id, v.record);
    v.record.consensus_signature = sign(v.consensus, payload);
    v.record.payout_signature = sign(v.payout, payload);
    return v;
}

// ---------------------------------------------------------------- rows: valid and invalid
void rowTests(const Vectors& vec) {
    std::string error;
    const Miner A = newMiner(), B = newMiner(), D = newMiner(), E = newMiner();
    Policy policy;
    policy.governance_notice = 3;
    policy.parameters[pid(Param::GeneratorBytes)] = 1;  // [65] tiny, so Q(sqrt 97) hits it at q = 43
    const Val val = newVal(policy.network_id);
    Genesis genesis;
    genesis.validators.push_back(val.record);
    Engine e(policy, genesis);
    for (int k = 0; e.height() < 20; ++k) closeNext(e, k % 2 ? A : B, k % 2 ? B : A);
    const std::uint64_t g4 = openRing(e, A, B, certificate(vec, -4), 1);   // Q(i)
    const std::uint64_t r97 = openRing(e, B, A, certificate(vec, 97), 2);  // Q(sqrt 97)
    while (e.height() < 60) closeNext(e, A, B);
    require(g4 == 1 && r97 == 2, "two rings open");

    // 1. Valid: three segments of Q(i) from 2 by D (no main-chain key: the work carries it).
    const auto segs = makeSegments(vec, -4, 2, 3);
    const RingWork w1 = makeWork(e, D, -4, segs);
    require(e.submitRingWork(w1, error), "valid work: " + error);
    refusedWork(e, w1, "already queued", "the same work twice");
    const Hash256 tip0 = e.rings().ring(g4)->tip;
    const auto v1 = mineBatch(e);
    require(v1.batch.blocks.size() == 1 && v1.batch.blocks[0].groups.size() == 1 &&
                v1.batch.blocks[0].groups[0].miner == D.address && v1.batch.blocks[0].groups[0].segments.size() == 3 &&
                v1.batch.blocks[0].previous == tip0 && v1.batch.blocks[0].start == 2 && v1.works.size() == 1,
            "the batch: one ring block, one group of three segments, linked to the genesis tip");
    const std::uint64_t h1 = e.height() + 1, last1 = lastOf(-4, segs.back());
    const RingEntry& r1 = *e.rings().ring(g4);
    require(r1.frontier == last1 && r1.tip == ringBlockHash(v1.batch.blocks[0]), "frontier and tip advance");
    std::size_t rows = 0, paid = 0;
    Amount miner_due;
    for (std::uint64_t q = 2; q <= last1; q = nextPrime(q), ++rows) {
        const auto row = e.assets().row(g4, Integer(q));
        require(row && row->height == h1 && row->miner == D.address, "row recorded for " + std::to_string(q));
        const RowType t = typeOf(-4, q);
        require(row->primes.size() == (t == RowType::Split ? 2u : t == RowType::Ramified ? 1u : 0u), "row shape");
        if (t == RowType::Inert) continue;
        ++paid;
        for (std::uint32_t i = 0; i < row->primes.size(); ++i) {
            const AssetId x{g4, Integer(q), i};
            const u128 nominal = (t == RowType::Ramified ? 2 : 1) * 100000;
            const bool older = primeIndex(q) < e.rings().ring(g4)->opened;  // the backlog share (parameter 19)
            const u128 expect = nominal * (older ? 1000 : 10000) / 10000;
            require(e.assets().issued(x).raw() == expect, "A4 issue of " + x.str());
            miner_due = *miner_due.add(e.assets().split(x)->miner);
        }
    }
    require(e.rings().ring(g4)->opened == 22, "ring 1 opened at 22 (primes to 73 are older)");
    require(e.assets().owned(AssetId{g4, Integer(5), 0}, D.address) == e.assets().split(AssetId{g4, Integer(5), 0})->miner,
            "the miner is credited at once");
    require(e.pendingRingWorkCount() == 0, "the work is used up");
    require(!e.buildRingBatch(), "one ring batch per main block");
    {
        RingWork more = makeWork(e, E, -4, makeSegments(vec, -4, nextPrime(last1), 1));
        require(e.submitRingWork(more, error), "next work: " + error);
        refused(e, handValue(e, g4, more, 0, 1), "one ring batch per main block", "a second batch in the same block");
    }
    const Block b1 = closeNext(e, A, B);
    require(b1.header.height == h1 && b1.header.extension_root == extensionRootOf(e, b1),
            "extension_root covers the ring records with their new tips");
    std::cout << "  valid: " << rows << " rows (" << paid << " paid) of ring 1 in block " << h1 << '\n';

    // 2. Wrong rows, refused at submission (the work's signature is valid each time).
    const std::uint64_t next1 = nextPrime(e.rings().ring(g4)->frontier.get_ui());
    {
        auto bad = makeSegments(vec, -4, next1, 2);
        // the generator of another prime
        std::size_t paid_at = 0;
        while (bad[0].rows[paid_at] == RowType::Inert) ++paid_at;
        auto s = bad;
        s[0].generators[0] = s[0].generators.size() > 1 ? s[0].generators[1] : bad[1].generators[0];
        refusedWork(e, makeWork(e, D, -4, s), "norm", "wrong generator (another prime's)");
        s = bad;
        s[0].generators[0].c0 += 1;
        refusedWork(e, makeWork(e, D, -4, s), "norm", "wrong norm");
        s = bad;
        s[0].generators[0] = quadConjugate(Integer(-4), s[0].generators[0]);
        refusedWork(e, makeWork(e, D, -4, s), "smaller root", "wrong root order (the conjugate)");
        s = bad;
        s[0].rows[paid_at] = s[0].rows[paid_at] == RowType::Split ? RowType::Inert : RowType::Split;
        refusedWork(e, makeWork(e, D, -4, s), "wrong row type", "wrong row type");
        s = bad;
        s[0].rows.pop_back();
        refusedWork(e, makeWork(e, D, -4, s), "R7", "segment ends too early");
        s = bad;
        s[0].rows.push_back(typeOf(-4, nextPrime(lastOf(-4, bad[0]))));
        if (s[0].rows.back() != RowType::Inert) s[0].generators.push_back(bad[1].generators[0]);
        refusedWork(e, makeWork(e, D, -4, s), "R7", "segment runs past its end");
        s = {bad[0], bad[0]};
        refusedWork(e, makeWork(e, D, -4, s), "not consecutive", "a gap between the segments");
        s = bad;
        s[1].start = nextPrime(s[1].start.get_ui());
        refusedWork(e, makeWork(e, D, -4, s), "not consecutive", "a segment starting one prime late");
        // a work signed by another key than the miner it names
        RingWork forged = makeWork(e, D, -4, bad);
        forged.miner = E.address;
        refusedWork(e, forged, "address does not match", "work claims another miner");
        forged.public_key.clear();
        resign(forged, D);
        refusedWork(e, forged, "unknown sender", "work without the key of an unregistered miner");
        RingWork other = makeWork(e, D, -4, bad);
        other.segments[1].ring_id = quadraticRingId(Integer(-7));
        resign(other, D);
        refusedWork(e, other, "another ring", "a segment of another ring");
        RingWork nowhere = makeWork(e, D, -7, makeSegments(vec, -4, next1, 1));
        refusedWork(e, nowhere, "unknown ring", "work for a ring that is not open");
        const auto beyond = makeSegments(vec, -4, nextPrime(e.chainFrontier().get_ui()), 1);
        RingWork past = makeWork(e, D, -4, beyond);
        refusedWork(e, past, "segment does not start", "segments above the chain frontier");
        refusedWork(e, makeWork(e, D, -4, segs), "already recorded", "segments already recorded");
    }
    // 4. Generator size (parameter 65, 1 byte here): Q(sqrt 97) has 293 at q = 43.
    std::vector<Segment> s97;
    {
        std::uint64_t q = 2;
        while (true) {
            s97.push_back(makeSegment(vec, 97, q));
            const auto rows97 = segmentRows(97, q);
            if (std::find(rows97.begin(), rows97.end(), 43) != rows97.end()) break;
            q = nextPrime(rows97.back());
        }
        require(s97.size() >= 2, "Q(sqrt 97): small segments before the one with 43");
        RingWork small = makeWork(e, E, 97, {s97.begin(), s97.end() - 1});
        require(e.submitRingWork(small, error), "generators of one byte pass: " + error);
        refusedWork(e, makeWork(e, E, 97, s97), "parameter 65", "a generator coefficient over the limit");
        mineBatch(e);
        closeNext(e, A, B);
        RingWork big = makeWork(e, E, 97, {s97.back()});
        refusedWork(e, big, "parameter 65", "over the limit at the ring's next prime");
        // A vote raises the limit to 2 bytes; it takes effect after the notice, never before.
        GovernanceTx t;
        t.kind = GovernanceTxKind::Vote;
        t.proposal.network_id = policy.network_id;
        t.proposal.kind = GovernanceKind::ParamChange;
        t.proposal.changes = {{pid(Param::GeneratorBytes), 1, 2}};
        t.proposal.expiry_height = e.height() + 20;
        t.proposal.sequence = 1;
        t.proposal.validator_set_digest = e.validatorSetDigest();
        t.signer_key = val.consensus.key.public_key;
        t.signature = sign(val.consensus, governanceSigningPayload(t));
        require(e.submitGovernance(t, error), "vote: " + error);
        const std::uint64_t voted = closeNext(e, A, B).header.height;
        std::size_t waited = 0;
        while (e.parameter(Param::GeneratorBytes) == 1) {
            refusedWork(e, big, "parameter 65", "still refused during the notice (block " + std::to_string(e.height() + 1) + ")");
            closeNext(e, A, B);
            require(++waited < 10, "the vote takes effect");
        }
        require(e.height() + 1 == voted + policy.governance_notice && waited >= 2, "in force after the notice, not before");
        require(e.submitRingWork(big, error), "accepted once the new limit is in force: " + error);
        const auto v = mineBatch(e);
        require(v.batch.blocks.size() == 1 && v.batch.blocks[0].ring == r97, "the large generator recorded");
        require(e.assets().row(r97, Integer(43)).has_value(), "row 43 of Q(sqrt 97)");
        closeNext(e, A, B);
        // Never retroactive: the chain replays with the limit in force at each block.
        auto again = Engine::replay(e.policy(), genesis, e.blocks(), e.ringBatches(), error);
        require(again && again->tip() == e.tip(), "replay with the limit changing midway: " + error);
    }
    // 5. Wrong miner credit, wrong links, wrong roots: hand-made values against the next segment.
    {
        const std::uint64_t start = nextPrime(e.rings().ring(g4)->frontier.get_ui());
        RingWork w = makeWork(e, D, -4, makeSegments(vec, -4, start, 2));
        require(e.submitRingWork(w, error), "work: " + error);
        const auto good = handValue(e, g4, w, 0, 2);
        std::string why;
        require(e.checkRingBatch(good, why), "the hand-made value is valid: " + why);
        auto v = good;
        v.batch.blocks[0].groups[0].miner = E.address;
        refused(e, v, "wrong miner credit", "credit to another miner");
        v = good;
        v.batch.blocks[0].groups[0].work_hash[0] ^= 1;
        refused(e, v, "not attached", "a group naming another work");
        v = good;
        v.works.push_back(makeWork(e, E, -4, makeSegments(vec, -4, start, 1)));
        refused(e, v, "no group names", "an extra work attached");
        v = good;
        v.batch.blocks[0].previous[0] ^= 1;
        refused(e, v, "wrong previous ring block hash", "wrong previous ring block hash");
        v = good;
        v.batch.blocks[0].start = nextPrime(start);
        refused(e, v, "segments are not in the work", "wrong start prime (group)");
        v = good;
        v.batch.blocks[0].start = nextPrime(start);
        v.works.clear();
        v.batch.blocks[0].groups.clear();
        refused(e, v, "", "wrong start prime with no group");
        v = good;
        v.batch.blocks[0].groups[0].segments[0].generators[0].c1 += 1;
        refused(e, v, "differ from the signed work", "a segment changed after signing");
        v = good;
        v.batch.open_height += 1;
        refused(e, v, "next main block", "wrong open height");
        v = good;
        v.batch.previous_main[0] ^= 1;
        refused(e, v, "main block h - 1", "wrong main block hash");
        v = good;
        v.batch.blocks.push_back(v.batch.blocks[0]);
        refused(e, v, "out of order", "two ring blocks for one ring");
        // the same as a raw ring batch (the replay path): wrong start prime, wrong previous hash
        {
            RingBatch raw = good.batch;
            raw.blocks[0].start = nextPrime(start);
            auto copy = Engine::replay(e.policy(), genesis, e.blocks(), e.ringBatches(), why);
            require(copy.has_value(), "replay: " + why);
            std::vector<RingBatch> batches = e.ringBatches();
            batches.push_back(raw);
            require(!Engine::replay(e.policy(), genesis, e.blocks(), batches, why) && why.find("wrong start prime") != std::string::npos,
                    "a ring batch with the wrong start prime fails replay: " + why);
            raw = good.batch;
            raw.blocks[0].previous[0] ^= 1;
            batches.back() = raw;
            require(!Engine::replay(e.policy(), genesis, e.blocks(), batches, why) &&
                        why.find("wrong previous ring block hash") != std::string::npos,
                    "a ring batch with the wrong previous hash fails replay: " + why);
        }
        require(e.checkRingBatch(good, why), "still valid after the refusals: " + why);
        require(e.applyDecidedRingBatch(good, why), "apply: " + why);
        const Block b = closeNext(e, A, B);
        require(b.header.extension_root == extensionRootOf(e, b), "extension root");
        // Wrong extension_root: a block with a forged root does not replay; nor does the chain
        // without the ring batch, or with its credit changed.
        std::vector<Block> blocks = e.blocks();
        blocks.back().header.extension_root[0] ^= 1;
        require(!Engine::replay(e.policy(), genesis, blocks, e.ringBatches(), why) &&
                    why.find("does not match its replay") != std::string::npos, "a forged extension_root is refused: " + why);
        std::vector<RingBatch> batches = e.ringBatches();
        batches.pop_back();
        require(!Engine::replay(e.policy(), genesis, e.blocks(), batches, why), "the chain without its last ring batch is refused");
        batches = e.ringBatches();
        batches.back().blocks[0].groups[0].miner = E.address;
        require(!Engine::replay(e.policy(), genesis, e.blocks(), batches, why) &&
                    why.find("does not match its replay") != std::string::npos,
                "a ring batch crediting another miner does not match the main block's extension_root: " + why);
        auto ok = Engine::replay(e.policy(), genesis, e.blocks(), e.ringBatches(), why);
        require(ok && ok->tip() == e.tip() && ok->assets() == e.assets() && ok->rings() == e.rings(),
                "the two files replay: " + why);
    }
    // 6. Limits: segments per signature, message size, batch size.
    {
        Policy p = policy;
        p.parameters[pid(Param::GeneratorBytes)] = 4096;
        p.parameters[pid(Param::SegmentsPerSignature)] = 2;
        p.parameters[pid(Param::MiningMessageBytes)] = 4000;
        p.parameters[pid(Param::RingBatchBytes)] = 1024;
        Engine f(p, genesis);
        for (int k = 0; f.height() < 20; ++k) closeNext(f, A, B);
        openRing(f, A, B, certificate(vec, -4), 1);
        openRing(f, B, A, certificate(vec, -7), 2);
        openRing(f, A, B, certificate(vec, 97), 3);  // padding is tested under a 4,096-byte limit
        // Padded generators (R10) in Q(sqrt 97): g * eps and g / eps.
        {
            const auto good = makeSegments(vec, 97, 2, 1);
            const QuadElement eps = *unitOf(vec, 97);
            auto s = good;
            s[0].generators[0] = quadMul(Integer(97), s[0].generators[0], eps);
            refusedWork(f, makeWork(f, A, 97, s, false), "padded", "generator times the unit");
            const Integer n = quadNorm(Integer(97), eps);
            QuadElement inv = quadConjugate(Integer(97), eps);
            inv.c0 *= n;
            inv.c1 *= n;
            s = good;
            s[0].generators[0] = quadMul(Integer(97), s[0].generators[0], inv);
            refusedWork(f, makeWork(f, A, 97, s, false), "padded", "generator divided by the unit");
        }
        while (f.height() < 200) closeNext(f, A, B);
        refusedWork(f, makeWork(f, A, -4, makeSegments(vec, -4, 2, 3), false), "segments per signature", "three segments, limit 2");
        refusedWork(f, makeWork(f, D, -4, makeSegments(vec, -4, 2, 1)), "size limit", "a work over the message limit (key included)");
        require(f.submitRingWork(makeWork(f, A, -4, makeSegments(vec, -4, 2, 2), false), error), "two segments fit: " + error);
        // Fill both rings with many works; the proposer stops at 1,024 bytes, and a value over the
        // limit is refused.
        std::vector<RingWork> works4{makeWork(f, A, -4, makeSegments(vec, -4, 2, 2), false)};
        std::uint64_t q4 = nextPrime(lastOf(-4, works4[0].segments.back())), q7 = 2;
        for (int i = 0; i < 12; ++i) {
            auto s4 = makeSegments(vec, -4, q4, 2), s7 = makeSegments(vec, -7, q7, 2);
            works4.push_back(makeWork(f, A, -4, s4, false));
            require(f.submitRingWork(works4.back(), error), "work -4: " + error);
            require(f.submitRingWork(makeWork(f, B, -7, s7, false), error), "work -7: " + error);
            q4 = nextPrime(lastOf(-4, s4.back()));
            q7 = nextPrime(lastOf(-7, s7.back()));
        }
        Engine::RingBatchValue all;
        all.batch.network_id = f.policy().network_id;
        all.batch.open_height = f.height() + 1;
        all.batch.previous_main = f.tip();
        RingBlock b;
        b.ring = 1;
        b.previous = f.rings().ring(1)->tip;
        b.start = 2;
        for (const auto& w : works4) b.groups.push_back({w.miner, ringWorkHash(w), w.segments});
        all.batch.blocks.push_back(b);
        all.works = works4;
        require(encode(all.batch).size() > 1024, "a hand-made value over 1,024 bytes");
        refused(f, all, "size limit", "a batch over the ring batch size limit");
        const auto v = f.buildRingBatch();
        require(v && encode(v->batch).size() <= 1024, "the proposer keeps the batch within its limit");
        require(f.checkRingBatch(*v, error) && f.applyDecidedRingBatch(*v, error), "apply: " + error);
        std::cout << "  batch within 1,024 bytes: " << encode(v->batch).size() << " bytes, " << v->batch.blocks.size()
                  << " ring block(s), " << v->batch.blocks[0].groups.size() << " groups; the hand-made one "
                  << encode(all.batch).size() << " bytes\n";
        closeNext(f, A, B);
        require(f.pendingRingWorkCount() > 0, "the rest waits for the next batches");
        std::size_t more = 0;
        while (f.pendingRingWorkCount() > 0 && more < 20) {
            mineBatch(f);
            closeNext(f, A, B);
            ++more;
        }
        require(f.pendingRingWorkCount() == 0, "everything recorded over " + std::to_string(more + 1) + " batches");
        auto again = Engine::replay(f.policy(), genesis, f.blocks(), f.ringBatches(), error);
        require(again && again->tip() == f.tip(), "replay: " + error);
    }
    std::cout << "ring rows: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- competing segments (R7, D4)
void competeTests(const Vectors& vec) {
    std::string error;
    const Miner A = newMiner(), B = newMiner(), D = newMiner(), E = newMiner(), F = newMiner();
    Engine e;
    for (int k = 0; e.height() < 20; ++k) closeNext(e, A, B);
    const std::uint64_t g4 = openRing(e, A, B, certificate(vec, -4), 1);
    const std::uint64_t g7 = openRing(e, B, A, certificate(vec, -7), 2);
    while (e.height() < 120) closeNext(e, A, B);
    const auto s = makeSegments(vec, -4, 2, 8);
    // 1. Same start: D sends 2 segments first, E 3; E reaches further and wins all three.
    require(e.submitRingWork(makeWork(e, D, -4, {s[0], s[1]}), error), error);
    require(e.submitRingWork(makeWork(e, E, -4, {s[0], s[1], s[2]}), error), error);
    auto v = mineBatch(e);
    require(v.batch.blocks[0].groups.size() == 1 && v.batch.blocks[0].groups[0].miner == E.address &&
                v.batch.blocks[0].groups[0].segments.size() == 3, "the work reaching furthest wins (D4)");
    require(e.pendingRingWorkCount() == 0, "the shorter work is stale and dropped");
    closeNext(e, A, B);
    // 2. Identical segments from two miners: the earlier arrival; a batch naming the copy is also
    //    valid (the proposer's choice plus the vote decides, R7).
    const RingWork orig = makeWork(e, D, -4, {s[3]}), copy = makeWork(e, F, -4, {s[3]});
    require(e.submitRingWork(orig, error) && e.submitRingWork(copy, error), error);
    const auto proposal = e.buildRingBatch();
    require(proposal && proposal->batch.blocks[0].groups[0].miner == D.address, "a tie goes to the earlier arrival");
    auto alternative = handValue(e, g4, copy, 0, 1);
    require(e.checkRingBatch(alternative, error), "naming the copy is valid too: " + error);
    require(e.applyDecidedRingBatch(*proposal, error), error);
    require(e.assets().row(g4, s[3].start)->miner == D.address, "the decided segment credits D");
    require(e.pendingRingWorkCount() == 0, "the copy is stale");
    refused(e, alternative, "", "the copy after the decision");
    closeNext(e, A, B);
    // 3. Tails: D has segments 5-6, E has 6-8: D gets 5-6, E the tail 7-8 (each segment on its own).
    //    A work further ahead (F, segment 10) waits for the gap; ring 2 is mined in the same batch.
    const auto s7 = makeSegments(vec, -7, 2, 2);
    const auto far = makeSegments(vec, -4, nextPrime(lastOf(-4, s[7])), 3);
    require(e.submitRingWork(makeWork(e, D, -4, {s[4], s[5]}), error), error);
    require(e.submitRingWork(makeWork(e, E, -4, {s[5], s[6], s[7]}), error), error);
    require(e.submitRingWork(makeWork(e, F, -4, {far[1], far[2]}), error), "a work beyond a gap is queued: " + error);
    require(e.submitRingWork(makeWork(e, A, -7, s7, false), error), error);
    v = mineBatch(e);
    require(v.batch.blocks.size() == 2 && v.batch.blocks[0].ring == g4 && v.batch.blocks[1].ring == g7,
            "one ring block per ring, by number");
    const auto& gs = v.batch.blocks[0].groups;
    require(gs.size() == 2 && gs[0].miner == D.address && gs[0].segments.size() == 2 && gs[1].miner == E.address &&
                gs[1].segments.size() == 2 && gs[1].segments[0].rows == s[6].rows, "D gets 5-6, E the tail 7-8");
    require(e.rings().ring(g4)->frontier == lastOf(-4, s[7]), "ring 1 complete to the end of segment 8");
    require(e.assets().row(g4, s[5].start)->miner == D.address && e.assets().row(g4, s[6].start)->miner == E.address,
            "each segment credits its own miner");
    require(e.pendingRingWorkCount() == 1, "the work beyond the gap waits");
    closeNext(e, A, B);
    // 4. The gap is filled: the waiting work's segments follow in the next batch.
    require(e.submitRingWork(makeWork(e, A, -4, {far[0]}, false), error), error);
    v = mineBatch(e);
    require(v.batch.blocks[0].groups.size() == 2 && v.batch.blocks[0].groups[0].miner == A.address &&
                v.batch.blocks[0].groups[1].miner == F.address, "the gap, then the waiting work");
    require(e.pendingRingWorkCount() == 0, "nothing left");
    closeNext(e, A, B);
    // 5. Holders at the end of block h - 1 receive the upstream share (R11, R13): A sells part of a
    //    main-chain prime q just before the row of q in Q(sqrt -7) is decided.
    {
        const Miner G = newMiner();
        std::uint64_t q = nextPrime(e.rings().ring(g7)->frontier.get_ui());
        while (typeOf(-7, q) != RowType::Split || e.balance(A.address, Integer(q)) == Amount{}) q = nextPrime(q);
        const Amount held = e.balance(A.address, Integer(q)), sold = Amount(123457);
        require(e.submitTransfer(makeTransfer(e, A, AssetId::main(Integer(q)), sold, G.address), error), "sale: " + error);
        closeNext(e, A, B);
        std::uint64_t at = nextPrime(e.rings().ring(g7)->frontier.get_ui());
        std::vector<Segment> run;
        while (true) {
            run.push_back(makeSegment(vec, -7, at));
            const auto rows = segmentRows(-7, at);
            if (rows.back() >= q) break;
            at = nextPrime(rows.back());
        }
        require(e.submitRingWork(makeWork(e, D, -7, run), error), "work to q: " + error);
        mineBatch(e);
        const AssetId x{g7, Integer(q), 0};
        const u128 up = e.assets().split(x)->below[0].second.raw();
        require(e.assets().due(x, G.address).raw() == up * sold.raw() / 1000000, "the buyer gets the share of what it held");
        require(e.assets().due(x, A.address).raw() == up * (held.raw() - sold.raw() - 1) / 1000000,
                "the seller keeps the share of the rest");
        closeNext(e, A, B);
    }
    auto again = Engine::replay(e.policy(), e.blocks(), error);
    require(!again, "the blocks alone do not replay once rings are mined");
    again = Engine::replay(e.policy(), Genesis{}, e.blocks(), e.ringBatches(), error);
    require(again && again->tip() == e.tip() && again->assets() == e.assets(), "replay from both: " + error);
    std::cout << "ring competition: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- issuance (rule A4) = a4_rows.py
struct IssuanceRun {
    std::map<std::string, Miner> miners;
    std::map<primechain::Address, std::string> names;
    std::vector<RingWork> queued;  // submitted works (journal data: a restarted node gets them again)
};

// Runs the schedule of the vectors on `e` (fresh or opened at height 2); `stop` = last block.
void runSchedule(Engine& e, const Vectors& vec, IssuanceRun& run, std::uint64_t from, std::uint64_t stop,
                 double* batch_ms = nullptr, std::size_t* batches = nullptr) {
    std::string error;
    std::map<std::uint64_t, std::vector<const WorkSpec*>> works, accepts;
    for (const auto& w : vec.works) works[w.h].push_back(&w);
    for (const auto& a : vec.accepts) accepts[a.h].push_back(&a);
    std::map<std::uint64_t, long> ring_d;
    for (const auto& r : vec.rings) ring_d[r.k] = r.d;
    if (e.pendingRingWorkCount() == 0)  // a restarted engine: the works still waiting are sent again, in order
        for (const auto& w : run.queued) {
            std::string ignored;
            e.submitRingWork(w, ignored);
        }
    for (std::uint64_t h = from; h <= stop; ++h) {
        require(e.height() + 1 == h, "schedule in step");
        for (const auto* w : works[h]) {
            const Miner& m = run.miners.at(w->miner);
            const long d = ring_d.at(w->k);
            run.queued.push_back(makeWork(e, m, d, makeSegments(vec, d, static_cast<std::uint64_t>(w->start), w->n),
                                          !e.hasKey(m.address)));
            require(e.submitRingWork(run.queued.back(), error), "work at " + std::to_string(h) + ": " + error);
        }
        // B3.6: the reveals ride in the block's ring batch, so they are queued before it is built.
        for (const auto& r : vec.rings) {
            if (r.commit + 1 == h) {
                std::uint64_t index = 0;
                for (const auto& o : vec.rings)
                    if (o.commit == r.commit && o.k < r.k) ++index;
                require(e.submitRingReveal(makeReveal(r.commit, index, salt(static_cast<std::uint8_t>(r.k)), certificate(vec, r.d)), error),
                        "reveal: " + error);
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto v = e.buildRingBatch();
        if (accepts[h].empty()) {
            require(!v || v->batch.blocks.empty(), "no ring blocks at " + std::to_string(h));
            if (v) require(e.checkRingBatch(*v, error) && e.applyDecidedRingBatch(*v, error), "reveals at " + std::to_string(h) + ": " + error);
        } else {
            require(v.has_value(), "a batch at " + std::to_string(h));
            std::vector<std::string> got, want;
            for (const auto& b : v->batch.blocks) {
                Integer pos = b.start;
                for (const auto& g : b.groups) {
                    got.push_back(std::to_string(b.ring) + " " + run.names.at(g.miner) + " " + pos.get_str() + " " +
                                  std::to_string(g.segments.size()));
                    std::uint64_t q = pos.get_ui();
                    for (std::size_t k = 0; k < g.segments.size(); ++k) q = nextPrime(segmentRows(ring_d.at(b.ring), q).back());
                    pos = q;
                }
            }
            for (const auto* a : accepts[h])
                want.push_back(std::to_string(a->k) + " " + a->miner + " " + std::to_string(a->start) + " " + std::to_string(a->n));
            require(got == want, "the batch at " + std::to_string(h) + " accepts what the simulation accepts");
            require(e.checkRingBatch(*v, error) && e.applyDecidedRingBatch(*v, error), "batch at " + std::to_string(h) + ": " + error);
            if (batch_ms) {
                *batch_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ++*batches;
            }
        }
        for (const auto& r : vec.rings) {
            if (r.commit == h) {
                const Miner& m = run.miners.at(r.who);
                const Bytes s = salt(static_cast<std::uint8_t>(r.k));
                require(e.submitRingCommit(makeCommit(e, m, s, certificate(vec, r.d), Integer(r.asset)), error), "commit: " + error);
            }
        }
        const auto& [ranger, finder] = vec.blocks.at(h);
        closeNext(e, run.miners.at(ranger), run.miners.at(finder));
        std::erase_if(run.queued, [&](const RingWork& w) {
            const RingEntry* r = e.rings().byId(w.ring_id);
            return lastOf(r->d.get_si(), w.segments.back()) <= r->frontier.get_ui();
        });
        for (const auto& r : vec.rings)
            if (r.commit + 1 == h) require(e.rings().ring(r.k) && e.rings().ring(r.k)->opened == h, "ring opens at " + std::to_string(h));
    }
}

// Compare every award with the simulation, to the last unit.
void compareIssuance(const Engine& e, const Vectors& vec, const IssuanceRun& run) {
    std::map<std::uint64_t, long> ring_d;
    for (const auto& r : vec.rings) ring_d[r.k] = r.d;
    std::vector<std::string> lines;
    std::map<std::string, std::uint64_t> due;
    std::uint64_t total = 0;
    for (const auto& [k, d] : ring_d) {
        const RingEntry& r = *e.rings().ring(k);
        require(r.frontier == vec.frontier.at(k), "ring " + std::to_string(k) + " frontier");
        std::uint64_t rows = 0, paid = 0, backlog = 0, issued = 0;
        for (std::uint64_t q = 2; q <= r.frontier.get_ui(); q = nextPrime(q)) {
            const auto row = e.assets().row(k, Integer(q));
            require(row.has_value(), "row " + std::to_string(q) + " of ring " + std::to_string(k));
            ++rows;
            if (row->primes.empty()) continue;
            ++paid;
            for (std::uint32_t i = 0; i < row->primes.size(); ++i) {
                const AssetId x{k, Integer(q), i};
                const u128 v = e.assets().issued(x).raw();
                if (v < row->primes[i].e * 100000u) ++backlog;
                issued += static_cast<std::uint64_t>(v);
                Amount held;
                for (const auto& [a, amount] : e.assets().holders(x)) {
                    const std::string name = run.names.at(a);
                    require(amount == e.assets().due(x, a), "no transfers: owned = due");
                    lines.push_back(std::to_string(k) + " " + std::to_string(q) + " " + std::to_string(i) + " " + name + " " +
                                    amount.str() + "\n");
                    due[name] += static_cast<std::uint64_t>(amount.raw());
                    held = *held.add(amount);
                }
                require(!(e.assets().issued(x) < held), "never more than issued");
            }
        }
        const auto& want = vec.issued.at(k);
        require(rows == want[0] && paid == want[1] && backlog == want[2] && issued == want[3],
                "ring " + std::to_string(k) + ": rows, paid rows, backlog primes and units issued as simulated");
        total += issued;
    }
    require(due == vec.due, "every address is due what the simulation pays");
    std::sort(lines.begin(), lines.end());
    std::string all;
    for (const auto& l : lines) all += l;
    const Hash256 h = sha3(Bytes(all.begin(), all.end()));
    static const char* hex = "0123456789abcdef";
    std::string digest;
    for (auto b : h) { digest += hex[b >> 4]; digest += hex[b & 15]; }
    require(lines.size() == vec.final_lines && digest == vec.final_digest && total == vec.final_total,
            "every award line equal to the simulation (sha3 " + digest.substr(0, 16) + "...)");
}

IssuanceRun namedMiners() {
    IssuanceRun run;
    for (const char* n : {"A", "B", "C", "D", "E", "F"}) {
        run.miners.emplace(n, newMiner());
        run.names[run.miners.at(n).address] = n;
    }
    return run;
}

void issuanceTests(const Vectors& vec) {
    require(vec.params.at("v") == 100000 && vec.params.at("backlog_bp") == 1000 && vec.params.at("upstream_bp") == 3500 &&
                vec.params.at("founder_bp") == 1500, "simulated with the starting parameters");
    IssuanceRun run = namedMiners();
    Engine e;
    double ms = 0;
    std::size_t batches = 0;
    runSchedule(e, vec, run, 3, static_cast<std::uint64_t>(vec.params.at("blocks")), &ms, &batches);
    compareIssuance(e, vec, run);
    std::size_t rows = 0;
    for (const auto& [k, f] : vec.frontier) rows += vec.issued.at(k)[0];
    std::cout << "  " << batches << " batches, " << rows << " rows; " << ms / batches << " ms per batch (build, check, apply)\n";
    // Awards are owned at once and count as held at h - 1 for later rows: a main-chain prime's holder
    // shares the upstream part pro rata (checked line by line above). Spot checks of the rule:
    {
        const RingEntry& r6 = *e.rings().ring(6);  // Q(sqrt 17), opened at block 101
        const AssetId old{6, Integer(13), 0};       // 13 split in Q(sqrt 17), mined in block 6: backlog
        require(e.assets().issued(old) == Amount(10000), "backlog: 10% of v for a prime older than the ring");
        const auto sp = e.assets().split(old);
        require(sp->miner == Amount(5000) && sp->founder == Amount(1500) && sp->below.size() == 1 &&
                    sp->below[0].second == Amount(3500), "R11 split of the backlog share");
        require(r6.terms.backlog_bp == 1000 && r6.terms.prime_value == 100000, "terms fixed at opening");
        Integer young = 0;
        for (std::uint64_t q = 2; q <= r6.frontier.get_ui(); q = nextPrime(q))
            if (q > 541 && typeOf(17, q) == RowType::Split) { young = q; break; }  // 541 = prime of block 100
        require(young != 0 && e.assets().issued(AssetId{6, young, 0}) == Amount(100000), "a prime mined after opening: v");
    }
    // Replay from the two files (in memory): identical.
    std::string error;
    auto again = Engine::replay(e.policy(), Genesis{}, e.blocks(), e.ringBatches(), error);
    require(again && again->tip() == e.tip() && again->stateRoot() == e.stateRoot() && again->assets() == e.assets() &&
                again->rings() == e.rings(), "replay: " + error);
    compareIssuance(*again, vec, run);
    // Ring awards are owned at once and can be spent (transfer version 2 names (ring, q, i)).
    {
        const Miner G = newMiner();
        const AssetId x{3, Integer(11), 0};  // 11 splits in Q(sqrt 5)
        std::string who;
        Amount have;
        for (const auto& [a, amount] : e.assets().holders(x))
            if (run.names.at(a) == "D" || run.names.at(a) == "E" || run.names.at(a) == "F") { who = run.names.at(a); have = amount; }
        require(!who.empty() && have != Amount{}, "a ring-only miner holds an award in " + x.str());
        const Miner& m = run.miners.at(who);
        Transfer v1 = makeTransfer(e, m, x, Amount(1000), G.address, kVersion1);
        require(!e.submitTransfer(v1, error) && error.find("version 2") != std::string::npos, "version 1 cannot name ring primes");
        require(!e.submitTransfer(makeTransfer(e, m, AssetId{3, Integer(13), 0}, Amount(1), G.address), error) &&
                    error.find("unknown asset") != std::string::npos, "13 is inert in Q(sqrt 5): no such asset");
        require(e.submitTransfer(makeTransfer(e, m, x, Amount(1000), G.address), error), "spend an award: " + error);
        const Hash256 root = e.stateRoot();
        Block b = closeNext(e, run.miners.at("A"), run.miners.at("B"));
        require(b.transfers.size() == 1 && b.transfers[0].version == kTransferVersionRings, "included");
        require(e.assets().owned(x, G.address) == Amount(1000) && e.assets().owned(x, m.address) == *have.sub(Amount(1001)) &&
                    e.assets().pool(x) == Amount(1), "the award moves; the fee goes to the pool in the same prime");
        require(e.stateRoot() != root, "the state root covers ring primes that moved");
        const Amount rest = e.assets().owned(x, m.address);
        require(e.submitTransfer(makeTransfer(e, m, x, rest, G.address), error), "form: " + error);
        b = closeNext(e, run.miners.at("A"), run.miners.at("B"));
        require(b.transfers.empty() && e.assets().owned(x, m.address) == rest, "one unit too many (with the fee): not included");
        Bytes raw = encode(e.blocks()[e.blocks().size() - 2].transfers[0]);
        Transfer back;
        require(decode(raw, back, e.policy().limits, error) && encode(back) == raw && back.inputs[0].ring == 3, "v2 codec round trip");
        auto after = Engine::replay(e.policy(), Genesis{}, e.blocks(), e.ringBatches(), error);
        require(after && after->tip() == e.tip() && after->stateRoot() == e.stateRoot() && after->assets() == e.assets(),
                "replay with ring primes spent: " + error);
    }
    std::cout << "ring issuance (A4 with backlog) equal to a4_rows.py: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- persistence (two files)
std::string tempDir(const std::string& tag) {
    std::string tmpl = (fs::temp_directory_path() / ("pcnext-" + tag + "-XXXXXX")).string();
    require(mkdtemp(tmpl.data()) != nullptr, "temp dir");
    return tmpl;
}

void flipByte(const std::string& path, std::uint64_t offset) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    require(f.good(), "open " + path);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(static_cast<char>(c ^ 0x5a));
}

void persistenceTests(const Vectors& vec) {
    std::string error;
    const std::string root = tempDir("rings");
    const std::string dir = root + "/chain";
    Policy policy;
    policy.snapshot_interval = 7;  // snapshots land between ring batches and blocks
    IssuanceRun run = namedMiners();
    const std::uint64_t stop = static_cast<std::uint64_t>(vec.params.at("blocks"));
    Hash256 tip{}, state{}, rows{};
    RingRegistry registry;
    AssetLedger ledger;
    std::uint64_t at = 2;
    // Run the issuance schedule, restarting at several heights; at each restart the engine is
    // opened again from the block file, the rings file, the snapshot and the row index.
    for (const std::uint64_t until : {40ul, 97ul, 150ul, 151ul, stop}) {
        auto opened = Engine::open(dir, policy, Genesis{}, error);
        require(opened.has_value(), "open at " + std::to_string(at) + ": " + error);
        Engine& e = *opened;
        if (at > 2)
            require(e.tip() == tip && e.stateRoot() == state && e.rings() == registry && e.assets() == ledger &&
                        e.assets().rowsDigest() == rows, "restart at " + std::to_string(at) + ": identical state");
        require(e.assets().rowsInMemory() == 0, "rows live on disk");
        runSchedule(e, vec, run, at + 1, until);
        at = until;
        tip = e.tip();
        state = e.stateRoot();
        registry = e.rings();
        ledger = e.assets();
        rows = e.assets().rowsDigest();
        if (until == stop) {
            compareIssuance(e, vec, run);
            const auto sizes = e.storeBytes();
            std::uint64_t n = 0;
            for (const auto& [k, f] : vec.frontier) n += vec.issued.at(k)[0];
            std::cout << "  files: blocks " << sizes.blocks << " B, rings " << sizes.rings << " B, row index " << sizes.rows
                      << " B for " << n << " rows (" << sizes.rings / n << " B per row in the rings file, "
                      << sizes.rows / n << " B per row in the index, chunks of 1,024 slots)\n";
        }
    }
    // A ring batch decided after the last block (a crash before the block): applied on open.
    {
        auto opened = Engine::open(dir, policy, Genesis{}, error);
        require(opened.has_value(), "open: " + error);
        Engine& e = *opened;
        for (int k = 0; k < 15; ++k) closeNext(e, run.miners.at("A"), run.miners.at("B"));
        const RingEntry& r1 = *e.rings().ring(1);
        RingWork w = makeWork(e, run.miners.at("D"), -4, makeSegments(vec, -4, nextPrime(r1.frontier.get_ui()), 1));
        require(e.submitRingWork(w, error), "work: " + error);
        mineBatch(e);
        tip = e.tip();
        registry = e.rings();
        ledger = e.assets();
    }
    {
        auto opened = Engine::open(dir, policy, Genesis{}, error);
        require(opened.has_value(), "open after a batch without its block: " + error);
        require(opened->rings() == registry && opened->assets() == ledger && opened->tip() == tip &&
                    opened->lastRingBatchHeight() == opened->height() + 1, "the decided batch is applied again");
        closeNext(*opened, run.miners.at("A"), run.miners.at("B"));
        tip = opened->tip();
        state = opened->stateRoot();
        registry = opened->rings();
        ledger = opened->assets();
    }
    // Replay from the two files alone (no snapshot, no row index): bit-identical.
    std::vector<Block> blocks;
    std::vector<RingBatch> batches;
    {
        Store store;
        require(store.open(dir, error), "store: " + error);
        for (std::uint64_t i = 0; i < store.blockCount(); ++i) {
            Bytes raw;
            Block b;
            require(store.read(i, raw, error) && decode(raw, b, policy.limits, error) && encode(b) == raw, "read block");
            blocks.push_back(b);
        }
        for (std::uint64_t i = 0; i < store.rings().count(); ++i) {
            Bytes raw;
            RingBatch b;
            require(store.rings().read(i, raw, error) && decode(raw, b, policy.limits.max_text, error) && encode(b) == raw,
                    "read ring batch (codec round trip)");
            require(b.open_height == store.rings().height(i), "open height in the frame");
            batches.push_back(b);
        }
        auto replayed = Engine::replay(policy, Genesis{}, blocks, batches, error);
        require(replayed.has_value(), "replay from the two files: " + error);
        require(replayed->tip() == tip && replayed->stateRoot() == state && replayed->rings() == registry &&
                    replayed->assets() == ledger, "replay from the two files: identical");
        for (std::size_t i = 0; i < batches.size(); ++i)
            require(encode(replayed->ringBatches()[i]) == encode(batches[i]), "ring batches bit-identical");
        require(replayed->blocks().size() == blocks.size(), "blocks");
        for (std::size_t i = 0; i < blocks.size(); ++i)
            require(encode(replayed->blocks()[i]) == encode(blocks[i]), "blocks bit-identical");
    }
    // A fresh directory from copies of the two files only: open rebuilds the row index.
    {
        const std::string fresh = root + "/fresh";
        fs::create_directories(fresh);
        for (const char* f : {"blocks.dat", "blocks.idx", "rings.dat", "rings.idx"}) fs::copy_file(dir + "/" + f, fresh + "/" + f);
        auto opened = Engine::open(fresh, policy, Genesis{}, error);
        require(opened && opened->tip() == tip && opened->assets() == ledger && opened->rings() == registry,
                "open from the two files without a snapshot: " + error);
        require(opened->assets().rowsInMemory() == 0, "rows written to the new index");
        require(opened->stateRoot() == state && opened->assets().rowsDigest() == ledger.rowsDigest(), "state root and rows digest");
    }
    // Corruption is refused, nothing is silently recreated (D19).
    auto copyChain = [&](const std::string& name) {
        const std::string d = root + "/" + name;
        fs::copy(dir, d, fs::copy_options::recursive);
        return d;
    };
    auto refusedOpen = [&](const std::string& d, const std::string& expect, const std::string& what) {
        std::string why;
        require(!Engine::open(d, policy, Genesis{}, why), what + ": refused");
        require(why.find(expect) != std::string::npos, what + ": expected '" + expect + "', got '" + why + "'");
    };
    {
        const std::string d = copyChain("bad-rings");
        flipByte(d + "/rings.dat", fs::file_size(d + "/rings.dat") / 2);
        refusedOpen(d, "rings file corrupt", "a corrupted rings file");
    }
    {
        const std::string d = copyChain("bad-rings-idx");
        flipByte(d + "/rings.idx", 30);
        refusedOpen(d, "rings index", "a corrupted rings index");
    }
    {
        const std::string d = copyChain("short-rings-idx");
        fs::resize_file(d + "/rings.idx", fs::file_size(d + "/rings.idx") - 48);
        refusedOpen(d, "rings index does not match", "a rings index missing entries");
    }
    {
        const std::string d = copyChain("no-rings-idx");
        fs::remove(d + "/rings.idx");
        refusedOpen(d, "missing", "a missing rings index");
    }
    {
        const std::string d = copyChain("bad-rows");
        flipByte(d + "/rows.dat", 32 + 32 + 32 * 5 + 3);  // a record of the first chunk
        refusedOpen(d, "row index corrupt", "a corrupted row index record");
    }
    {
        const std::string d = copyChain("bad-chunk");
        flipByte(d + "/rows.dat", 32 + 10);  // the first chunk header
        refusedOpen(d, "row index corrupt", "a corrupted chunk header");
    }
    {
        const std::string d = copyChain("bad-miners");
        flipByte(d + "/miners.dat", 6);
        refusedOpen(d, "row index corrupt", "a corrupted miner table");
    }
    {
        const std::string d = copyChain("no-rows");
        fs::remove(d + "/rows.dat");
        refusedOpen(d, "missing", "a missing row index");
    }
    {
        const std::string d = copyChain("empty-rows");  // a valid but empty index under a snapshot with rows
        fs::remove(d + "/rows.dat");
        fs::remove(d + "/miners.dat");
        refusedOpen(d, "row index does not match the state", "an empty row index under a snapshot with rows");
    }
    {
        const std::string d = copyChain("no-rings");
        fs::remove(d + "/rings.dat");
        fs::remove(d + "/rings.idx");
        refusedOpen(d, "", "the block file without its rings file");
    }
    {
        // An interrupted append of the rings file (an incomplete final frame) is cut away; the
        // batch was not decided durably, so the chain opens without it.
        const std::string d = copyChain("torn-rings");
        {
            std::ofstream f(d + "/rings.dat", std::ios::app | std::ios::binary);
            f << "torn";
        }
        auto opened = Engine::open(d, policy, Genesis{}, error);
        require(opened && opened->tip() == tip, "an incomplete final ring frame is truncated: " + error);
    }
    fs::remove_all(root);
    std::cout << "ring persistence: " << checks << " checks passed\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: primechain-next-ring-mine-tests rows|compete|issuance|persistence <a4_vectors.txt>");
        const std::string mode = argv[1];
        const Vectors vec = load(argv[2]);
        if (mode == "rows") rowTests(vec);
        else if (mode == "compete") competeTests(vec);
        else if (mode == "issuance") issuanceTests(vec);
        else if (mode == "persistence") persistenceTests(vec);
        else throw std::runtime_error("unknown mode " + mode);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ring mining test failed: " << e.what() << '\n';
        return 1;
    }
}
