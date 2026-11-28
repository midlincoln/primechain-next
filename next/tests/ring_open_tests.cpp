// Tests for B3.4: opening rings (RING_COMMIT, RING_REVEAL, deposit, gate 1, kappa, RING_PAYEE,
// the registry), with restart and replay. Specification: docs/ring-records.md section 2,
// docs/rings-design.md R2, R3, R5, R6.
// Usage: primechain-next-ring-open-tests race|rules|payee|governance|persistence
#include "primechain/crypto/signature.hpp"
#include "primechain_next/engine.hpp"
#include "primechain_next/merkle.hpp"

#include <array>
#include <optional>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace primechain::next;
namespace crypto = primechain::crypto;

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

// One block: ranges by `ranger`, the prime by `finder`, plus whatever is queued.
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

// ---- certificates (from experiments/extensions/quadratic_vectors_v2.txt)
Bytes cert(long d, std::vector<std::pair<long, unsigned>> factors, std::vector<std::array<long, 3>> gens = {},
           std::optional<std::pair<long, long>> unit = std::nullopt) {
    QuadCertificate c;
    c.d = Integer(d);
    for (const auto& [p, e] : factors) c.factors.emplace_back(Integer(p), e);
    for (const auto& g : gens) c.generators.push_back({Integer(g[0]), {Integer(g[1]), Integer(g[2])}});
    if (unit) c.unit = QuadElement{Integer(unit->first), Integer(unit->second)};
    return encode(c);
}
const Bytes kMinus7 = cert(-7, {{7, 1}});
const Bytes kMinus4 = cert(-4, {{2, 2}});
const Bytes kMinus43 = cert(-43, {{43, 1}});
const Bytes kFive = cert(5, {{5, 1}}, {}, std::make_pair(0L, -1L));
const Bytes kSeventeen = cert(17, {{17, 1}}, {{2, 2, -1}}, std::make_pair(-3L, -2L));
const Bytes kFiftySeven = cert(57, {{3, 1}, {19, 1}}, {{2, -4, 1}, {3, 13, 4}}, std::make_pair(131L, 40L));
const Bytes kSeventeenBad = cert(17, {{17, 1}}, {{2, 3, -1}}, std::make_pair(-3L, -2L));  // wrong generator
const Bytes kMinus15 = cert(-15, {{3, 1}, {5, 1}});  // class number 2: no PID certificate

Bytes salt(std::uint8_t tag) { return Bytes(kRingSaltBytes, tag); }

struct Secret { Bytes salt; Bytes certificate; };

RingCommit makeCommit(const Engine& e, const Miner& m, const Secret& s, const Integer& asset,
                      std::optional<Amount> deposit = std::nullopt, std::uint64_t nonce_offset = 1) {
    RingCommit c;
    c.network_id = e.policy().network_id;
    c.discoverer = m.address;
    c.commitment = ringCommitment(c.network_id, m.address, s.salt, certificateHash(1, 1, s.certificate));
    c.deposit_asset = asset;
    c.deposit = deposit ? *deposit : Amount::fromRaw(e.parameter(Param::RingDeposit));
    c.nonce = e.nonce(m.address) + nonce_offset;
    if (!e.hasKey(m.address)) c.public_key = m.key.public_key;
    c.signature = sign(m, ringCommitSigningPayload(c));
    return c;
}
RingReveal makeReveal(std::uint64_t height, std::uint64_t index, const Secret& s) {
    RingReveal v;
    v.commit_height = height;
    v.commit_index = index;
    v.salt = s.salt;
    v.certificate = s.certificate;
    return v;
}
void commit(Engine& e, const RingCommit& c) {
    std::string error;
    require(e.submitRingCommit(c, error), "commit submitted: " + error);
}
void reveal(Engine& e, const RingReveal& v) {
    std::string error;
    require(e.submitRingReveal(v, error), "reveal submitted: " + error);
}

// A main-chain prime in which `a` holds at least `need` (mined earlier).
Integer fundedAsset(const Engine& e, const Miner& m, const Amount& need) {
    for (const auto& x : e.assets().assets())
        if (x.isMain() && !(e.balance(m.address, x.q) < need) && e.balance(m.address, x.q) != Amount{}) return x.q;
    throw std::runtime_error("no funded asset");
}

Hash256 merkleRootOf(const RingRegistry& registry, const Block& block) {
    std::vector<Bytes> leaves = registry.leaves();
    for (const auto& v : block.ring_reveals) leaves.push_back(encode(v));
    return merkleRoot(leaves);
}

