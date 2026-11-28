// B3.4: opening rings. Specification: docs/ring-records.md section 2; docs/rings-design.md R2
// (gate 1, kappa), R3 (eligibility), R4 (size), R5 (commit-reveal, opening rule agreed
// 2026-11-25), R6 (founder record and schedule). Everything here runs for the block being sealed,
// h = height_ + 1, with the parameters in force at h.
//
// Small rules chosen here (proposed, not agreed; listed in next/README.md):
//  - One nonce sequence per address for transfers, ring commits and ring payee changes.
//  - The deposit is exactly parameter RingDeposit in force at the commit's block, in any
//    main-chain prime (0 allowed: then no units move).
//  - A commit may be revealed in blocks height+1 .. height+W, W the reveal window in force at the
//    commit's block (never retroactive); it expires at the start of block height+W+1. Not built:
//    "blocks under a pause of all rings do not count toward the window" (pausing is B3.6).
//  - The deposit of a valid reveal is refunded in the block of the reveal, also for a loser.
//  - A reveal that matches its commitment but fails (type, size, encoding, eligibility,
//    certificate, ring already open before the commit's block) forfeits the deposit to the fee
//    pool at once and settles the commit, so it no longer delays later ones.
//  - A reveal whose ring was opened in or after its commit's block is a valid loser (refunded).
//  - Eligibility uses the chain frontier at the end of the previous block.
//  - kappa: a commit is refused while the rings opened in the current epoch reach kappa. Commits
//    already made may still open, so an epoch can exceed kappa by the commits in flight.
//  - RING_PAYEE takes effect from its own block; the last change in a block counts.
#include "primechain_next/engine.hpp"

#include "primechain/crypto/signature.hpp"

#include <stdexcept>

