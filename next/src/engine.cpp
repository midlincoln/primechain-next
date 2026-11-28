#include "primechain_next/engine.hpp"

#include "primechain/crypto/signature.hpp"
#include "primechain_next/merkle.hpp"

#include <algorithm>
#include <stdexcept>

namespace primechain::next {
namespace {

Integer powMod(const Integer& base, const Integer& exponent, const Integer& modulus) {
    Integer out;
    mpz_powm(out.get_mpz_t(), base.get_mpz_t(), exponent.get_mpz_t(), modulus.get_mpz_t());
    return out;
}

bool verify(const Bytes& key, const Bytes& payload, const Bytes& signature) {
    std::string ignored;
    return primechain::crypto::verifyProtocolMessageSignature(key, payload, signature, ignored);
}

std::string grouped(u128 v) {  // 1024 -> "1,024"
    std::string digits = Amount::fromRaw(v).str(), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

}  // namespace

ParameterValues Policy::genesisParameters() const {
    ParameterValues v = defaultParameters();
    auto put = [&](Param id, u128 value) { v[static_cast<std::uint32_t>(id)] = value; };
    put(Param::MainReward, reward_micro_units);
    put(Param::MinFee, min_fee_micro_units);
    put(Param::MaxOutputs, max_outputs);
    put(Param::EpochLength, epoch_length);
    put(Param::GovernanceNotice, governance_notice);
    put(Param::ProposerSkipAfter, proposer_skip_after);
    for (const auto& [id, value] : parameters) v[id] = value;  // unknown ids are caught by checkParameters
    return v;
}

Hash256 Policy::digest() const {
    Writer w;
    w.text("primechain-next/policy");
    w.varint(kVersion1);
    w.text(network_id);
    w.varint(limits.max_integer_bytes);
    w.hash(parametersDigest(genesisParameters()));
    return sha3(w.data());
}

Engine::Engine(Policy policy, Genesis genesis) : policy_(std::move(policy)), validators_(std::move(genesis.validators)) {
    if (policy_.snapshot_interval == 0) throw std::invalid_argument("snapshot interval must be positive");
    const ParameterValues start = policy_.genesisParameters();
    if (const std::string why = checkParameters(start); !why.empty())
        throw std::invalid_argument("genesis parameters: " + why);
    params_ = ParameterTable(start);
    // D18: each genesis validator binds its consensus key to its payout address; both sign.
    std::set<Hash256> consensus_keys;
    for (const auto& v : validators_) {
        const Bytes payload = validatorBindingPayload(policy_.network_id, v);
        if (v.version != kVersion1 || v.consensus_key.empty() || v.payout_address.empty() ||
            v.payout_address != primechain::crypto::addressFromProtocolPublicKey(v.payout_key) ||
            !verify(v.consensus_key, payload, v.consensus_signature) || !verify(v.payout_key, payload, v.payout_signature) ||
            !consensus_keys.insert(sha3(v.consensus_key)).second)
            throw std::invalid_argument("invalid genesis validator binding");
    }
    std::sort(validators_.begin(), validators_.end(), [](const ValidatorRecord& a, const ValidatorRecord& b) {
        return a.payout_address != b.payout_address ? a.payout_address < b.payout_address
                                                    : sha3(a.consensus_key) < sha3(b.consensus_key);
    });
    genesis_digest_ = validatorEpochDigest();
    std::vector<Address> payouts;
    for (const auto& v : validators_) payouts.push_back(v.payout_address);
    ledger_.setValidators(0, payouts);
    // D7: genesis seeds the primes 2 (#1) and 3 (#2), with no rewards (nominal issue 0: no holders).
    for (const unsigned long p : {2ul, 3ul}) {
        chain_frontier_ = accepted_frontier_ = Integer(p);
        primes_.insert(Integer(p));
        std::string error;
        if (!ledger_.addMainPrime(Integer(p), height_ + 1, Amount{}, error)) throw std::logic_error(error);
        sealBlock(Integer(p), std::nullopt);
    }
}

Amount Engine::balance(const Address& address, const Integer& asset) const {
    return ledger_.owned(AssetId::main(asset), address);
}

Amount Engine::feePoolBalance(const Integer& asset) const { return ledger_.pool(AssetId::main(asset)); }

RingTerms Engine::ringTermsAt(std::uint64_t height) const {
    RingTerms t;
    t.prime_value = parameterAt(Param::RingPrimeValue, height);
    t.upstream_bp = parameterAt(Param::RingUpstreamShareBp, height);
    t.founder_bp = parameterAt(Param::RingFounderShareBp, height);
    t.kept_bp = parameterAt(Param::RingKeptDownBp, height);
    t.backlog_bp = parameterAt(Param::RingBacklogShareBp, height);  // [19] A4 backlog (B3.5)
    return t;
}

std::uint64_t Engine::nonce(const Address& address) const {
    const auto it = nonces_.find(address);
    return it == nonces_.end() ? 0 : it->second;
}

Hash256 Engine::validatorEpochDigest() const {
    Writer w;
    w.text("primechain-next/validator-epoch");
    w.varint(validators_.size());
    for (const auto& v : validators_) w.bytes(encode(v));
    return sha3(w.data());
}

Hash256 Engine::stateRoot() const {
    Writer w;
    w.text("primechain-next/state");
    w.varint(height_);
    w.integer(chain_frontier_);
    w.integer(accepted_frontier_);
    w.hash(validatorEpochDigest());
    w.varint(asset_hashes_.size());
    for (const auto& [asset, hash] : asset_hashes_) {
        w.integer(asset);
        w.hash(hash);
    }
    w.varint(keys_.size());
    for (const auto& [address, key] : keys_) {
        w.text(address);
        w.hash(sha3(key));
    }
    w.varint(nonces_.size());
    for (const auto& [address, n] : nonces_) {
        w.text(address);
        w.varint(n);
    }
    if (!ring_asset_hashes_.empty()) {  // B3.5: ring primes that moved (absent: the root is unchanged)
        w.text("primechain-next/ring-assets");
        w.varint(ring_asset_hashes_.size());
        for (const auto& [x, hash] : ring_asset_hashes_) {
            encode(w, x);
            w.hash(hash);
        }
    }
    w.hash(governanceDigest());
    return sha3(w.data());
}

Hash256 Engine::governanceDigest() const {
    Writer w;
    w.text("primechain-next/governance-state");
    w.hash(params_.historyDigest());
    w.varint(last_sequence_);
    w.varint(tallies_.size());
    for (const auto& [id, tally] : tallies_) {
        w.hash(id);
        w.varint(tally.voters.size());
        for (const auto& v : tally.voters) w.hash(v);
    }
    w.varint(ready_.size());
    for (const auto& [id, ready] : ready_) {
        w.hash(id);
        w.varint(ready.height);
        w.varint(ready.expiry);
        w.varint(ready.sequence);
    }
    w.varint(scheduled_.size());
    for (const auto& [height, proposal] : scheduled_) {
        w.varint(height);
        w.hash(proposalId(proposal));
    }
    return sha3(w.data());
}

std::size_t Engine::votesFor(const Hash256& id) const {
    const auto it = tallies_.find(id);
    return it == tallies_.end() ? 0 : it->second.voters.size();
}

void Engine::refreshAssetHash(const Integer& asset) { asset_hashes_[asset] = ledger_.assetDigest(AssetId::main(asset)); }

void Engine::refreshAsset(const AssetId& x) {
    if (x.isMain()) refreshAssetHash(x.q);
    else ring_asset_hashes_[x] = ledger_.assetDigest(x);  // B3.5: ring primes moved by transfers
}

bool Engine::checkSigned(const Bytes& payload, const Address& provider, const Bytes& key, const Bytes& signature,
                         std::string& error) const {
    const Bytes* use = &key;
    if (key.empty()) {  // a registered key may be omitted
        const auto it = keys_.find(provider);
        if (it == keys_.end()) {
            error = "unknown sender: public key required on first use";
            return false;
        }
        use = &it->second;
    } else if (provider != primechain::crypto::addressFromProtocolPublicKey(key)) {
        error = "address does not match public key";
        return false;
    }
    if (!verify(*use, payload, signature)) {
        error = "invalid signature";
        return false;
    }
    return true;
}

bool Engine::checkDivisors(const Integer& start, const Integer& end, const std::vector<Integer>& divisors,
                           std::string& error) const {
    if (end < start) { error = "range end before start"; return false; }
    const Integer length = end - start + 1;
    const u128 max_length = parameter(Param::MaxRangeComposites);
    if (length > Integer(static_cast<unsigned long>(max_length))) {
        error = "range is longer than " + grouped(max_length) + " composites";
        return false;
    }
    if (integerMagnitude(end).size() > parameter(Param::MaxIntegerBytes)) { error = "integer too large"; return false; }
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
    std::string ignored;
    if (!chosen.range.public_key.empty()) registerKey(chosen.range.provider, chosen.range.public_key, ignored);

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

// ---------------------------------------------------------------- transfers (D16)

bool Engine::checkTransferForm(const Transfer& t, std::string& error) const {
    if (t.version != kVersion1 && t.version != kTransferVersionRings) { error = "unsupported transfer version"; return false; }
    // B3.5: version 2 names assets (ring, q, i); version 1 only main-chain primes.
    const bool rings = t.version == kTransferVersionRings;
    auto idOf = [&](const Integer& q, std::uint64_t ring, std::uint32_t index) { return AssetId{ring, q, index}; };
    auto known = [&](const AssetId& x) { return x.isMain() ? x.i == 0 && primes_.count(x.q) != 0 : ledger_.exists(x); };
    if (!rings) {
        bool any = t.fee_ring != 0 || t.fee_index != 0;
        for (const auto& in : t.inputs) any = any || in.ring != 0 || in.index != 0;
        for (const auto& out : t.outputs) any = any || out.ring != 0 || out.index != 0;
        if (any) { error = "ring primes need transfer version 2"; return false; }
    }
    if (t.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (t.sender.empty()) { error = "missing sender"; return false; }
    if (t.nonce == 0) { error = "nonces start at 1"; return false; }
    if (t.inputs.empty() || t.outputs.empty()) { error = "transfer needs inputs and outputs"; return false; }
    if (t.outputs.size() > parameter(Param::MaxOutputs)) { error = "too many outputs"; return false; }
    if (t.fee < Amount::fromRaw(parameter(Param::MinFee))) { error = "fee below minimum"; return false; }
    std::map<AssetId, Amount> spent;  // per asset: outputs + fee
    auto add = [&](const AssetId& asset, const Amount& amount) {
        const auto sum = spent[asset].add(amount);
        if (!sum) return false;
        spent[asset] = *sum;
        return true;
    };
    for (const auto& out : t.outputs) {
        const AssetId x = idOf(out.asset, out.ring, out.index);
        if (out.amount == Amount{} || out.receiver.empty()) { error = "empty output"; return false; }
        if (!known(x)) { error = "unknown asset " + x.str(); return false; }
        if (!add(x, out.amount)) { error = "amount overflow"; return false; }
    }
    const AssetId fee_x = idOf(t.fee_asset, t.fee_ring, t.fee_index);
    if (!known(fee_x)) { error = "unknown fee asset"; return false; }
    if (!add(fee_x, t.fee)) { error = "amount overflow"; return false; }
    for (std::size_t i = 0; i < t.inputs.size(); ++i) {
        const auto& in = t.inputs[i];
        const AssetId x = idOf(in.asset, in.ring, in.index);
        if (i > 0 && !(idOf(t.inputs[i - 1].asset, t.inputs[i - 1].ring, t.inputs[i - 1].index) < x)) {
            error = "inputs must be in strictly increasing asset order";
            return false;
        }
        if (in.amount == Amount{}) { error = "empty input"; return false; }
        if (!known(x)) { error = "unknown asset " + x.str(); return false; }
        const auto it = spent.find(x);
        if (it == spent.end() || it->second != in.amount) { error = "conservation violated for asset " + x.str(); return false; }
        spent.erase(it);
    }
    if (!spent.empty()) { error = "conservation violated: output or fee asset without input"; return false; }
    return checkSigned(transferSigningPayload(t), t.sender, t.public_key, t.signature, error);
}

bool Engine::submitTransfer(const Transfer& t, std::string& error) {
    if (!checkTransferForm(t, error)) return false;
    if (t.nonce <= nonce(t.sender)) { error = "nonce already used"; return false; }
    pending_transfers_.push_back(t);
    return true;
}

bool Engine::applyTransfer(const Transfer& t, std::string& error) {
    if (t.nonce != nonce(t.sender) + 1) { error = "nonce out of order"; return false; }
    for (const auto& in : t.inputs) {
        const AssetId x{in.ring, in.asset, in.index};
        if (ledger_.owned(x, t.sender) < in.amount) { error = "insufficient balance in asset " + x.str(); return false; }
    }
    if (!t.public_key.empty() && !registerKey(t.sender, t.public_key, error)) return false;
    // The form check made the inputs exactly the outputs plus the fee, per asset.
    std::vector<std::pair<AssetId, Amount>> debits;
    for (const auto& in : t.inputs) debits.emplace_back(AssetId{in.ring, in.asset, in.index}, in.amount);
    std::vector<AssetLedger::Credit> credits;
    for (const auto& out : t.outputs) credits.push_back({AssetId{out.ring, out.asset, out.index}, out.receiver, out.amount});
    const SpendRefusal refused =
        ledger_.spend(t.sender, debits, credits, AssetId{t.fee_ring, t.fee_asset, t.fee_index}, t.fee, height_ + 1);
    if (refused != SpendRefusal::None)
        throw std::logic_error(std::string("ledger refused a checked transfer: ") + refusalText(refused));
    nonces_[t.sender] = t.nonce;
    for (const auto& [x, amount] : debits) refreshAsset(x);
    return true;
}

// ---------------------------------------------------------------- governance (A3-2)

bool Engine::checkGovernanceForm(const GovernanceTx& tx, std::string& error) const {
    const auto& p = tx.proposal;
    if (tx.version != kVersion1 || p.version != kVersion1) { error = "unsupported governance version"; return false; }
    if (p.network_id != policy_.network_id) { error = "governance: wrong network"; return false; }
    if (p.kind == GovernanceKind::ParamChange) {
        // D20: a non-empty list in increasing id order; known parameters; new values within the
        // hard bounds and different from the old ones. The old values are checked against the
        // table at inclusion and again at activation.
        if (p.changes.empty() || p.changes.size() > parameterSpecs().size()) { error = "governance: bad change list"; return false; }
        for (std::size_t i = 0; i < p.changes.size(); ++i) {
            const auto& c = p.changes[i];
            const ParamSpec* spec = findParam(c.id);
            if (!spec) { error = "governance: unknown parameter"; return false; }
            if (i > 0 && c.id <= p.changes[i - 1].id) { error = "governance: changes must be in increasing id order"; return false; }
            if (c.new_value < spec->min || c.new_value > spec->max) { error = std::string("governance: out of bounds: ") + spec->name; return false; }
            if (c.new_value == c.old_value) { error = "governance: change without effect"; return false; }
        }
    }
    if (!checkPauseProposal(p, error)) return false;
    if (tx.kind == GovernanceTxKind::Ready) {
        if (p.kind != GovernanceKind::ValidatorAdd || tx.signer_key != p.validator.consensus_key) {
            error = "governance: ready must be signed by the validator being added";
            return false;
        }
    } else if (tx.kind != GovernanceTxKind::Vote) {
        error = "governance: unknown transaction kind";
        return false;
    }
    if (!verify(tx.signer_key, governanceSigningPayload(tx), tx.signature)) {
        error = "governance: invalid signature";
        return false;
    }
    return true;
}

bool Engine::submitGovernance(const GovernanceTx& tx, std::string& error) {
    if (!checkGovernanceForm(tx, error)) return false;
    pending_governance_.push_back(tx);
    return true;
}

// B3.6 (R2): a pause of one ring names a ring number; an emergency pause names one ring or all
// (0) and the hash of the fault report that shows the verifier faulty (nonzero; it is on chain in
// every vote). An emergency can only pause; unpausing always takes the notice.
bool Engine::checkPauseProposal(const GovernanceProposal& p, std::string& error) const {
    if (p.kind == GovernanceKind::RingPause && p.ring == 0) { error = "governance: a ring pause names a ring"; return false; }
    if (p.kind == GovernanceKind::EmergencyPause && p.evidence == Hash256{}) {
        error = "governance: an emergency pause needs the hash of its fault report";
        return false;
    }
    return true;
}

bool Engine::isScheduled(const Hash256& id) const {
    return std::any_of(scheduled_.begin(), scheduled_.end(), [&](const auto& e) { return proposalId(e.second) == id; });
}

// A ready message is useless once its proposal can no longer pass (expired at `height`, or its
// sequence is used up) and it is not waiting to take effect.
bool Engine::readyDead(const Hash256& id, std::uint64_t expiry, std::uint64_t sequence, std::uint64_t height) const {
    return (expiry < height || sequence <= last_sequence_) && !isScheduled(id);
}

// State checks at inclusion in the block being sealed (height_ + 1).
bool Engine::applyGovernance(const GovernanceTx& tx, std::string& error) {
    const std::uint64_t h = height_ + 1;
    const Hash256 id = proposalId(tx.proposal);
    const auto& p = tx.proposal;
    if (tx.kind == GovernanceTxKind::Ready) {
        if (ready_.count(id)) { error = "governance: ready already recorded"; return false; }
        if (readyDead(id, p.expiry_height, p.sequence, h)) { error = "governance: ready for a proposal that cannot pass"; return false; }
        ready_.emplace(id, Ready{h, p.expiry_height, p.sequence});
        return true;
    }
    if (p.validator_set_digest != validatorEpochDigest()) { error = "governance: vote for another validator set"; return false; }
    if (h > p.expiry_height) { error = "governance: proposal expired"; return false; }
    if (p.sequence <= last_sequence_) { error = "governance: sequence already used"; return false; }
    const Hash256 voter = sha3(tx.signer_key);
    const bool member = std::any_of(validators_.begin(), validators_.end(),
                                    [&](const ValidatorRecord& v) { return sha3(v.consensus_key) == voter; });
    if (!member) { error = "governance: voter is not an active validator"; return false; }
    if (p.kind == GovernanceKind::ParamChange) {  // stale or inconsistent with the table in force now
        ParameterValues next = params_.valuesAt(h);
        for (const auto& c : p.changes) {
            if (next.at(c.id) != c.old_value) { error = "governance: old value does not match the table"; return false; }
            next[c.id] = c.new_value;
        }
        if (const std::string why = checkParameters(next); !why.empty()) { error = "governance: " + why; return false; }
    }
    if ((p.kind == GovernanceKind::RingPause || p.kind == GovernanceKind::EmergencyPause) && p.ring != 0 &&
        !registry_.ring(p.ring)) {
        error = "governance: unknown ring";
        return false;
    }
    auto& tally = tallies_[id];
    tally.proposal = p;
    if (!tally.voters.insert(voter).second) { error = "governance: duplicate vote"; return false; }
    if (tally.voters.size() >= quorum(validators_.size())) {
        const std::uint64_t notice = static_cast<std::uint64_t>(params_.value(Param::GovernanceNotice, h));
        // R2: an emergency pause takes effect at once, from the next block (the batch of this block
        // was decided before its prime); every other change after the notice.
        const std::uint64_t effective =
            p.kind == GovernanceKind::EmergencyPause ? h + 1 : std::max(p.effective_height, h + notice);
        scheduled_.emplace_back(effective, p);
        last_sequence_ = std::max(last_sequence_, p.sequence);
        tallies_.erase(id);
    }
    return true;
}

void Engine::activateScheduled(std::uint64_t height) {
    std::vector<std::pair<std::uint64_t, GovernanceProposal>> later;
    bool set_changed = false;
    for (auto& [effective, p] : scheduled_) {
        if (effective > height) { later.emplace_back(effective, std::move(p)); continue; }
        const Hash256 id = proposalId(p);
        if (p.kind == GovernanceKind::ParamChange) {
            // Applied only if every old value still holds (an earlier change may have moved it);
            // otherwise the whole change is dropped. Never retroactive: in force from `height` on.
            ParameterValues next = params_.valuesAt(height), changed;
            bool current = true;
            for (const auto& c : p.changes) {
                current &= next.at(c.id) == c.old_value;
                next[c.id] = changed[c.id] = c.new_value;
            }
            if (current && checkParameters(next).empty()) params_.set(height, changed);
        } else if (p.kind == GovernanceKind::ValidatorAdd) {
            // A3-3: a newcomer counts only if its signed "ready" was included before `height`.
            const auto ready = ready_.find(id);
            const auto& v = p.validator;
            const Bytes payload = validatorBindingPayload(policy_.network_id, v);
            const bool duplicate = std::any_of(validators_.begin(), validators_.end(),
                                               [&](const ValidatorRecord& x) { return x.consensus_key == v.consensus_key; });
            if (ready != ready_.end() && ready->second.height < height && !duplicate && v.version == kVersion1 &&
                v.payout_address == primechain::crypto::addressFromProtocolPublicKey(v.payout_key) &&
                verify(v.consensus_key, payload, v.consensus_signature) && verify(v.payout_key, payload, v.payout_signature)) {
                validators_.push_back(v);
                set_changed = true;
            }
        } else if (p.kind == GovernanceKind::RingPause) {  // B3.6 (R2), after the notice
            registry_.setPaused(p.ring, p.pause);
        } else if (p.kind == GovernanceKind::EmergencyPause) {  // B3.6 (R2), immediate
            if (p.ring != 0) registry_.setPaused(p.ring, true);
            else if (params_.value(Param::RingsPaused, height) == 0)
                params_.set(height, {{static_cast<std::uint32_t>(Param::RingsPaused), 1}});
        } else if (p.kind == GovernanceKind::ValidatorRemove && validators_.size() > 1) {
            const auto before = validators_.size();
            std::erase_if(validators_, [&](const ValidatorRecord& x) { return sha3(x.consensus_key) == p.remove_key_hash; });
            set_changed = validators_.size() != before;
        }
        ready_.erase(id);
    }
    scheduled_ = std::move(later);
    if (set_changed) {
        std::sort(validators_.begin(), validators_.end(), [](const ValidatorRecord& a, const ValidatorRecord& b) {
            return a.payout_address != b.payout_address ? a.payout_address < b.payout_address
                                                        : sha3(a.consensus_key) < sha3(b.consensus_key);
        });
        std::vector<Address> payouts;  // ring shares of 2 and 3 go to the set active at the row's block (R11)
        for (const auto& v : validators_) payouts.push_back(v.payout_address);
        ledger_.setValidators(height, payouts);
    }
}

// ---------------------------------------------------------------- closing prime, rewards, distribution

bool Engine::checkPrimeArithmetic(const Integer& p, const Integer& witness, std::string& error) const {
    if (integerMagnitude(p).size() > parameter(Param::MaxIntegerBytes)) { error = "prime too large"; return false; }
    if (accepted_frontier_ <= chain_frontier_) { error = "gap has no accepted composites"; return false; }
    if (p != accepted_frontier_ + 1) { error = "gap is not fully covered before the prime"; return false; }
    if (witness <= 1 || witness >= p) { error = "Pratt witness out of range"; return false; }
    const Integer p1 = p - 1;
    if (powMod(witness, p1, p) != 1) { error = "Pratt witness fails Fermat condition"; return false; }
    std::vector<Integer> factors;
    if (!distinctPrimeFactors(p1, factors, error)) return false;
    for (const auto& q : factors)
        if (powMod(witness, p1 / q, p) == 1) {
            error = "Pratt witness is not a primitive root";
            return false;
        }
    return true;
}

bool Engine::validatePrime(const PrimeSubmission& s, std::string& error) const {
    if (s.version != kVersion1) { error = "unsupported prime version"; return false; }
    if (s.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (s.anchor != tip_) { error = "prime is not anchored to the current tip"; return false; }
    if (!checkPrimeArithmetic(s.prime, s.witness, error)) return false;
    return checkSigned(primeSigningPayload(s), s.provider, s.public_key, s.signature, error);
}

bool Engine::registerKey(const Address& address, const Bytes& key, std::string& error) {
    if (address != primechain::crypto::addressFromProtocolPublicKey(key)) {
        error = "key record address does not match its key";
        return false;
    }
    if (keys_.count(address)) return true;  // already registered; nothing to add
    keys_.emplace(address, key);
    new_keys_.push_back({address, key});
    return true;
}

// D17: pay the fee pool out in equal shares to the validators' payout addresses (sorted order);
// the remainder goes one unit each to the first validators in that order.
void Engine::distributePools() {
    std::vector<Address> payouts;  // none configured: the pool keeps accumulating
    for (const auto& v : validators_) payouts.push_back(v.payout_address);
    for (const auto& asset : ledger_.distributePool(payouts, height_ + 1)) refreshAsset(asset);
}

std::optional<Block> Engine::closeBlock(const PrimeSubmission& prime, std::string& error) {
    if (!validatePrime(prime, error)) return std::nullopt;
    // B3.6: reveals ride in ring batches. Without consensus, the queued reveals go into a batch of
    // their own if this block has none yet (the proposer would have put them into its batch).
    if (!pending_ring_reveals_.empty() && last_ring_batch_ < height_ + 1 && !ringsPausedAt(height_ + 1)) {
        RingBatchValue v;
        v.batch.network_id = policy_.network_id;
        v.batch.open_height = height_ + 1;
        v.batch.previous_main = tip_;
        const std::size_t limit = static_cast<std::size_t>(parameter(Param::RingBatchBytes));
        std::string ignored;
        for (const auto& r : pending_ring_reveals_) {
            RingBatch trial = v.batch;
            trial.reveals.push_back(r);
            if (matchReveal(r, height_ + 1, ignored) && encode(trial).size() <= limit) v.batch.reveals = std::move(trial.reveals);
        }
        if (!v.batch.reveals.empty() && !applyDecidedRingBatch(v, error)) return std::nullopt;
    }
    if (!prime.public_key.empty() && !registerKey(prime.provider, prime.public_key, error)) return std::nullopt;
    AcceptedPrime accepted;
    accepted.prime = prime.prime;
    accepted.witness = prime.witness;
    accepted.provider = prime.provider;
    accepted.submission_hash = submissionHash(prime);  // covers the signature bytes
    PrimeContents candidates;
    candidates.transfers = std::move(pending_transfers_);
    pending_transfers_.clear();
    candidates.governance = std::move(pending_governance_);
    pending_governance_.clear();
    candidates.ring_commits = std::move(pending_ring_commits_);
    pending_ring_commits_.clear();
    candidates.ring_payees = std::move(pending_ring_payees_);
    pending_ring_payees_.clear();
    auto block = closeAccepted(accepted, candidates, /*strict=*/false, error);
    std::erase_if(pending_ring_reveals_, [&](const RingReveal& v) {
        std::string ignored;
        return !matchReveal(v, height_ + 1, ignored);
    });
    return block;
}

std::optional<Block> Engine::closeAccepted(const AcceptedPrime& accepted, const PrimeContents& contents, bool strict,
                                           std::string& error) {
    if (accepted.version != kVersion1 || accepted.provider.empty()) { error = "invalid accepted prime"; return std::nullopt; }
    if (!checkPrimeArithmetic(accepted.prime, accepted.witness, error)) return std::nullopt;
    // 1. Transfers in order (D6, D16). Live: invalid ones are dropped. Replay: any invalid one fails.
    included_transfers_.clear();
    for (const auto& t : contents.transfers) {
        std::string reason;
        if (checkTransferForm(t, reason) && applyTransfer(t, reason)) {
            included_transfers_.push_back(t);
        } else if (strict) {
            error = "invalid transfer in block: " + reason;
            return std::nullopt;
        }
    }
    // 1b. Ring commits, then payee changes (B3.4). A commit's index is its position in the block.
    included_ring_commits_.clear();
    for (const auto& c : contents.ring_commits) {
        std::string reason;
        if (checkRingCommitForm(c, reason) && applyRingCommit(c, included_ring_commits_.size(), reason)) {
            included_ring_commits_.push_back(c);
        } else if (strict) {
            error = "invalid ring commit in block: " + reason;
            return std::nullopt;
        }
    }
    included_ring_payees_.clear();
    for (const auto& p : contents.ring_payees) {
        std::string reason;
        if (checkRingPayeeForm(p, reason) && applyRingPayee(p, reason)) {
            included_ring_payees_.push_back(p);
        } else if (strict) {
            error = "invalid ring payee change in block: " + reason;
            return std::nullopt;
        }
    }
    // 1c. Governance votes and ready messages (A3-2), after the transfers.
    included_governance_.clear();
    for (const auto& g : contents.governance) {
        std::string reason;
        if (checkGovernanceForm(g, reason) && applyGovernance(g, reason)) {
            included_governance_.push_back(g);
        } else if (strict) {
            error = "invalid governance transaction in block: " + reason;
            return std::nullopt;
        }
    }
    std::erase_if(tallies_, [&](const auto& entry) { return entry.second.proposal.expiry_height <= height_; });  // lapse
    // Ready messages of proposals that lapsed without passing are pruned with their tallies.
    std::erase_if(ready_, [&](const auto& e) { return readyDead(e.first, e.second.expiry, e.second.sequence, height_ + 1); });
    // 1d. The ring step (R5): commits past their window expire, then every ring whose earlier
    // commits are settled opens. The reveals of this block were applied with its ring batch (B3.6).
    ringStep(height_ + 1);
    expireCommits();
    openRings();
    // 2. D8 rewards: half to the prime finder, half per composite; remainders to the finder.
    const Integer& p = accepted.prime;
    const std::uint64_t total = static_cast<std::uint64_t>(parameter(Param::MainReward));
    const u128 composites_bp = 10000 - parameter(Param::FinderShareBp);
    const std::uint64_t composites_total = static_cast<std::uint64_t>(u128(total) * composites_bp / 10000);
    const std::uint64_t finder = total - composites_total;
    const std::uint64_t count = Integer(p - chain_frontier_ - 1).get_ui();
    const std::uint64_t share = composites_total / count;
    const AssetId asset = AssetId::main(p);
    for (const auto& range : open_ranges_) {
        const auto amount = Amount(share).mul(Integer(range.end - range.accepted_start + 1).get_ui());
        if (!amount) throw std::overflow_error("reward overflow");
        ledger_.credit(asset, range.provider, *amount, height_ + 1);
    }
    ledger_.credit(asset, accepted.provider, Amount(finder + composites_total % count), height_ + 1);
    if (!ledger_.addMainPrime(p, height_ + 1, Amount(total), error)) return std::nullopt;  // issued(p), for ring awards
    refreshAssetHash(p);

    chain_frontier_ = p;
    primes_.insert(p);
    pending_.clear();  // every queued batch was anchored to the previous tip
    pending_primes_.clear();
    // 3. D17: epoch distribution when the block being sealed ends an epoch.
    if ((height_ + 1) % parameter(Param::EpochLength) == 0) distributePools();
    return sealBlock(p, accepted);
}

Block Engine::sealBlock(const Integer& prime, const std::optional<AcceptedPrime>& accepted) {
    Block block;
    block.ranges = std::move(open_ranges_);
    open_ranges_.clear();
    block.new_keys = std::move(new_keys_);
    new_keys_.clear();
    block.transfers = std::move(included_transfers_);
    included_transfers_.clear();
    block.governance = std::move(included_governance_);
    included_governance_.clear();
    block.ring_commits = std::move(included_ring_commits_);
    included_ring_commits_.clear();
    block.ring_payees = std::move(included_ring_payees_);
    included_ring_payees_.clear();
    block.prime = accepted;
    ++height_;
    accepted_frontier_ = prime;
    BlockHeader& h = block.header;
    h.network_id = policy_.network_id;
    h.height = height_;
    h.previous_hash = tip_;
    h.prime = prime;
    h.policy_digest = params_.digestAt(height_);  // D20: the table in force for this block
    h.validator_epoch_digest = validatorEpochDigest();
    h.ranges_root = merkleRoot(rangesRootLeaves(block));
    std::vector<Bytes> tx_leaves;
    for (const auto& t : block.transfers) tx_leaves.push_back(encode(t));
    for (const auto& g : block.governance) tx_leaves.push_back(encode(g));
    for (const auto& c : block.ring_commits) tx_leaves.push_back(encode(c));
    for (const auto& p : block.ring_payees) tx_leaves.push_back(encode(p));
    h.transactions_root = merkleRoot(tx_leaves);
    h.state_root = stateRoot();
    // ring-records section 3 (proposed form): the ring records with their tips and pause state,
    // ordered by number, the open commits, then the ring batch decided for this block (its ring
    // blocks and reveals), if any (B3.6: the root covers every ring batch since the last prime).
    h.extension_root = merkleRoot(extensionLeaves(registry_, open_batch_hash_));
    open_batch_hash_ = {};
    tip_ = blockHash(h);
    blocks_.push_back(block);
    activateScheduled(height_ + 1);  // A3-2/A3-3: changes effective at the next height start now
    if (store_) {  // D19: fail-stop if the block cannot be made durable
        std::string error;
        if (!store_->append(encode(block), error)) throw std::runtime_error("persistence failure: " + error);
        if (height_ % policy_.snapshot_interval == 0 && !store_->writeSnapshot(height_, encodeState(), error))
            throw std::runtime_error("snapshot failure: " + error);
    }
    return block;
}

// ---------------------------------------------------------------- replay and persistence (D19)

bool Engine::applyBlock(const Block& b, std::string& error) {
    const std::string where = " at height " + std::to_string(b.header.height);
    if (b.header.height != height_ + 1 || b.header.previous_hash != tip_) {
        error = "broken linkage" + where;
        return false;
    }
    for (const auto& range : b.ranges) {
        if (range.version != kVersion1 || range.provider.empty() || range.accepted_start != accepted_frontier_ + 1 ||
            !checkDivisors(range.accepted_start, range.end, range.divisors, error)) {
            if (error.empty()) error = "invalid accepted range";
            error += where;
            return false;
        }
        applyAccepted(range);
    }
    for (const auto& key : b.new_keys) {
        if (keys_.count(key.address) || !registerKey(key.address, key.public_key, error)) {
            if (error.empty()) error = "key registered twice";
            error += where;
            return false;
        }
    }
    if (!b.prime) {
        error = "missing prime" + where;
        return false;
    }
    PrimeContents contents;
    contents.transfers = b.transfers;
    contents.governance = b.governance;
    contents.ring_commits = b.ring_commits;
    contents.ring_payees = b.ring_payees;
    const auto rebuilt = closeAccepted(*b.prime, contents, /*strict=*/true, error);
    if (!rebuilt) {
        error += where;
        return false;
    }
    if (encode(*rebuilt) != encode(b)) {
        error = "block does not match its replay" + where;
        return false;
    }
    return true;
}

std::optional<Engine> Engine::replay(const Policy& policy, const Genesis& genesis, const std::vector<Block>& blocks,
                                     std::string& error) {
    Engine e(policy, genesis);
    if (blocks.size() < 2 || encode(blocks[0]) != encode(e.blocks_[0]) || encode(blocks[1]) != encode(e.blocks_[1])) {
        error = "genesis mismatch";
        return std::nullopt;
    }
    for (std::size_t i = 2; i < blocks.size(); ++i)
        if (!e.applyBlock(blocks[i], error)) return std::nullopt;
    return e;
}

Bytes Engine::encodeState() const {
    Writer w;
    w.text("primechain-next/state-snapshot");
    w.varint(kVersion1);
    w.hash(policy_.digest());
    w.hash(genesis_digest_);
    w.varint(validators_.size());
    for (const auto& v : validators_) w.bytes(encode(v));
    w.varint(height_);
    w.integer(chain_frontier_);
    w.integer(accepted_frontier_);
    w.hash(tip_);
    w.varint(primes_.size());
    for (const auto& p : primes_) w.integer(p);
    w.varint(divisors_.size());
    for (const auto& [n, d] : divisors_) {
        w.integer(n);
        w.integer(d);
    }
    ledger_.encode(w);  // balances with their history, and the fee pool (B3.3)
    w.varint(keys_.size());
    for (const auto& [address, key] : keys_) {
        w.text(address);
        w.bytes(key);
    }
    w.varint(nonces_.size());
    for (const auto& [address, n] : nonces_) {
        w.text(address);
        w.varint(n);
    }
    // Governance state (A3-2) and the parameter table with its history (D20).
    params_.encode(w);
    w.varint(last_sequence_);
    w.varint(tallies_.size());
    for (const auto& [id, tally] : tallies_) {
        w.bytes(encode(tally.proposal));
        w.varint(tally.voters.size());
        for (const auto& v : tally.voters) w.hash(v);
    }
    w.varint(ready_.size());
    for (const auto& [id, ready] : ready_) {
        w.hash(id);
        w.varint(ready.height);
        w.varint(ready.expiry);
        w.varint(ready.sequence);
    }
    w.varint(scheduled_.size());
    for (const auto& [height, proposal] : scheduled_) {
        w.varint(height);
        w.bytes(encode(proposal));
    }
    registry_.encode(w);  // ring records and open commits (B3.4)
    w.varint(last_ring_batch_);  // B3.5: one ring batch per main block
    return w.data();
}

bool Engine::decodeState(const Bytes& data, std::string& error) {
    Reader r(data, policy_.limits.max_integer_bytes);
    std::string tag;
    std::uint64_t version = 0, count = 0;
    Hash256 policy_digest{}, validator_digest{};
    const auto& l = policy_.limits;
    auto fail = [&](const std::string& message) {
        error = "snapshot: " + (r.error().empty() ? message : r.error());
        return false;
    };
    if (!r.text(tag, 64) || tag != "primechain-next/state-snapshot" || !r.varint(version) || version != kVersion1)
        return fail("bad header");
    if (!r.hash(policy_digest) || policy_digest != policy_.digest()) return fail("policy mismatch");
    if (!r.hash(validator_digest) || validator_digest != genesis_digest_) return fail("genesis validator mismatch");
    if (!r.varint(count) || count > l.max_list) return fail("bad validator set");
    std::vector<ValidatorRecord> validators(count);
    for (auto& v : validators) {
        Bytes raw;
        std::string inner;
        if (!r.bytes(raw, 2 * l.max_key_bytes + 2 * l.max_signature_bytes + l.max_text + 64) ||
            !decode(raw, v, l, inner))
            return fail("bad validator");
    }
    validators_ = std::move(validators);
    blocks_.clear();
    primes_.clear(); divisors_.clear(); ledger_ = AssetLedger{};
    keys_.clear(); nonces_.clear(); asset_hashes_.clear(); ring_asset_hashes_.clear();
    if (!r.varint(height_) || !r.integer(chain_frontier_) || !r.integer(accepted_frontier_) || !r.hash(tip_) ||
        !r.varint(count))
        return fail("bad frontier");
    for (std::uint64_t i = 0; i < count; ++i) {
        Integer p;
        if (!r.integer(p)) return fail("bad prime");
        primes_.insert(p);
    }
    if (!r.varint(count)) return fail("bad divisor count");
    for (std::uint64_t i = 0; i < count; ++i) {
        Integer n, d;
        if (!r.integer(n) || !r.integer(d)) return fail("bad divisor");
        divisors_.emplace(n, d);
    }
    std::string ledger_error;
    if (!ledger_.decode(r, l.max_text, ledger_error)) return fail(ledger_error);
    std::set<Integer> assets;
    std::vector<AssetId> ring_assets;
    for (const auto& x : ledger_.movedAssets()) {
        if (x.isMain()) assets.insert(x.q);
        else ring_assets.push_back(x);
    }
    for (const auto& x : ledger_.assets())
        if (x.isMain()) assets.insert(x.q);
    if (!r.varint(count)) return fail("bad key registry");
    for (std::uint64_t i = 0; i < count; ++i) {
        Address address;
        Bytes key;
        if (!r.text(address, l.max_text) || !r.bytes(key, l.max_key_bytes)) return fail("bad key");
        keys_.emplace(address, key);
    }
    if (!r.varint(count)) return fail("bad nonces");
    for (std::uint64_t i = 0; i < count; ++i) {
        Address address;
        std::uint64_t n = 0;
        if (!r.text(address, l.max_text) || !r.varint(n)) return fail("bad nonce");
        nonces_.emplace(address, n);
    }
    tallies_.clear(); ready_.clear(); scheduled_.clear();
    auto readProposalBytes = [&](GovernanceProposal& out) {
        Bytes raw;
        std::string inner;
        return r.bytes(raw, 2 * l.max_key_bytes + 2 * l.max_signature_bytes + 1024) && decode(raw, out, l, inner);
    };
    std::string table_error;
    if (!params_.decode(r, table_error)) return fail(table_error);
    if (!r.varint(last_sequence_) || !r.varint(count)) return fail("bad governance state");
    for (std::uint64_t i = 0; i < count; ++i) {
        Tally tally;
        std::uint64_t voters = 0;
        if (!readProposalBytes(tally.proposal) || !r.varint(voters)) return fail("bad tally");
        for (std::uint64_t j = 0; j < voters; ++j) {
            Hash256 v{};
            if (!r.hash(v)) return fail("bad voter");
            tally.voters.insert(v);
        }
        tallies_.emplace(proposalId(tally.proposal), std::move(tally));
    }
    if (!r.varint(count)) return fail("bad ready set");
    for (std::uint64_t i = 0; i < count; ++i) {
        Hash256 id{};
        Ready ready;
        if (!r.hash(id) || !r.varint(ready.height) || !r.varint(ready.expiry) || !r.varint(ready.sequence)) return fail("bad ready entry");
        ready_.emplace(id, ready);
    }
    if (!r.varint(count)) return fail("bad schedule");
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t height = 0;
        GovernanceProposal p;
        if (!r.varint(height) || !readProposalBytes(p)) return fail("bad scheduled change");
        scheduled_.emplace_back(height, std::move(p));
    }
    std::string registry_error;
    if (!registry_.decode(r, l.max_text, registry_error)) return fail(registry_error);
    if (!r.varint(last_ring_batch_) || last_ring_batch_ > height_ + 1) return fail("bad ring batch height");
    ring_batches_.clear();
    pending_works_.clear();
    if (!r.done()) return fail("trailing bytes");
    for (const auto& asset : assets) refreshAssetHash(asset);
    for (const auto& x : ring_assets) refreshAsset(x);
    return true;
}

std::optional<Engine> Engine::open(const std::string& directory, const Policy& policy, const Genesis& genesis,
                                   std::string& error) {
    auto store = std::make_shared<Store>();
    if (!store->open(directory, error)) return std::nullopt;
    Engine e(policy, genesis);
    // B3.5: rows live in the store's row index; the rings file holds the ring batches.
    e.ledger_.attachRowIndex(store->rowIndex());
    e.persist_rows_ = true;
    const RingFile& rings = store->rings();
    if (store->blockCount() == 0) {  // new chain: persist the genesis seed blocks
        if (rings.count() != 0) {
            error = "rings file without a block file";
            return std::nullopt;
        }
        for (const auto& b : e.blocks_)
            if (!store->append(encode(b), error)) return std::nullopt;
        e.store_ = store;
        return e;
    }
    auto readBlock = [&](std::uint64_t index, Block& out) {
        Bytes raw;
        std::string inner;
        if (!store->read(index, raw, error)) return false;
        if (!decode(raw, out, policy.limits, inner)) {
            error = "block file corrupt at frame " + std::to_string(index) + ": " + inner;
            return false;
        }
        return true;
    };
    std::uint64_t snapshot_height = 0;
    Bytes state;
    bool found = false;
    if (!store->loadSnapshot(snapshot_height, state, found, error)) return std::nullopt;
    std::uint64_t next = 2;
    if (found) {
        if (snapshot_height < 2 || snapshot_height > store->blockCount()) {
            error = "snapshot is ahead of the block file";
            return std::nullopt;
        }
        if (!e.decodeState(state, error)) return std::nullopt;
        e.ledger_.attachRowIndex(store->rowIndex());  // the snapshot names the rows by count and digest
        if (!e.ledger_.verifyRowIndex(error)) return std::nullopt;
        Block anchor;
        if (!readBlock(snapshot_height - 1, anchor)) return std::nullopt;
        if (blockHash(anchor.header) != e.tip_ || anchor.header.height != snapshot_height) {
            error = "snapshot does not match the block file";
            return std::nullopt;
        }
        next = snapshot_height;
    } else {
        for (std::uint64_t i = 0; i < 2; ++i) {
            Block b;
            if (!readBlock(i, b)) return std::nullopt;
            if (encode(b) != encode(e.blocks_[i])) {
                error = "genesis mismatch in block file";
                return std::nullopt;
            }
        }
    }
    // Ring batches: before main block h, the batch with open height h (batches up to the snapshot are
    // in its state; one decided after the last block is applied at the end).
    std::uint64_t ring_next = 0;
    while (ring_next < rings.count() && rings.height(ring_next) <= e.height_) ++ring_next;
    if (e.last_ring_batch_ != 0 && (ring_next == 0 || rings.height(ring_next - 1) != e.last_ring_batch_)) {
        error = "rings file does not reach the snapshot";  // the snapshot's last ring batch must be in the file
        return std::nullopt;
    }
    auto ringBatchFor = [&](std::uint64_t h) {
        if (ring_next >= rings.count() || rings.height(ring_next) != h) return true;
        Bytes raw;
        RingBatch batch;
        std::string inner;
        if (!rings.read(ring_next, raw, error)) return false;
        if (!decode(raw, batch, policy.limits.max_text, inner) || batch.open_height != h) {
            error = "rings file corrupt at frame " + std::to_string(ring_next) + ": " + inner;
            return false;
        }
        if (!e.applyRingBatch(batch, /*append=*/false, error)) {
            error = "ring batch at open height " + std::to_string(h) + ": " + error;
            return false;
        }
        ++ring_next;
        return true;
    };
    for (std::uint64_t i = next; i < store->blockCount(); ++i) {
        Block b;
        if (!ringBatchFor(e.height_ + 1) || !readBlock(i, b) || !e.applyBlock(b, error)) return std::nullopt;
    }
    if (!ringBatchFor(e.height_ + 1)) return std::nullopt;
    if (ring_next != rings.count()) {
        error = "rings file is ahead of the block file";
        return std::nullopt;
    }
    e.store_ = store;
    return e;
}

// ---------------------------------------------------------------- hooks for consensus (A3-1)

Engine Engine::scratch() const {
    // A dry run needs the state but not the block history, which would make every check O(height).
    // The history is moved aside for the copy and restored; Engine is single-threaded.
    auto& self = const_cast<Engine&>(*this);
    std::vector<Block> history;
    std::vector<RingBatch> ring_history;
    history.swap(self.blocks_);
    ring_history.swap(self.ring_batches_);
    Engine copy = *this;
    self.blocks_.swap(history);
    self.ring_batches_.swap(ring_history);
    copy.store_.reset();  // dry runs must never touch the store
    copy.persist_rows_ = false;  // nor the row index (rows stay in the copy's memory)
    return copy;
}

AcceptedPrime Engine::acceptedFrom(const PrimeSubmission& prime) {
    AcceptedPrime accepted;
    accepted.prime = prime.prime;
    accepted.witness = prime.witness;
    accepted.provider = prime.provider;
    accepted.submission_hash = submissionHash(prime);  // covers the signature bytes
    return accepted;
}

std::optional<RangeSubmission> Engine::bestPendingRange() const {
    const Pending* best = nullptr;
    std::string ignored;
    for (const auto& p : pending_) {
        if (p.range.end <= accepted_frontier_ || !validateRange(p.range, ignored)) continue;
        if (!best || p.range.end > best->range.end || (p.range.end == best->range.end && p.arrival < best->arrival))
            best = &p;
    }
    if (!best) return std::nullopt;
    return best->range;
}

bool Engine::applyDecidedRange(const RangeSubmission& range, std::string& error) {
    if (!validateRange(range, error)) return false;
    if (!range.public_key.empty() && !registerKey(range.provider, range.public_key, error)) return false;
    AcceptedRange accepted;
    accepted.provider = range.provider;
    accepted.accepted_start = accepted_frontier_ + 1;
    accepted.end = range.end;
    accepted.submission_hash = submissionHash(range);
    const Integer offset = accepted.accepted_start - range.start;
    accepted.divisors.assign(range.divisors.begin() + static_cast<std::ptrdiff_t>(offset.get_ui()), range.divisors.end());
    applyAccepted(accepted);
    std::erase_if(pending_, [&](const Pending& p) { return p.hash == accepted.submission_hash || p.range.end <= accepted_frontier_; });
    return true;
}

bool Engine::submitPrime(const PrimeSubmission& s, std::string& error) {
    if (s.version != kVersion1) { error = "unsupported prime version"; return false; }
    if (s.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (s.anchor != tip_) { error = "prime is not anchored to the current tip"; return false; }
    if (!checkSigned(primeSigningPayload(s), s.provider, s.public_key, s.signature, error)) return false;
    const Hash256 h = submissionHash(s);
    for (const auto& p : pending_primes_)
        if (submissionHash(p) == h) return true;
    pending_primes_.push_back(s);
    return true;
}

std::optional<PrimeSubmission> Engine::bestPendingPrime() const {
    std::string ignored;
    for (const auto& p : pending_primes_)
        if (validatePrime(p, ignored)) return p;
    return std::nullopt;
}

std::optional<Engine::PrimeContents> Engine::buildPrimeContents(const PrimeSubmission& prime, std::string& error) const {
    Engine sim = scratch();
    if (!sim.validatePrime(prime, error)) return std::nullopt;
    if (!prime.public_key.empty() && !sim.registerKey(prime.provider, prime.public_key, error)) return std::nullopt;
    PrimeContents candidates;
    candidates.transfers = pending_transfers_;
    candidates.governance = pending_governance_;
    candidates.ring_commits = pending_ring_commits_;
    candidates.ring_payees = pending_ring_payees_;
    const auto block = sim.closeAccepted(acceptedFrom(prime), candidates, /*strict=*/false, error);
    if (!block) return std::nullopt;
    PrimeContents out;
    out.transfers = block->transfers;
    out.governance = block->governance;
    out.ring_commits = block->ring_commits;
    out.ring_payees = block->ring_payees;
    out.header_hash = blockHash(block->header);
    return out;
}

bool Engine::checkPrimeValue(const PrimeSubmission& prime, const PrimeContents& contents, std::string& error) const {
    Engine sim = scratch();
    if (!sim.validatePrime(prime, error)) return false;
    if (!prime.public_key.empty() && !sim.registerKey(prime.provider, prime.public_key, error)) return false;
    const auto block = sim.closeAccepted(acceptedFrom(prime), contents, /*strict=*/true, error);
    if (!block) return false;
    if (blockHash(block->header) != contents.header_hash) { error = "prime value: header hash does not match"; return false; }
    return true;
}

std::optional<Block> Engine::applyDecidedPrime(const PrimeSubmission& prime, const PrimeContents& contents,
                                               std::string& error) {
    if (!validatePrime(prime, error)) return std::nullopt;
    if (!prime.public_key.empty() && !registerKey(prime.provider, prime.public_key, error)) return std::nullopt;
    auto block = closeAccepted(acceptedFrom(prime), contents, /*strict=*/true, error);
    if (!block) return std::nullopt;
    if (blockHash(block->header) != contents.header_hash) {  // checked before deciding; a mismatch here is a bug
        error = "decided prime value: header hash does not match the sealed block";
        return std::nullopt;
    }
    // Mempool revalidation against the new finalized state (F9): drop what is now included or stale.
    std::set<Hash256> included;
    for (const auto& t : block->transfers) included.insert(sha3(encode(t)));
    std::erase_if(pending_transfers_, [&](const Transfer& t) {
        return included.count(sha3(encode(t))) || t.nonce <= nonce(t.sender);
    });
    std::set<Hash256> included_gov;
    for (const auto& g : block->governance) included_gov.insert(sha3(encode(g)));
    std::erase_if(pending_governance_, [&](const GovernanceTx& g) {
        return included_gov.count(sha3(encode(g))) ||
               (g.kind == GovernanceTxKind::Vote && (g.proposal.expiry_height <= height_ || g.proposal.sequence <= last_sequence_)) ||
               (g.kind == GovernanceTxKind::Ready &&
                (ready_.count(proposalId(g.proposal)) ||
                 readyDead(proposalId(g.proposal), g.proposal.expiry_height, g.proposal.sequence, height_ + 1)));
    });
    // B3.4: drop included ring records, stale nonces, and reveals that no longer match.
    std::set<Hash256> included_ring;
    for (const auto& c : block->ring_commits) included_ring.insert(sha3(encode(c)));
    for (const auto& p : block->ring_payees) included_ring.insert(sha3(encode(p)));
    std::erase_if(pending_ring_commits_, [&](const RingCommit& c) {
        return included_ring.count(sha3(encode(c))) || c.nonce <= nonce(c.discoverer);
    });
    std::erase_if(pending_ring_payees_, [&](const RingPayee& p) {
        const RingEntry* r = registry_.ring(p.ring);
        return included_ring.count(sha3(encode(p))) || !r || p.nonce <= nonce(r->payees.back().second);
    });
    std::erase_if(pending_ring_reveals_, [&](const RingReveal& v) {  // revealed in a batch, or expired
        std::string ignored;
        return !matchReveal(v, height_ + 1, ignored);
    });
    pending_primes_.clear();
    return block;
}

}  // namespace primechain::next