const OpenCommit* sealedAt(const Engine& e, std::uint64_t h, std::uint64_t i) { return e.rings().commit(h, i); }

struct World {
    Miner A = newMiner(), B = newMiner(), C = newMiner(), N = newMiner();  // N never mines
    Engine e;
    Integer a_asset, b_asset, c_asset;
    explicit World(Policy policy = {}, Genesis genesis = {}, std::uint64_t to = 99) : e(std::move(policy), std::move(genesis)) {
        const Miner* who[] = {&A, &B, &C};
        for (std::uint64_t k = 0; e.height() < to; ++k) closeNext(e, *who[(k + 1) % 3], *who[k % 3]);
        const Amount d = Amount::fromRaw(e.parameter(Param::RingDeposit));
        a_asset = fundedAsset(e, A, d);
        b_asset = fundedAsset(e, B, d);
        c_asset = fundedAsset(e, C, d);
    }
    Block next() { return closeNext(e, C, C); }
};

const Secret kSecretA{salt(0xA1), kMinus7}, kSecretB{salt(0xB2), kMinus7};

// ---------------------------------------------------------------- the races of ring-records.md
void raceTests() {
    // 1. A commits at 100, B commits the same ring at 101 and reveals at 102, A reveals at 105.
    //    B waits; at 105 the ring opens for A, and B gets the deposit back.
    {
        World w;
        Engine& e = w.e;
        const Amount dep = Amount::fromRaw(e.parameter(Param::RingDeposit));
        require(dep == Amount(100000), "deposit starts at 100,000 micro-units");
        const Amount a0 = e.balance(w.A.address, w.a_asset), b0 = e.balance(w.B.address, w.b_asset);
        const Amount pool_a0 = e.feePoolBalance(w.a_asset);
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        Block b100 = w.next();
        require(b100.header.height == 100 && b100.ring_commits.size() == 1, "A's commit in block 100");
        require(e.balance(w.A.address, w.a_asset) == *a0.sub(dep), "the deposit leaves A");
        require(e.feePoolBalance(w.a_asset) == pool_a0, "the deposit is held by nobody, not the pool");
        commit(e, makeCommit(e, w.B, kSecretB, w.b_asset));
        require(w.next().header.height == 101, "B commits at 101");
        require(e.balance(w.B.address, w.b_asset) == *b0.sub(dep), "the deposit leaves B");
        reveal(e, makeReveal(101, 0, kSecretB));
        Block b102 = w.next();
        require(b102.ring_reveals.size() == 1, "B reveals at 102");
        require(e.rings().rings().empty(), "B waits: A's earlier commit is still sealed");
        require(e.balance(w.B.address, w.b_asset) == b0, "B's valid reveal is refunded at once");
        require(sealedAt(e, 101, 0) && sealedAt(e, 101, 0)->revealed, "B's commit is revealed and waiting");
        w.next();
        w.next();
        require(e.rings().rings().empty() && e.height() == 104, "still waiting at 104");
        reveal(e, makeReveal(100, 0, kSecretA));
        w.next();
        require(e.height() == 105 && e.rings().rings().size() == 1, "the ring opens at 105");
        const RingEntry& r = *e.rings().ring(1);
        require(r.founder == w.A.address && r.opened == 105 && r.commit_height == 100, "the ring opens for A");
        require(r.ring_id == quadraticRingId(Integer(-7)) && r.d == -7 && r.certificate_type == 1, "ring identity");
        require(r.certificate_hash == certificateHash(1, 1, kMinus7), "certificate hash in the record");
        require(r.payees.size() == 1 && r.payees[0] == std::make_pair(std::uint64_t{105}, w.A.address), "payee is the founder");
        require(r.terms == e.ringTermsAt(105), "terms fixed from the parameters at opening");
        require(r.frontier == 0 && r.tip == ringGenesisHash(1, r.ring_id, r.certificate_hash, 105) && !r.paused,
                "frontier, tip and pause state");
        require(e.rings().commits().empty(), "both commits settled");
        require(e.balance(w.A.address, w.a_asset) == a0 && e.balance(w.B.address, w.b_asset) == b0,
                "both deposits returned (B as the loser)");
        require(e.feePoolBalance(w.a_asset) == pool_a0, "nothing went to the pool");
        // B3.3 wiring: the ledger has the ring with the same terms and founder.
        const RingRecord* lr = e.assets().ring(1);
        require(lr && lr->terms == r.terms && lr->opened == 105 && lr->payees == r.payees, "ring wired into the asset ledger");
        std::string error;
        require(Engine::replay(e.policy(), e.blocks(), error).has_value(), "race replays: " + error);
    }
    // 2. B reveals first with no earlier commit: the ring opens at once.
    {
        World w;
        Engine& e = w.e;
        commit(e, makeCommit(e, w.B, kSecretB, w.b_asset));
        w.next();
        reveal(e, makeReveal(100, 0, kSecretB));
        w.next();
        require(e.rings().rings().size() == 1 && e.rings().ring(1)->opened == 101 &&
                e.rings().ring(1)->founder == w.B.address, "opens in the block of the reveal");
    }
    // 3. Commits in one block count in block order: A (index 0) and B (index 1) both in block 100;
    //    B reveals at 101, A at 103: A gets it.
    {
        World w;
        Engine& e = w.e;
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        commit(e, makeCommit(e, w.B, kSecretB, w.b_asset));
        Block b = w.next();
        require(b.ring_commits.size() == 2, "two commits in one block");
        reveal(e, makeReveal(100, 1, kSecretB));
        w.next();
        require(e.rings().rings().empty(), "B (index 1) waits for A (index 0)");
        w.next();
        reveal(e, makeReveal(100, 0, kSecretA));
        w.next();
        require(e.rings().ring(1) && e.rings().ring(1)->founder == w.A.address, "block order decides");
    }
    // 4. Expiry: A commits at 100 and never reveals; B commits at 101 and reveals at 102. A's
    //    window ends with block 110; at 111 A's deposit goes to the fee pool and B's ring opens
    //    (at most 10 blocks after B's own commit). A later reveal by A is refused.
    {
        World w;
        Engine& e = w.e;
        const Amount dep = Amount::fromRaw(e.parameter(Param::RingDeposit));
        const Amount a0 = e.balance(w.A.address, w.a_asset), pool0 = e.feePoolBalance(w.a_asset);
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        w.next();
        const Secret sb{salt(0xB3), kMinus4};
        commit(e, makeCommit(e, w.B, sb, w.b_asset));
        w.next();
        reveal(e, makeReveal(101, 0, sb));
        w.next();
        while (e.height() < 110) w.next();
        require(e.rings().rings().empty() && sealedAt(e, 100, 0), "A still sealed through block 110");
        w.next();
        require(e.height() == 111 && e.rings().ring(1) && e.rings().ring(1)->founder == w.B.address &&
                e.rings().ring(1)->opened == 111, "B's ring opens at 111 = 101 + 10");
        require(!sealedAt(e, 100, 0), "A's commit expired");
        require(e.feePoolBalance(w.a_asset) == *pool0.add(dep), "A's deposit went to the fee pool");
        require(e.balance(w.A.address, w.a_asset) == *a0.sub(dep), "A is not refunded");
        std::string error;
        require(!e.submitRingReveal(makeReveal(100, 0, kSecretA), error), "a reveal after expiry is refused");
        // Later commits are no longer blocked: C commits at 112 and reveals at 113, opening at 113.
        const Secret sc{salt(0xC1), kFive};
        commit(e, makeCommit(e, w.C, sc, w.c_asset));
        w.next();
        reveal(e, makeReveal(112, 0, sc));
        w.next();
        require(e.rings().ring(2) && e.rings().ring(2)->opened == 113 && e.rings().ring(2)->d == 5, "later commits unblocked");
    }
    // 5. The last block of the window still counts: commit at 100, reveal at 110.
    {
        World w;
        Engine& e = w.e;
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        w.next();
        while (e.height() < 109) w.next();
        reveal(e, makeReveal(100, 0, kSecretA));
        w.next();
        require(e.height() == 110 && e.rings().ring(1) && e.rings().ring(1)->opened == 110, "reveal in block h + 10");
    }
    // 6. A loser that reveals after the ring opened: A commits at 100, C at 101 (same ring), A
    //    reveals at 102 (opens), C reveals at 103. C committed before the opening, so C's reveal
    //    is valid and refunded; a commit made after the opening would forfeit (rules test).
    {
        World w;
        Engine& e = w.e;
        const Amount c0 = e.balance(w.C.address, w.c_asset);
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        w.next();
        const Secret sc{salt(0xC7), kMinus7};
        commit(e, makeCommit(e, w.C, sc, w.c_asset));
        w.next();
        reveal(e, makeReveal(100, 0, kSecretA));
        w.next();
        require(e.rings().ring(1) && e.rings().ring(1)->opened == 102 && e.rings().ring(1)->founder == w.A.address,
                "A's ring opens at 102 while C is sealed (C is later)");
        reveal(e, makeReveal(101, 0, sc));
        w.next();
        bool refunded = false, lost = false;
        for (const auto& o : e.lastRingOutcomes()) {
            refunded |= o.kind == RingOutcome::Kind::Refunded && o.commit_height == 101;
            lost |= o.kind == RingOutcome::Kind::Lost && o.commit_height == 101 && o.ring == 1;
        }
        require(refunded && lost && e.balance(w.C.address, w.c_asset) == c0, "the late loser is refunded");
        require(e.rings().rings().size() == 1 && e.rings().commits().empty(), "no second ring");
    }
    std::cout << "ring races: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- copies, invalid reveals, limits
void ruleTests() {
    std::string error;
    // 1. Copied reveals. A commits at 100; B commits at 101 and reveals at 102.
    {
        World w;
        Engine& e = w.e;
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        w.next();
        const Secret sb{salt(0xB2), kSeventeen};
        commit(e, makeCommit(e, w.B, sb, w.b_asset));
        const Secret sc_own{salt(0xC9), kFive};  // C's own sealed commit, to a different certificate
        commit(e, makeCommit(e, w.C, sc_own, w.c_asset));
        w.next();
        // Reveals that do not match the commitment cost nothing and are not accepted.
        require(!e.submitRingReveal(makeReveal(100, 0, {salt(0xEE), kMinus7}), error) && error.find("match") != std::string::npos,
                "wrong salt refused");
        require(!e.submitRingReveal(makeReveal(100, 0, {kSecretA.salt, kMinus4}), error), "wrong certificate refused");
        require(!e.submitRingReveal(makeReveal(100, 5, kSecretA), error), "no commit at that index");
        require(!e.submitRingReveal(makeReveal(101, 0, kSecretA), error), "A's secret under B's commit refused");
        reveal(e, makeReveal(101, 0, sb));
        w.next();  // 102
        require(!e.submitRingReveal(makeReveal(101, 0, sb), error), "a second reveal of the same commit is refused");
        // C replays B's public salt and certificate under C's own earlier commit: no match.
        require(!e.submitRingReveal(makeReveal(101, 1, sb), error), "B's reveal under C's commit refused");
        require(sealedAt(e, 101, 1) && !sealedAt(e, 101, 1)->revealed, "C's commit untouched");
        // C commits a copy (C's name, B's salt and certificate) after B's reveal: valid but later.
        const Amount c0 = e.balance(w.C.address, w.c_asset);  // C's first deposit is still held
        commit(e, makeCommit(e, w.C, sb, w.c_asset));
        w.next();  // 103
        reveal(e, makeReveal(103, 0, sb));
        w.next();  // 104
        require(e.rings().rings().empty(), "nothing opens while A is sealed");
        // A never reveals; C's first commit is revealed (kFive), then A expires at 111.
        reveal(e, makeReveal(101, 1, sc_own));
        while (e.height() < 111) w.next();
        require(e.rings().rings().size() == 2, "two rings open at 111");
        require(e.rings().byId(quadraticRingId(Integer(17)))->founder == w.B.address, "B, not the copier, gets ring 17");
        require(e.rings().byId(quadraticRingId(Integer(5)))->founder == w.C.address, "C gets ring 5");
        require(e.rings().ring(1)->d == 17 && e.rings().ring(2)->d == 5, "numbers in commit order (101/0, then 101/1)");
        bool lost = false;
        for (const auto& o : e.lastRingOutcomes()) lost |= o.kind == RingOutcome::Kind::Lost && o.commit_height == 103 && o.ring == 1;
        require(lost, "the copy lost ring 17 to B");
        const Amount dep = Amount::fromRaw(e.parameter(Param::RingDeposit));
        require(e.balance(w.C.address, w.c_asset) == *c0.add(dep), "both of C's deposits refunded, the copier's as a loser");
        require(e.rings().commits().empty(), "all settled");
        // Strict replay rejects a block that carries an unmatched reveal.
        std::vector<Block> blocks = e.blocks();
        blocks[101].ring_reveals.push_back(makeReveal(100, 0, {salt(0xEE), kMinus7}));  // block 102
        require(!Engine::replay(e.policy(), blocks, error).has_value() && error.find("reveal") != std::string::npos,
                "an unmatched reveal invalidates a block: " + error);
        require(Engine::replay(e.policy(), e.blocks(), error).has_value(), "the real chain replays: " + error);
    }
    // 2. Invalid certificates, duplicate ring, too large: the reveal matches, so it is included,
    //    the deposit goes to the fee pool and the commit is settled at once (it no longer delays).
    {
        Policy policy;
        policy.parameters[pid(Param::CertificateBytes)] = 1024;  // the lowest bound, to test the limit
        World w(policy);
        Engine& e = w.e;
        const Amount dep = Amount::fromRaw(e.parameter(Param::RingDeposit));
        // Ring -7 opens first (A), to test a duplicate.
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
        w.next();
        reveal(e, makeReveal(100, 0, kSecretA));
        w.next();  // 101: ring 1 = -7
        require(e.rings().ring(1) && e.rings().ring(1)->d == -7, "ring -7 open");
        const Bytes junk(100, 0x07);
        const Bytes large(2000, 0x01);
        const std::vector<std::pair<Secret, std::string>> bad = {
            {{salt(1), kSeventeenBad}, "certificate: "},
            {{salt(2), kMinus15}, "certificate: "},
            {{salt(3), junk}, "certificate encoding"},
            {{salt(4), large}, "size limit"},
            {{salt(5), kMinus7}, "already open"},
        };
        const Integer asset = fundedAsset(e, w.B, *dep.mul(bad.size()));
        const Amount pool0 = e.feePoolBalance(asset), b0 = e.balance(w.B.address, asset);
        for (std::size_t i = 0; i < bad.size(); ++i) {
            commit(e, makeCommit(e, w.B, bad[i].first, asset, std::nullopt, i + 1));
        }
        Block cb = w.next();  // 102
        require(cb.ring_commits.size() == bad.size(), "five commits");
        require(e.balance(w.B.address, asset) == *b0.sub(*dep.mul(bad.size())), "five deposits held");
        for (std::size_t i = 0; i < bad.size(); ++i) reveal(e, makeReveal(102, i, bad[i].first));
        // A valid commit behind them is not delayed by the invalid ones.
        const Secret good{salt(6), kFiftySeven};
        commit(e, makeCommit(e, w.C, good, w.c_asset));
        Block rb = w.next();  // 103
        require(rb.ring_reveals.size() == bad.size(), "matched invalid reveals are included");
        for (std::size_t i = 0; i < bad.size(); ++i) {
            bool seen = false;
            for (const auto& o : e.lastRingOutcomes())
                if (o.commit_height == 102 && o.commit_index == i && o.kind == RingOutcome::Kind::Forfeited) {
                    seen = true;
                    require(o.reason.find(bad[i].second) != std::string::npos, "reason for " + std::to_string(i) + ": " + o.reason);
                }
            require(seen, "forfeit recorded for bad reveal " + std::to_string(i));
        }
        require(e.feePoolBalance(asset) == *pool0.add(*dep.mul(bad.size())), "the deposits went to the fee pool");
        require(e.balance(w.B.address, asset) == *b0.sub(*dep.mul(bad.size())), "no refund for invalid reveals");
        for (std::size_t i = 0; i < bad.size(); ++i) require(!sealedAt(e, 102, i), "invalid commit settled");
        reveal(e, makeReveal(103, 0, good));
        w.next();  // 104
        require(e.rings().ring(2) && e.rings().ring(2)->d == 57 && e.rings().ring(2)->opened == 104 &&
                e.rings().ring(2)->unit == QuadElement{Integer(131), Integer(40)}, "the valid ring opens with its unit");
        require(Engine::replay(e.policy(), e.blocks(), error).has_value(), "replays: " + error);
    }
    // 3. Eligibility |D| <= F^2 against a small frontier, with deposit 0 (allowed, nothing moves).
    {
        Policy policy;
        policy.parameters[pid(Param::RingDeposit)] = 0;
        Engine e(policy);
        const Miner A = newMiner(), N = newMiner();  // N holds nothing at all
        closeNext(e, A, A);  // block 3 closes 5; frontier 5
        require(e.chainFrontier() == 5, "frontier 5");
        const Secret s43{salt(43), kMinus43}, s7{salt(7), kMinus7};
        commit(e, makeCommit(e, N, s43, Integer(2), Amount{}));
        commit(e, makeCommit(e, N, s7, Integer(2), Amount{}, 2));
        Block b4 = closeNext(e, A, A);  // block 4 closes 7
        require(b4.ring_commits.size() == 2 && e.nonce(N.address) == 2, "deposit 0: commits by an address with no units");
        reveal(e, makeReveal(4, 1, s7));
        closeNext(e, A, A);  // block 5: -7 waits for -43 (sealed, index 0)
        require(e.rings().rings().empty(), "-7 waits for the earlier -43");
        reveal(e, makeReveal(4, 0, s43));
        closeNext(e, A, A);  // block 6
        require(e.rings().rings().size() == 2 && e.rings().ring(1)->d == -43 && e.rings().ring(2)->d == -7,
                "both open in commit order once eligible (F = 11 at block 6)");
        // A fresh engine at F = 3: commits in block 3, reveals in block 4 against F = 5 (F^2 = 25).
        Engine low(policy);
        const Secret s7b{salt(8), kMinus7}, s43b{salt(9), kMinus43}, s17{salt(10), kSeventeen};
        commit(low, makeCommit(low, N, s7b, Integer(2), Amount{}));
        commit(low, makeCommit(low, N, s43b, Integer(2), Amount{}, 2));
        commit(low, makeCommit(low, N, s17, Integer(2), Amount{}, 3));
        closeNext(low, A, A);  // block 3 (F before = 3, after = 5)
        reveal(low, makeReveal(3, 0, s7b));
        reveal(low, makeReveal(3, 1, s43b));
        reveal(low, makeReveal(3, 2, s17));
        closeNext(low, A, A);  // block 4: F = 5, F^2 = 25: -7 eligible (7 > F is prime by trial division), -43 not, 17 yes
        require(low.rings().ring(1) && low.rings().ring(1)->d == -7, "-7 opens at F = 5 (factor 7 above the frontier)");
        require(low.rings().ring(2) && low.rings().ring(2)->d == 17, "17 opens at F = 5");
        require(low.rings().rings().size() == 2, "-43 refused at F = 5");
        bool ineligible = false;
        for (const auto& o : low.lastRingOutcomes())
            ineligible |= o.commit_index == 1 && o.kind == RingOutcome::Kind::Forfeited && o.reason.find("eligible") != std::string::npos;
        require(ineligible, "eligibility failure recorded");
        require(low.rings().commits().empty(), "all settled");
        require(Engine::replay(low.policy(), low.blocks(), error).has_value(), "replays: " + error);
    }
    // 4. Insufficient funds for the deposit: accepted for the queue, not included.
    {
        World w;
        Engine& e = w.e;
        const RingCommit c = makeCommit(e, w.N, kSecretA, w.a_asset);
        commit(e, c);
        Block b = w.next();
        require(b.ring_commits.empty() && e.nonce(w.N.address) == 0 && e.rings().commits().empty(), "not included without the deposit");
        // A deposit other than the parameter is refused as well.
        commit(e, makeCommit(e, w.A, kSecretA, w.a_asset, Amount(1)));
        require(w.next().ring_commits.empty(), "wrong deposit amount not included");
        // Strict replay rejects such a block.
        std::vector<Block> blocks = e.blocks();
        blocks.back().ring_commits.push_back(c);
        require(!Engine::replay(e.policy(), blocks, error).has_value() && error.find("ring commit") != std::string::npos,
                "a commit without funds invalidates a block: " + error);
    }
    // 5. Gate 1 closed: rejected on arrival, nothing taken. kappa: one ring per epoch.
    {
        Policy closed;
        closed.parameters[pid(Param::RingGateOpen)] = 0;
        World w(closed, {}, 5);
        const Amount a0 = w.e.balance(w.A.address, w.a_asset);
        require(!w.e.submitRingCommit(makeCommit(w.e, w.A, kSecretA, w.a_asset), error) && error.find("gate 1") != std::string::npos,
                "gate 1 closed: commit refused");
        require(w.e.balance(w.A.address, w.a_asset) == a0 && w.e.pendingRingCount() == 0, "nothing taken");

        Policy kappa;
        kappa.parameters[pid(Param::RingsPerEpoch)] = 1;
        kappa.epoch_length = 10;
        World k(kappa, {}, 21);  // next block 22, epoch 21..30
        commit(k.e, makeCommit(k.e, k.A, kSecretA, k.a_asset));
        k.next();  // 22
        reveal(k.e, makeReveal(22, 0, kSecretA));
        k.next();  // 23: ring 1 opens
        commit(k.e, makeCommit(k.e, k.B, {salt(0x44), kMinus4}, k.b_asset));
        require(k.next().ring_commits.empty(), "kappa reached: commit not included in this epoch");
        while (k.e.height() < 30) k.next();
        const Secret s4{salt(0x45), kMinus4};
        commit(k.e, makeCommit(k.e, k.B, s4, k.b_asset));
        require(k.next().ring_commits.size() == 1 && k.e.height() == 31, "a new epoch accepts commits again");
    }
    std::cout << "ring rules: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- RING_PAYEE
RingPayee makePayee(const Engine& e, const Miner& signer, std::uint64_t ring, const Miner& to, std::uint64_t nonce_offset = 1) {
    RingPayee p;
    p.network_id = e.policy().network_id;
    p.ring = ring;
    p.payee = to.address;
    p.nonce = e.nonce(signer.address) + nonce_offset;
    if (!e.hasKey(signer.address)) p.public_key = signer.key.public_key;
    p.signature = sign(signer, ringPayeeSigningPayload(p));
    return p;
}

void payeeTests() {
    std::string error;
    World w;
    Engine& e = w.e;
    commit(e, makeCommit(e, w.B, kSecretB, w.b_asset));
    w.next();
    reveal(e, makeReveal(100, 0, kSecretB));
    w.next();  // 101: ring 1, founder B
    require(!e.submitRingPayee(makePayee(e, w.A, 1, w.A), error), "A is not the payee");
    require(!e.submitRingPayee(makePayee(e, w.B, 2, w.C), error) && error == "unknown ring", "unknown ring");
    const std::uint64_t nb = e.nonce(w.B.address);
    require(e.submitRingPayee(makePayee(e, w.B, 1, w.C), error), "B names C: " + error);
    Block b = w.next();  // 102
    require(b.ring_payees.size() == 1 && e.nonce(w.B.address) == nb + 1, "included; B's nonce used");
    const RingEntry& r = *e.rings().ring(1);
    require(r.founder == w.B.address && r.payees.size() == 2 && r.payees[1] == std::make_pair(std::uint64_t{102}, w.C.address),
            "payee history (101, B), (102, C); the founder never changes");
    require(e.assets().ring(1)->payees == r.payees, "ledger payee history agrees");
    require(!e.submitRingPayee(makePayee(e, w.B, 1, w.B), error), "B is no longer the payee");
    // A payee change by the old payee forced into a block invalidates it on replay.
    {
        std::vector<Block> blocks = e.blocks();
        blocks.back().ring_payees.push_back(makePayee(e, w.B, 1, w.B));
        require(!Engine::replay(e.policy(), blocks, error).has_value(), "payee change by a non-payee invalidates a block");
    }
    require(e.submitRingPayee(makePayee(e, w.C, 1, w.A), error), "C names A");
    w.next();  // 103
    require(e.rings().ring(1)->payees.back() == std::make_pair(std::uint64_t{103}, w.A.address), "A from 103");
    // A replayed (same nonce) change is refused.
    require(!e.submitRingPayee(makePayee(e, w.C, 1, w.A, 0), error), "stale nonce refused");
    require(Engine::replay(e.policy(), e.blocks(), error).has_value(), "payee chain replays: " + error);
    std::cout << "ring payee: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- gate 1 by vote (R2)
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

void governanceTests() {
    // Commits finalized while gate 1 was open may still be revealed after it closes (R2).
    std::string error;
    Policy policy;
    policy.governance_notice = 2;
    policy.epoch_length = 1000;
    const Val v = newVal(policy.network_id);
    Genesis genesis;
    genesis.validators.push_back(v.record);
    World w(policy, genesis, 10);
    Engine& e = w.e;
    commit(e, makeCommit(e, w.A, kSecretA, w.a_asset));
    GovernanceTx t;
    t.kind = GovernanceTxKind::Vote;
    t.proposal.network_id = policy.network_id;
    t.proposal.kind = GovernanceKind::ParamChange;
    t.proposal.changes = {{pid(Param::RingGateOpen), 1, 0}};
    t.proposal.expiry_height = e.height() + 20;
    t.proposal.sequence = 1;
    t.proposal.validator_set_digest = e.validatorSetDigest();
    t.signer_key = v.consensus.key.public_key;
    t.signature = sign(v.consensus, governanceSigningPayload(t));
    require(e.submitGovernance(t, error), "vote: " + error);
    w.next();  // 11: commit and vote; the gate closes from 13
    w.next();  // 12
    require(e.parameter(Param::RingGateOpen) == 0, "gate 1 closed from block 13");
    require(!e.submitRingCommit(makeCommit(e, w.B, kSecretB, w.b_asset), error), "new commits refused");
    reveal(e, makeReveal(11, 0, kSecretA));
    w.next();  // 13
    require(e.rings().ring(1) && e.rings().ring(1)->opened == 13, "a commit made while open still opens after closing");
    std::cout << "ring gate vote: " << checks << " checks passed\n";
}

// ---------------------------------------------------------------- restart and replay
void persistenceTests() {
    namespace fs = std::filesystem;
    std::string tmpl = (fs::temp_directory_path() / "pcnext-rings-XXXXXX").string();
    require(mkdtemp(tmpl.data()) != nullptr, "temp dir");
    const std::string dir = (fs::path(tmpl) / "chain").string();
    std::string error;
    Policy policy;
    policy.snapshot_interval = 3;  // snapshots land while commits are open
    const Miner A = newMiner(), B = newMiner(), C = newMiner();
    Hash256 tip{}, state{};
    RingRegistry registry;
    AssetLedger ledger;
    std::vector<Block> all;
    {
        auto opened = Engine::open(dir, policy, Genesis{}, error);
        require(opened.has_value(), "open: " + error);
        Engine& e = *opened;
        for (int k = 0; e.height() < 30; ++k) closeNext(e, k % 2 ? A : B, k % 3 ? A : B);
        const Amount dep = Amount::fromRaw(e.parameter(Param::RingDeposit));
        const Integer aa = fundedAsset(e, A, dep), ba = fundedAsset(e, B, dep);
        commit(e, makeCommit(e, A, kSecretA, aa));                 // 31: A sealed
        closeNext(e, C, C);
        commit(e, makeCommit(e, B, kSecretB, ba));                 // 32: B
        const Secret never{salt(0x99), kFive};
        commit(e, makeCommit(e, A, never, aa, std::nullopt, 1));   // 32: A, never revealed (index 1)
        closeNext(e, C, C);
        reveal(e, makeReveal(32, 0, kSecretB));                     // 33: B waits
        closeNext(e, C, C);
        // Restart in the middle: B revealed and waiting, A sealed twice.
        tip = e.tip();
        state = e.stateRoot();
        registry = e.rings();
        ledger = e.assets();
    }
    {
        auto opened = Engine::open(dir, policy, Genesis{}, error);
        require(opened.has_value(), "reopen: " + error);
        Engine& e = *opened;
        require(e.tip() == tip && e.stateRoot() == state && e.rings() == registry && e.assets() == ledger,
                "restart with open commits: identical state");
        require(e.rings().commits().size() == 3 && e.rings().commit(32, 0)->revealed, "open commits survive");
        reveal(e, makeReveal(31, 0, kSecretA));                     // 34: A's ring opens, B loses
        closeNext(e, C, C);
        require(e.rings().ring(1) && e.rings().ring(1)->founder == A.address, "opens after restart");
        require(e.submitRingPayee(makePayee(e, A, 1, C), error), "payee: " + error);
        closeNext(e, C, C);                                          // 35
        while (e.height() < 43) closeNext(e, C, C);                  // A's second commit expires at 43
        require(e.rings().commits().empty(), "the unrevealed commit expired");
        tip = e.tip();
        state = e.stateRoot();
        registry = e.rings();
        ledger = e.assets();
    }
    {
        auto opened = Engine::open(dir, policy, Genesis{}, error);
        require(opened.has_value(), "reopen 2: " + error);
        require(opened->tip() == tip && opened->stateRoot() == state && opened->rings() == registry && opened->assets() == ledger,
                "second restart: identical state");
        require(opened->blocks().back().header.extension_root == merkleRootOf(opened->rings(), opened->blocks().back()),
                "extension root covers the registry");
    }
    // Replay from the block file alone.
    {
        Store store;
        require(store.open(dir, error), "store: " + error);
        for (std::uint64_t i = 0; i < store.blockCount(); ++i) {
            Bytes raw;
            Block b;
            require(store.read(i, raw, error) && decode(raw, b, policy.limits, error), "read block");
            require(encode(b) == raw, "block codec round trip");
            all.push_back(b);
        }
        auto replayed = Engine::replay(policy, all, error);
        require(replayed.has_value(), "replay: " + error);
        require(replayed->tip() == tip && replayed->stateRoot() == state && replayed->rings() == registry &&
                replayed->assets() == ledger, "replay: identical state");
    }
    fs::remove_all(tmpl);
    std::cout << "ring persistence: " << checks << " checks passed\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "race";
        if (mode == "race") raceTests();
        else if (mode == "rules") ruleTests();
        else if (mode == "payee") payeeTests();
        else if (mode == "governance") governanceTests();
        else if (mode == "persistence") persistenceTests();
        else throw std::runtime_error("usage: primechain-next-ring-open-tests race|rules|payee|governance|persistence");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ring open test failed: " << e.what() << '\n';
        return 1;
    }
}