namespace primechain::next {

// R3 makes every prime factor of an eligible |D| <= F^2 checkable from the chain: one at or below
// the frontier is on chain (or classified composite); above it, trial division by the chain's
// primes up to its square root (<= F) decides. Exact, so no Pratt certificate is needed.
bool Engine::chainPrime(const Integer& n) const {
    if (n < 2) return false;
    if (n <= chain_frontier_) return primes_.count(n) != 0;
    if (n > chain_frontier_ * chain_frontier_) return false;
    for (const auto& q : primes_) {
        if (q * q > n) return true;
        if (mpz_divisible_p(n.get_mpz_t(), q.get_mpz_t()) != 0) return false;
    }
    return false;
}

// ---------------------------------------------------------------- RING_COMMIT

bool Engine::checkRingCommitForm(const RingCommit& c, std::string& error) const {
    if (c.version != kVersion1) { error = "unsupported ring commit version"; return false; }
    if (c.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (c.discoverer.empty()) { error = "missing discoverer"; return false; }
    if (c.nonce == 0) { error = "nonces start at 1"; return false; }
    if (!primes_.count(c.deposit_asset)) { error = "unknown deposit asset"; return false; }
    return checkSigned(ringCommitSigningPayload(c), c.discoverer, c.public_key, c.signature, error);
}

bool Engine::submitRingCommit(const RingCommit& c, std::string& error) {
    // R2: while gate 1 is closed a commit is rejected on arrival, and nothing is taken.
    if (parameter(Param::RingGateOpen) == 0) { error = "gate 1 is closed: no new rings"; return false; }
    if (!checkRingCommitForm(c, error)) return false;
    if (c.nonce <= nonce(c.discoverer)) { error = "nonce already used"; return false; }
    pending_ring_commits_.push_back(c);
    return true;
}

bool Engine::applyRingCommit(const RingCommit& c, std::uint64_t index, std::string& error) {
    const std::uint64_t h = height_ + 1;
    if (parameter(Param::RingGateOpen) == 0) { error = "gate 1 is closed: no new rings"; return false; }
    const std::uint64_t epoch = static_cast<std::uint64_t>(parameter(Param::EpochLength));
    const std::uint64_t epoch_start = (h - 1) / epoch * epoch + 1;
    if (registry_.openedSince(epoch_start) >= parameter(Param::RingsPerEpoch)) {
        error = "the rings of this epoch are used up (kappa)";
        return false;
    }
    if (c.deposit != Amount::fromRaw(parameter(Param::RingDeposit))) { error = "deposit differs from the ring deposit"; return false; }
    if (c.nonce != nonce(c.discoverer) + 1) { error = "nonce out of order"; return false; }
    const AssetId asset = AssetId::main(c.deposit_asset);
    if (ledger_.owned(asset, c.discoverer) < c.deposit) { error = "insufficient balance for the deposit"; return false; }
    if (!c.public_key.empty() && !registerKey(c.discoverer, c.public_key, error)) return false;
    // The deposit is held by nobody (ring-records section 2).
    if (const SpendRefusal refused = ledger_.debit(asset, c.discoverer, c.deposit, h); refused != SpendRefusal::None)
        throw std::logic_error(std::string("ledger refused a checked deposit: ") + refusalText(refused));
    nonces_[c.discoverer] = c.nonce;
    OpenCommit open;
    open.height = h;
    open.index = index;
    open.discoverer = c.discoverer;
    open.commitment = c.commitment;
    open.deposit_asset = c.deposit_asset;
    open.deposit = c.deposit;
    open.expires_after = h + static_cast<std::uint64_t>(parameter(Param::RingRevealWindow));
    registry_.addCommit(std::move(open));
    refreshAssetHash(c.deposit_asset);
    return true;
}

// ---------------------------------------------------------------- RING_PAYEE

bool Engine::checkRingPayeeForm(const RingPayee& p, std::string& error) const {
    if (p.version != kVersion1) { error = "unsupported ring payee version"; return false; }
    if (p.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (p.payee.empty()) { error = "missing payee"; return false; }
    if (p.nonce == 0) { error = "nonces start at 1"; return false; }
    const RingEntry* r = registry_.ring(p.ring);
    if (!r) { error = "unknown ring"; return false; }
    // Signed by the current payee (R6).
    return checkSigned(ringPayeeSigningPayload(p), r->payees.back().second, p.public_key, p.signature, error);
}

bool Engine::submitRingPayee(const RingPayee& p, std::string& error) {
    if (!checkRingPayeeForm(p, error)) return false;
    if (p.nonce <= nonce(registry_.ring(p.ring)->payees.back().second)) { error = "nonce already used"; return false; }
    pending_ring_payees_.push_back(p);
    return true;
}

bool Engine::applyRingPayee(const RingPayee& p, std::string& error) {
    const std::uint64_t h = height_ + 1;
    const Address signer = registry_.ring(p.ring)->payees.back().second;  // the form check found the ring
    if (p.nonce != nonce(signer) + 1) { error = "nonce out of order"; return false; }
    if (!p.public_key.empty() && !registerKey(signer, p.public_key, error)) return false;
    if (!registry_.setPayee(p.ring, h, p.payee, error)) return false;
    if (!ledger_.setPayee(p.ring, h, p.payee, error)) throw std::logic_error("ledger refused a checked payee: " + error);
    nonces_[signer] = p.nonce;
    return true;
}

// ---------------------------------------------------------------- RING_REVEAL

// Checked against an unexpired sealed commit first, so an unmatched reveal costs nothing.
const OpenCommit* Engine::matchReveal(const RingReveal& v, std::uint64_t h, std::string& error) const {
    if (v.version != kVersion1) { error = "unsupported ring reveal version"; return nullptr; }
    if (v.salt.size() != kRingSaltBytes) { error = "bad salt"; return nullptr; }
    const OpenCommit* c = registry_.commit(v.commit_height, v.commit_index);
    if (!c || c->revealed) { error = "no sealed commit at that reference"; return nullptr; }
    if (c->height >= h) { error = "a commit is revealed in a later block"; return nullptr; }
    if (h > c->expires_after) { error = "commit expired"; return nullptr; }
    const Hash256 cert_hash = certificateHash(v.certificate_type, v.certificate_version, v.certificate);
    if (ringCommitment(policy_.network_id, c->discoverer, v.salt, cert_hash) != c->commitment) {
        error = "reveal does not match the commitment";
        return nullptr;
    }
    return c;
}

bool Engine::submitRingReveal(const RingReveal& v, std::string& error) {
    if (!matchReveal(v, height_ + 1, error)) return false;
    for (const auto& p : pending_ring_reveals_)
        if (p.commit_height == v.commit_height && p.commit_index == v.commit_index) { error = "commit already revealed"; return false; }
    pending_ring_reveals_.push_back(v);
    return true;
}

bool Engine::applyRingReveal(const RingReveal& v, std::string& error) {
    const std::uint64_t h = height_ + 1;
    const OpenCommit* matched = matchReveal(v, h, error);
    if (!matched) return false;
    OpenCommit& c = *registry_.commit(matched->height, matched->index);
    const AssetId asset = AssetId::main(c.deposit_asset);
    // Then: type, size, encoding, eligibility (R3), the PID certificate (B3.2), the registry.
    std::string why;
    QuadCertificate cert;
    const CertificateLimits limits{static_cast<std::uint64_t>(parameter(Param::CertificateBytes)),
                                   static_cast<std::uint64_t>(parameter(Param::CertificateIntegerBytes)),
                                   static_cast<std::uint64_t>(parameter(Param::RingEligibilityExponent))};
    if (v.certificate_type != kQuadraticCertificateType || v.certificate_version != kVersion1) {
        why = "unknown certificate type or version";
    } else if (v.certificate.size() > limits.certificate_bytes) {
        why = "certificate exceeds the size limit";
    } else if (!decode(v.certificate, cert, limits, why)) {
        why = "certificate encoding: " + why;
    } else if (!eligible(cert.d, chain_frontier_, limits.eligibility_exponent)) {
        why = "ring not eligible at this frontier";
    } else {
        const std::vector<Integer> primes(primes_.begin(), primes_.end());
        const PrimeOracle oracle = [this](const Integer& n) { return chainPrime(n); };
        if (!verifyCertificate(cert, chain_frontier_, primes, oracle, limits, why)) {
            why = "certificate: " + why;
        } else if (const RingEntry* open = registry_.byId(quadraticRingId(cert.d)); open && open->opened < c.height) {
            why = "ring already open when committed";
        }
    }
    if (!why.empty()) {  // matched but invalid: the deposit goes to the fee pool, the commit is settled
        ledger_.creditPool(asset, c.deposit);
        refreshAssetHash(c.deposit_asset);
        ring_outcomes_.push_back({RingOutcome::Kind::Forfeited, c.height, c.index, 0, why});
        registry_.eraseCommit(c.height, c.index);
        return true;
    }
    // Valid: the deposit is returned now, also if an earlier commit gets the ring.
    ledger_.credit(asset, c.discoverer, c.deposit, h);
    refreshAssetHash(c.deposit_asset);
    c.revealed = true;
    c.revealed_in = h;
    c.ring_id = quadraticRingId(cert.d);
    c.certificate_type = v.certificate_type;
    c.certificate_version = v.certificate_version;
    c.certificate_hash = certificateHash(v.certificate_type, v.certificate_version, v.certificate);
    c.d = cert.d;
    c.unit = cert.unit;
    ring_outcomes_.push_back({RingOutcome::Kind::Refunded, c.height, c.index, 0, {}});
    return true;
}

// ---------------------------------------------------------------- expiry and opening (R5)

void Engine::expireCommits() {
    const std::uint64_t h = height_ + 1;
    std::vector<RingRegistry::Key> expired;
    for (const auto& [key, c] : registry_.commits())
        if (!c.revealed && c.expires_after < h) expired.push_back(key);
    for (const auto& [ch, ci] : expired) {
        const OpenCommit& c = *registry_.commit(ch, ci);
        ledger_.creditPool(AssetId::main(c.deposit_asset), c.deposit);
        refreshAssetHash(c.deposit_asset);
        ring_outcomes_.push_back({RingOutcome::Kind::Expired, ch, ci, 0, {}});
        registry_.eraseCommit(ch, ci);
    }
}

// A revealed ring opens once no earlier commit is still sealed; commits in one block count in
// block order; the earliest revealed commit for a ring gets it, later ones are losers.
void Engine::openRings() {
    const std::uint64_t h = height_ + 1;
    while (!registry_.commits().empty()) {
        const OpenCommit c = registry_.commits().begin()->second;
        if (!c.revealed) break;  // still sealed: every later commit waits
        registry_.eraseCommit(c.height, c.index);
        if (const RingEntry* won = registry_.byId(c.ring_id)) {  // a loser; its deposit was returned at the reveal
            ring_outcomes_.push_back({RingOutcome::Kind::Lost, c.height, c.index, won->number, {}});
            continue;
        }
        RingEntry e;
        e.number = registry_.nextNumber();
        e.ring_id = c.ring_id;
        e.certificate_type = c.certificate_type;
        e.certificate_version = c.certificate_version;
        e.certificate_hash = c.certificate_hash;
        e.d = c.d;
        e.unit = c.unit;
        e.founder = c.discoverer;
        e.payees = {{h, c.discoverer}};
        e.terms = ringTermsAt(h);  // R6, rule 4: fixed for this ring from now on
        e.opened = h;
        e.commit_height = c.height;
        e.commit_index = c.index;
        e.frontier = 0;
        e.tip = ringGenesisHash(e.number, e.ring_id, e.certificate_hash, h);
        std::string error;
        if (!ledger_.openRing(e.number, e.terms, h, e.founder, error)) throw std::logic_error("ledger refused a ring: " + error);
        ring_outcomes_.push_back({RingOutcome::Kind::Opened, c.height, c.index, e.number, {}});
        registry_.open(std::move(e));
    }
}

}  // namespace primechain::next
