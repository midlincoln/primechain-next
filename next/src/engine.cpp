#include "primechain_next/engine.hpp"

#include "primechain/crypto/signature.hpp"
#include "primechain_next/merkle.hpp"

#include <stdexcept>

namespace primechain::next {
namespace {

Hash256 validatorEpochDigest() {
    Writer w;
    w.text("primechain-next/validator-epoch/unset");  // validator management arrives in A2.2/A3
    return sha3(w.data());
}

Integer powMod(const Integer& base, const Integer& exponent, const Integer& modulus) {
    Integer out;
    mpz_powm(out.get_mpz_t(), base.get_mpz_t(), exponent.get_mpz_t(), modulus.get_mpz_t());
    return out;
}

}  // namespace

Hash256 Policy::digest() const {
    Writer w;
    w.text("primechain-next/policy");
    w.varint(kVersion1);
    w.text(network_id);
    w.varint(limits.max_integer_bytes);
    w.varint(reward_micro_units);
    return sha3(w.data());
}

std::uint64_t reward(const Policy& policy, const Integer&) { return policy.reward_micro_units; }

Engine::Engine(Policy policy) : policy_(std::move(policy)) {
    // D7: genesis seeds the primes 2 (#1) and 3 (#2), with no rewards.
    for (const unsigned long p : {2ul, 3ul}) {
        chain_frontier_ = accepted_frontier_ = Integer(p);
        primes_.insert(Integer(p));
        sealBlock(Integer(p), std::nullopt);
    }
}

Amount Engine::balance(const Address& address, const Integer& asset) const {
    const auto a = balances_.find(asset);
    if (a == balances_.end()) return Amount{};
    const auto b = a->second.find(address);
    return b == a->second.end() ? Amount{} : b->second;
}

Amount Engine::poolBalance(const Integer& asset) const {
    const auto it = pool_.find(asset);
    return it == pool_.end() ? Amount{} : it->second;
}

Hash256 Engine::stateRoot() const {
    Writer w;
    w.text("primechain-next/state");
    w.varint(height_);
    w.integer(chain_frontier_);
    w.integer(accepted_frontier_);
    w.varint(asset_hashes_.size());
    for (const auto& [asset, hash] : asset_hashes_) {
        w.integer(asset);
        w.hash(hash);
    }
    return sha3(w.data());
}

void Engine::refreshAssetHash(const Integer& asset) {
    Writer w;
    w.text("primechain-next/asset");
    w.integer(asset);
    const auto& holders = balances_[asset];
    w.varint(holders.size());
    for (const auto& [address, amount] : holders) {
        w.text(address);
        w.amount(amount);
    }
    w.amount(poolBalance(asset));
    asset_hashes_[asset] = sha3(w.data());
}

bool Engine::checkSigned(const Bytes& payload, const Address& provider, const Bytes& key, const Bytes& signature,
                         std::string& error) const {
    if (provider != primechain::crypto::addressFromProtocolPublicKey(key)) {
        error = "provider address does not match public key";
        return false;
    }
    std::string inner;
    if (!primechain::crypto::verifyProtocolMessageSignature(key, payload, signature, inner)) {
        error = "invalid signature";
        return false;
    }
    return true;
}

bool Engine::checkDivisors(const Integer& start, const Integer& end, const std::vector<Integer>& divisors,
                           std::string& error) const {
    if (end < start) { error = "range end before start"; return false; }
    const Integer length = end - start + 1;
    if (!length.fits_ulong_p() || divisors.size() != length.get_ui()) {
        error = "one divisor per integer is required";
        return false;
    }
    Integer n = start;
    for (const auto& d : divisors) {
        if (d <= 1 || d >= n || mpz_divisible_p(n.get_mpz_t(), d.get_mpz_t()) == 0) {
            error = "invalid divisor for " + n.get_str();
            return false;
        }
        ++n;
    }
    return true;
}

bool Engine::validateRange(const RangeSubmission& r, std::string& error) const {
    if (r.version != kVersion1) { error = "unsupported range version"; return false; }
    if (r.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (r.anchor != tip_) { error = "range is not anchored to the current tip"; return false; }
    if (r.start <= chain_frontier_) { error = "range starts at or below the chain frontier"; return false; }
    if (r.start > accepted_frontier_ + 1 || r.end <= accepted_frontier_) {
        error = "range does not continue the accepted frontier";
        return false;
    }
    if (!checkDivisors(r.start, r.end, r.divisors, error)) return false;
    return checkSigned(rangeSigningPayload(r), r.provider, r.public_key, r.signature, error);
}

bool Engine::submitRange(const RangeSubmission& range, std::string& error) {
    if (!validateRange(range, error)) return false;
    pending_.push_back({arrivals_++, range, submissionHash(range)});
    return true;
}

std::optional<AcceptedRange> Engine::decideNextRange() {
    auto stale = [&](const Pending& p) { return p.range.end <= accepted_frontier_; };
    std::erase_if(pending_, stale);
    if (pending_.empty()) return std::nullopt;
    auto best = pending_.begin();
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
        if (it->range.end > best->range.end || (it->range.end == best->range.end && it->arrival < best->arrival)) best = it;
    const Pending chosen = *best;
    pending_.erase(best);

    AcceptedRange accepted;
    accepted.provider = chosen.range.provider;
    accepted.accepted_start = accepted_frontier_ + 1;
    accepted.end = chosen.range.end;
    accepted.submission_hash = chosen.hash;
    const Integer offset = accepted.accepted_start - chosen.range.start;
    accepted.divisors.assign(chosen.range.divisors.begin() + static_cast<std::ptrdiff_t>(offset.get_ui()),
                             chosen.range.divisors.end());
    applyAccepted(accepted);
    std::erase_if(pending_, stale);
    return accepted;
}

void Engine::applyAccepted(const AcceptedRange& range) {
    Integer n = range.accepted_start;
    for (const auto& d : range.divisors) divisors_.emplace(n++, d);
    accepted_frontier_ = range.end;
    open_ranges_.push_back(range);
}

bool Engine::distinctPrimeFactors(const Integer& n, std::vector<Integer>& out, std::string& error) const {
    std::vector<Integer> stack{n};
    std::set<Integer> found;
    while (!stack.empty()) {
        const Integer m = stack.back();
        stack.pop_back();
        if (m == 1) continue;
        if (primes_.count(m)) { found.insert(m); continue; }
        const auto it = divisors_.find(m);
        if (it == divisors_.end()) {
            error = "integer " + m.get_str() + " is not classified on chain";
            return false;
        }
        stack.push_back(it->second);
        stack.push_back(m / it->second);
    }
    out.assign(found.begin(), found.end());
    return true;
}

bool Engine::validatePrime(const PrimeSubmission& s, std::string& error) const {
    if (s.version != kVersion1) { error = "unsupported prime version"; return false; }
    if (s.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (s.anchor != tip_) { error = "prime is not anchored to the current tip"; return false; }
    if (accepted_frontier_ <= chain_frontier_) { error = "gap has no accepted composites"; return false; }
    if (s.prime != accepted_frontier_ + 1) { error = "gap is not fully covered before the prime"; return false; }
    const Integer& p = s.prime;
    if (s.witness <= 1 || s.witness >= p) { error = "Pratt witness out of range"; return false; }
    const Integer p1 = p - 1;
    if (powMod(s.witness, p1, p) != 1) { error = "Pratt witness fails Fermat condition"; return false; }
    std::vector<Integer> factors;
    if (!distinctPrimeFactors(p1, factors, error)) return false;
    for (const auto& q : factors)
        if (powMod(s.witness, p1 / q, p) == 1) {
            error = "Pratt witness is not a primitive root";
            return false;
        }
    return checkSigned(primeSigningPayload(s), s.provider, s.public_key, s.signature, error);
}

void Engine::credit(const Address* address, const Integer& asset, const Amount& amount) {
    Amount& slot = address ? balances_[asset][*address] : pool_[asset];
    const auto sum = slot.add(amount);
    if (!sum) throw std::overflow_error("balance overflow");
    slot = *sum;
}

std::optional<Block> Engine::closeBlock(const PrimeSubmission& prime, std::string& error) {
    if (!validatePrime(prime, error)) return std::nullopt;
    // D8: 45% prime finder, 45% per composite, 10% validator pool; remainder to the finder.
    const Integer& p = prime.prime;
    const std::uint64_t total = reward(policy_, p);
    const std::uint64_t finder = total / 100 * 45, composites_total = total / 100 * 45;
    const std::uint64_t pool = total - finder - composites_total;
    const Integer composite_count = p - chain_frontier_ - 1;
    const std::uint64_t count = composite_count.get_ui();
    const std::uint64_t share = composites_total / count;
    for (const auto& range : open_ranges_) {
        const Integer n = range.end - range.accepted_start + 1;
        const auto amount = Amount(share).mul(n.get_ui());
        if (!amount) throw std::overflow_error("reward overflow");
        credit(&range.provider, p, *amount);
    }
    credit(&prime.provider, p, Amount(finder + composites_total % count));
    credit(nullptr, p, Amount(pool));
    refreshAssetHash(p);

    chain_frontier_ = p;
    primes_.insert(p);
    pending_.clear();  // every queued batch was anchored to the previous tip
    return sealBlock(p, prime);
}

Block Engine::sealBlock(const Integer& prime, const std::optional<PrimeSubmission>& submission) {
    Block block;
    block.ranges = std::move(open_ranges_);
    open_ranges_.clear();
    block.prime_submission = submission;
    ++height_;
    accepted_frontier_ = prime;
    BlockHeader& h = block.header;
    h.network_id = policy_.network_id;
    h.height = height_;
    h.previous_hash = tip_;
    h.prime = prime;
    h.policy_digest = policy_.digest();
    h.validator_epoch_digest = validatorEpochDigest();
    h.ranges_root = merkleRoot(rangesRootLeaves(block));
    h.transactions_root = merkleRoot({});
    h.state_root = stateRoot();
    tip_ = blockHash(h);
    blocks_.push_back(block);
    return block;
}

std::optional<Engine> Engine::replay(const Policy& policy, const std::vector<Block>& blocks, std::string& error) {
    Engine e(policy);
    if (blocks.size() < 2 || encode(blocks[0]) != encode(e.blocks_[0]) || encode(blocks[1]) != encode(e.blocks_[1])) {
        error = "genesis mismatch";
        return std::nullopt;
    }
    for (std::size_t i = 2; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        const std::string where = " at height " + std::to_string(b.header.height);
        if (b.header.height != e.height_ + 1 || b.header.previous_hash != e.tip_) {
            error = "broken linkage" + where;
            return std::nullopt;
        }
        for (const auto& range : b.ranges) {
            if (range.version != kVersion1 || range.provider.empty() || range.accepted_start != e.accepted_frontier_ + 1 ||
                !e.checkDivisors(range.accepted_start, range.end, range.divisors, error)) {
                if (error.empty()) error = "invalid accepted range";
                error += where;
                return std::nullopt;
            }
            e.applyAccepted(range);
        }
        if (!b.prime_submission) {
            error = "missing prime submission" + where;
            return std::nullopt;
        }
        const auto rebuilt = e.closeBlock(*b.prime_submission, error);
        if (!rebuilt) {
            error += where;
            return std::nullopt;
        }
        if (encode(*rebuilt) != encode(b)) {
            error = "block does not match its replay" + where;
            return std::nullopt;
        }
    }
    return e;
}

}  // namespace primechain::next
