#include "primechain_next/consensus.hpp"

#include "primechain/crypto/signature.hpp"
#include "primechain_next/codec.hpp"
#include "primechain_next/merkle.hpp"

#include <algorithm>
#include <stdexcept>

namespace primechain::next::consensus {

namespace {

constexpr std::size_t kServeBatch = 32;       // decisions per catch-up reply
constexpr std::size_t kMaxBuffered = 20000;   // messages held for future instances

std::string describe(const InstanceId& id, std::uint64_t round) {
    return "(h=" + std::to_string(id.h) + ", k=" + std::to_string(id.k) + ", r=" + std::to_string(round) + ")";
}

void writeInstance(Writer& w, const InstanceId& id, std::uint64_t round) {
    w.varint(id.h);
    w.varint(id.k);
    w.varint(round);
}

}  // namespace

// ---------------------------------------------------------------- crypto, timing, encodings

Crypto mldsa() {
    return {[](const Bytes& secret, const Bytes& payload) {
                std::string error;
                auto signature = primechain::crypto::signProtocolMessage(secret, payload, error);
                if (!signature) throw std::runtime_error("consensus signing failed: " + error);
                return *signature;
            },
            [](const Bytes& key, const Bytes& payload, const Bytes& signature) {
                std::string error;
                return primechain::crypto::verifyProtocolMessageSignature(key, payload, signature, error);
            }};
}

Crypto fastMock() {
    auto tag = [](const Bytes& key, const Bytes& payload) {
        Writer w;
        w.text("mock-signature");
        w.bytes(key);
        w.bytes(payload);
        const Hash256 h = sha3(w.data());
        return Bytes(h.begin(), h.end());
    };
    return {[tag](const Bytes& secret, const Bytes& payload) { return tag(secret, payload); },  // secret == public
            [tag](const Bytes& key, const Bytes& payload, const Bytes& signature) { return tag(key, payload) == signature; }};
}

std::uint64_t Timing::timeout(std::uint64_t round) const {
    std::uint64_t t = base_ms;
    for (std::uint64_t i = 0; i < round && t < cap_ms; ++i) t *= 2;
    return std::min(t, cap_ms);
}

Bytes encode(const Certificate& c) {
    Writer w;
    w.text("primechain-next/consensus-certificate");
    writeInstance(w, c.id, c.round);
    w.hash(c.value);
    w.bytes(Bytes(c.signers.begin(), c.signers.end()));
    w.varint(c.signatures.size());
    for (const auto& s : c.signatures) w.bytes(s);
    return w.data();
}

Bytes encode(const Value& v) {
    Writer w;
    w.text("primechain-next/consensus-value");
    w.u8(static_cast<std::uint8_t>(v.kind));
    if (v.kind == Value::Kind::Range) {
        w.bytes(primechain::next::encode(v.range));
    } else {
        w.bytes(primechain::next::encode(v.prime));
        w.varint(v.contents.transfers.size());
        for (const auto& t : v.contents.transfers) w.bytes(primechain::next::encode(t));
        w.varint(v.contents.governance.size());
        for (const auto& g : v.contents.governance) w.bytes(primechain::next::encode(g));
    }
    w.u8(v.previous ? 1 : 0);
    if (v.previous) w.bytes(encode(*v.previous));
    return w.data();
}

Hash256 valueHash(const Value& value) { return sha3(encode(value)); }

// D10 domain: tag, network, validator-set digest, (h, k, r), phase, value hash or nil.
Bytes voteSigningPayload(const std::string& network_id, const Hash256& set_digest, const Vote& vote) {
    Writer w;
    w.text("primechain-next/consensus-vote");
    w.text(network_id);
    w.hash(set_digest);
    writeInstance(w, vote.id, vote.round);
    w.u8(static_cast<std::uint8_t>(vote.phase));
    w.u8(vote.value ? 1 : 0);
    if (vote.value) w.hash(*vote.value);
    return w.data();
}

Bytes proposalSigningPayload(const std::string& network_id, const Hash256& set_digest, const Proposal& p) {
    Writer w;
    w.text("primechain-next/consensus-proposal");
    w.text(network_id);
    w.hash(set_digest);
    writeInstance(w, p.id, p.round);
    w.varint(static_cast<std::uint64_t>(p.valid_round + 1));
    w.hash(valueHash(p.value));
    return w.data();
}

// ---------------------------------------------------------------- construction and durable state

Node::Node(Bytes consensus_public, Bytes consensus_secret, Engine engine, Crypto crypto, Timing timing,
           Retention retention)
    : public_(std::move(consensus_public)), secret_(std::move(consensus_secret)), engine_(std::move(engine)),
      crypto_(std::move(crypto)), timing_(timing), retention_(retention) {
    if (retention_.window == 0 || retention_.checkpoint_interval == 0)
        throw std::invalid_argument("retention window and interval must be positive");
    id_ = {engine_.height() + 1, 0};
    loadInstanceContext();
}

Durable Node::durable() const {
    Durable d;
    for (const auto& [id, decision] : decisions_)
        if (id.h >= engine_.height()) d.decisions.emplace(id, decision);  // the open block must be replayable
    d.certificates = certificates_;
    d.missed = missed_;
    d.previous_id = previous_id_;
    d.previous_value = previous_value_;
    d.previous_keys = previous_keys_;
    d.previous_digest = previous_digest_;
    d.previous_candidates = previous_candidates_;
    d.set_change_heights = set_change_heights_;
    d.id = id_;
    d.round = round_;
    d.locked = locked_;
    d.locked_round = locked_round_;
    d.valid = valid_;
    d.valid_round = valid_round_;
    d.sent = sent_;
    d.proposed = proposed_;
    return d;
}

Node Node::restore(Bytes consensus_public, Bytes consensus_secret, Engine engine, const Durable& d, Crypto crypto,
                   Timing timing, Retention retention) {
    Node n(std::move(consensus_public), std::move(consensus_secret), std::move(engine), std::move(crypto), timing,
           retention);
    // The store holds every sealed block; decisions inside the open block are replayed from the log.
    for (const auto& [id, decision] : d.decisions) {
        if (id.h <= n.engine_.height()) continue;
        std::string error;
        if (id.h != n.engine_.height() + 1 || !n.applyValue(decision.value, error))
            throw std::runtime_error("restore: cannot replay decision " + describe(id, decision.certificate.round) +
                                     ": " + error);
    }
    n.decisions_ = d.decisions;
    n.certificates_ = d.certificates;
    n.missed_ = d.missed;
    n.previous_id_ = d.previous_id;
    n.previous_value_ = d.previous_value;
    n.previous_keys_ = d.previous_keys;
    n.previous_digest_ = d.previous_digest;
    n.previous_candidates_ = d.previous_candidates;
    n.set_change_heights_ = d.set_change_heights;
    n.id_ = d.id;
    if (n.id_.h != n.engine_.height() + 1) throw std::runtime_error("restore: instance does not match the store");
    n.loadInstanceContext();
    n.round_ = d.round;
    n.locked_ = d.locked;
    n.locked_round_ = d.locked_round;
    n.valid_ = d.valid;
    n.valid_round_ = d.valid_round;
    n.sent_ = d.sent;
    n.proposed_ = d.proposed;
    n.resume_ = true;
    return n;
}

// ---------------------------------------------------------------- instance context

void Node::loadInstanceContext() {
    keys_.clear();
    for (const auto& v : engine_.validators()) keys_.push_back(v.consensus_key);
    set_digest_ = engine_.validatorSetDigest();
    // A3-3: skip validators with >= M consecutive missed proposer turns; never skip everyone.
    candidates_.clear();
    const std::uint64_t m = engine_.policy().proposer_skip_after;
    for (std::uint32_t i = 0; i < keys_.size(); ++i) {
        const auto it = missed_.find(sha3(keys_[i]));
        if (it == missed_.end() || it->second < m) candidates_.push_back(i);
    }
    if (candidates_.empty())
        for (std::uint32_t i = 0; i < keys_.size(); ++i) candidates_.push_back(i);
}

void Node::resetInstanceState() {
    round_ = 0;
    step_ = Step::Propose;
    locked_.reset();
    locked_round_ = -1;
    valid_.reset();
    valid_round_ = -1;
    rounds_.clear();
    prevote_timer_started_.clear();
    precommit_timer_started_.clear();
    polka_done_.clear();
    timers_.clear();
    sent_.clear();
    proposed_.clear();
    valid_cache_.clear();
    values_.clear();
    seen_.clear();
}

std::optional<std::uint32_t> Node::selfIndex() const {
    for (std::uint32_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == public_) return i;
    return std::nullopt;
}

std::uint32_t Node::proposer(std::uint64_t round) const {
    return candidates_[(id_.h + id_.k + round) % candidates_.size()];
}

Vote Node::signVote(const InstanceId& id, std::uint64_t round, Phase phase, const std::optional<Hash256>& value) const {
    Vote v{id, round, phase, value, selfIndex().value_or(0), {}};
    v.signature = crypto_.sign(secret_, voteSigningPayload(engine_.policy().network_id, set_digest_, v));
    return v;
}

void Node::signProposal(Proposal& p) const {
    p.signature = crypto_.sign(secret_, proposalSigningPayload(engine_.policy().network_id, set_digest_, p));
}

// ---------------------------------------------------------------- rounds

void Node::start(std::uint64_t now, std::vector<Outgoing>& out) {
    if (resume_) {
        resume_ = false;
        for (const auto& v : sent_) {  // our own votes count again, and are re-sent, never re-signed
            seen_[{v.id.h, v.id.k, v.round, static_cast<std::uint8_t>(v.phase), v.signer}] = v;
            (v.phase == Phase::Prevote ? rounds_[v.round].prevotes : rounds_[v.round].precommits)[v.signer] = v;
        }
        for (const auto& p : proposed_) {
            rounds_[p.round].proposal = p;
            values_[valueHash(p.value)] = p.value;
        }
        for (const auto* v : {&locked_, &valid_})
            if (*v) values_[valueHash(**v)] = **v;
        for (const auto& v : sent_) broadcast(v, out);
        startRound(round_, now, out);
        return;
    }
    beginInstance(now, out);
}

void Node::beginInstance(std::uint64_t now, std::vector<Outgoing>& out) {
    resetInstanceState();
    loadInstanceContext();
    startRound(0, now, out);
    // Replay messages that arrived early for this instance; drop those for instances now past.
    auto pending = future_.find(id_);
    std::vector<std::pair<Message, std::uint32_t>> replay;
    if (pending != future_.end()) replay = std::move(pending->second);
    for (auto it = future_.begin(); it != future_.end() && it->first <= id_;) {
        buffered_ -= it->second.size();
        it = future_.erase(it);
    }
    const InstanceId here = id_;
    for (const auto& [message, from] : replay) {
        if (id_ != here) {  // an earlier replayed message decided this instance; keep the rest for later
            buffer(message, here, from);
            continue;
        }
        receive(message, from, now, out);
    }
}

void Node::schedule(std::uint8_t kind, std::uint64_t round, std::uint64_t now) {
    const std::uint64_t delay = kind == 3 ? 2 * timing_.base_ms : timing_.timeout(round);
    timers_.push_back({now + delay, kind, round});
}

std::optional<std::uint64_t> Node::nextDeadline() const {
    std::optional<std::uint64_t> due;
    for (const auto& t : timers_)
        if (!due || t.due < *due) due = t.due;
    return due;
}

std::optional<Value> Node::getValue() {
    Value v;
    if (previous_id_) {
        const auto it = certificates_.find(*previous_id_);
        if (it == certificates_.end()) return std::nullopt;
        v.previous = it->second;
    }
    std::string error;
    if (auto prime = engine_.bestPendingPrime()) {  // a valid prime means the gap is covered: close the block
        auto contents = engine_.buildPrimeContents(*prime, error);
        if (contents) {
            v.kind = Value::Kind::Prime;
            v.prime = *prime;
            v.contents = std::move(*contents);
            return v;
        }
    }
    if (auto range = engine_.bestPendingRange()) {
        v.kind = Value::Kind::Range;
        v.range = *range;
        return v;
    }
    return std::nullopt;
}

void Node::propose(std::uint64_t now, std::vector<Outgoing>& out) {
    const auto self = selfIndex();
    if (!self || *self != proposer(round_) || step_ != Step::Propose) return;
    for (const auto& p : proposed_)
        if (p.round == round_) return;
    std::optional<Value> value = valid_;
    if (!value) value = getValue();
    if (!value) return;  // nothing to propose yet; a submission arriving later retries
    Proposal p{id_, round_, valid_ ? valid_round_ : -1, *value, *self, {}};
    signProposal(p);
    proposed_.push_back(p);
    const Hash256 h = valueHash(p.value);
    values_[h] = p.value;
    rounds_[round_].proposal = p;
    broadcast(p, out);
    evaluate(now, out);
}

void Node::startRound(std::uint64_t round, std::uint64_t now, std::vector<Outgoing>& out) {
    round_ = round;
    step_ = Step::Propose;
    timers_.clear();
    schedule(0, round, now);  // the proposer too: if it has nothing to propose, the round must still end
    schedule(3, round, now);  // retransmission
    for (const auto& v : sent_)
        if (v.round == round_) step_ = std::max(step_, v.phase == Phase::Precommit ? Step::Precommit : Step::Prevote);
    bool reproposed = false;
    for (const auto& p : proposed_)
        if (p.round == round_) {
            broadcast(p, out);
            reproposed = true;
        }
    if (!reproposed) propose(now, out);
    evaluate(now, out);
}

std::size_t Node::count(const std::map<std::uint32_t, Vote>& votes, const std::optional<Hash256>& value, bool any) const {
    if (any) return votes.size();
    return static_cast<std::size_t>(
        std::count_if(votes.begin(), votes.end(), [&](const auto& e) { return e.second.value == value; }));
}

void Node::castVote(Phase phase, const std::optional<Hash256>& value, std::vector<Outgoing>& out) {
    for (const auto& v : sent_)
        if (v.round == round_ && v.phase == phase) {  // never sign twice: re-send what was signed
            broadcast(v, out);
            return;
        }
    if (!selfIndex()) return;  // observers follow the chain without voting
    const Vote v = signVote(id_, round_, phase, value);
    sent_.push_back(v);
    seen_[{v.id.h, v.id.k, v.round, static_cast<std::uint8_t>(phase), v.signer}] = v;
    (phase == Phase::Prevote ? rounds_[round_].prevotes : rounds_[round_].precommits)[v.signer] = v;
    broadcast(v, out);
}

bool Node::valid(const Value& value) {
    const Hash256 h = valueHash(value);
    if (const auto it = valid_cache_.find(h); it != valid_cache_.end()) return it->second;
    bool ok = false;
    std::string error;
    if (previous_id_) {
        ok = value.previous && value.previous->id == *previous_id_ && value.previous->value == previous_value_ &&
             verifyCertificate(*value.previous, previous_keys_, previous_digest_);
    } else {
        ok = !value.previous;
    }
    if (ok)
        ok = value.kind == Value::Kind::Range ? engine_.validateRange(value.range, error)
                                              : engine_.checkPrimeValue(value.prime, value.contents, error);
    valid_cache_[h] = ok;
    return ok;
}

void Node::evaluate(std::uint64_t now, std::vector<Outgoing>& out) {
    const std::size_t n = keys_.size(), q = Engine::quorum(n);
    const InstanceId here = id_;
    for (bool changed = true; changed && id_ == here;) {
        changed = false;
        auto& rs = rounds_[round_];
        const std::uint64_t r = round_;
        // Rules 1-2: prevote on the round's proposal.
        if (step_ == Step::Propose && rs.proposal) {
            const auto& p = *rs.proposal;
            const Hash256 h = valueHash(p.value);
            const bool unlocked = locked_round_ == -1 || valueHash(*locked_) == h;
            if (p.valid_round == -1) {
                castVote(Phase::Prevote, valid(p.value) && unlocked ? std::optional(h) : std::nullopt, out);
                step_ = Step::Prevote;
                changed = true;
            } else if (p.valid_round < static_cast<std::int64_t>(r) &&
                       count(rounds_[static_cast<std::uint64_t>(p.valid_round)].prevotes, h, false) >= q) {
                const bool justified = locked_round_ <= p.valid_round || unlocked;  // justified unlock
                castVote(Phase::Prevote, valid(p.value) && justified ? std::optional(h) : std::nullopt, out);
                step_ = Step::Prevote;
                changed = true;
            }
        }
        // Rule 3: any prevote quorum starts the prevote timer.
        if (step_ == Step::Prevote && count(rs.prevotes, {}, true) >= q && prevote_timer_started_.insert(r).second)
            schedule(1, r, now);
        // Rule 4: a polka for the proposal locks (if still prevoting) and records the valid value.
        if (rs.proposal && step_ != Step::Propose && !polka_done_.count(r)) {
            const Hash256 h = valueHash(rs.proposal->value);
            if (count(rs.prevotes, h, false) >= q && valid(rs.proposal->value)) {
                polka_done_.insert(r);
                if (step_ == Step::Prevote) {
                    locked_ = rs.proposal->value;
                    locked_round_ = static_cast<std::int64_t>(r);
                    castVote(Phase::Precommit, h, out);
                    step_ = Step::Precommit;
                }
                valid_ = rs.proposal->value;
                valid_round_ = static_cast<std::int64_t>(r);
                changed = true;
            }
        }
        // Rule 5: a nil polka.
        if (step_ == Step::Prevote && count(rs.prevotes, std::nullopt, false) >= q) {
            castVote(Phase::Precommit, std::nullopt, out);
            step_ = Step::Precommit;
            changed = true;
        }
        // Rule 6: any precommit quorum starts the precommit timer.
        if (count(rs.precommits, {}, true) >= q && precommit_timer_started_.insert(r).second) schedule(2, r, now);
        // Rule 7: a precommit quorum for a known valid value decides, in any round.
        for (const auto& [round, state] : rounds_) {
            std::map<Hash256, std::size_t> tally;
            for (const auto& [signer, v] : state.precommits)
                if (v.value) ++tally[*v.value];
            for (const auto& [h, c] : tally) {
                if (c < q) continue;
                const auto value = values_.find(h);
                if (value == values_.end()) {  // decided elsewhere, value not seen here: ask for the decision
                    requestSync(std::nullopt, now, out);
                    continue;
                }
                if (!valid(value->second)) continue;
                const auto certificate = buildCertificate(round, h);
                if (certificate) {
                    const Value decided = value->second;
                    decide(decided, *certificate, now, out);
                    return;
                }
            }
        }
        // Rule 8: n - q + 1 distinct senders in a higher round skip ahead to it.
        for (auto it = rounds_.upper_bound(round_); it != rounds_.end(); ++it) {
            std::set<std::uint32_t> senders;
            for (const auto& [signer, v] : it->second.prevotes) senders.insert(signer);
            for (const auto& [signer, v] : it->second.precommits) senders.insert(signer);
            if (it->second.proposal) senders.insert(it->second.proposal->proposer);
            if (senders.size() >= n - q + 1) {
                startRound(it->first, now, out);
                return;
            }
        }
    }
}

void Node::tick(std::uint64_t now, std::vector<Outgoing>& out) {
    while (true) {
        auto it = std::min_element(timers_.begin(), timers_.end(), [](const Timer& a, const Timer& b) { return a.due < b.due; });
        if (it == timers_.end() || it->due > now) return;
        const Timer t = *it;
        timers_.erase(it);
        const InstanceId here = id_;
        if (t.kind == 3) {  // retransmit what this node signed in this instance
            for (const auto& v : sent_) broadcast(v, out);
            for (const auto& p : proposed_)
                if (p.round == round_) broadcast(p, out);
            if (!proposed_.empty() || selfIndex()) propose(now, out);
            schedule(3, round_, now);
            continue;
        }
        if (t.round != round_) continue;
        if (t.kind == 0 && step_ == Step::Propose) {
            ++timeouts_fired_;
            castVote(Phase::Prevote, std::nullopt, out);
            step_ = Step::Prevote;
        } else if (t.kind == 1 && step_ == Step::Prevote) {
            ++timeouts_fired_;
            castVote(Phase::Precommit, std::nullopt, out);
            step_ = Step::Precommit;
        } else if (t.kind == 2) {
            ++timeouts_fired_;
            startRound(round_ + 1, now, out);
            continue;
        } else {
            continue;
        }
        if (id_ == here) evaluate(now, out);
    }
}

// ---------------------------------------------------------------- messages

bool Node::acceptVote(const Vote& vote, bool relay, std::vector<Outgoing>& out) {
    if (vote.signer >= keys_.size() || vote.round > round_ + 1000000) return false;
    const auto key = std::make_tuple(vote.id.h, vote.id.k, vote.round, static_cast<std::uint8_t>(vote.phase), vote.signer);
    const auto existing = seen_.find(key);
    if (existing != seen_.end() && existing->second.value == vote.value) return false;  // duplicate (cheap path)
    if (!crypto_.verify(keys_[vote.signer], voteSigningPayload(engine_.policy().network_id, set_digest_, vote),
                        vote.signature))
        return false;
    if (existing != seen_.end()) {  // F15: equivocation, both votes signed by the same key
        if (evidenced_.insert(key).second) {
            evidence_.push_back({existing->second, vote});
            if (relay) broadcast(vote, out);  // let others see the conflict too
        }
        return false;
    }
    seen_.emplace(key, vote);
    (vote.phase == Phase::Prevote ? rounds_[vote.round].prevotes : rounds_[vote.round].precommits)[vote.signer] = vote;
    if (relay) broadcast(vote, out);
    return true;
}

bool Node::acceptProposal(const Proposal& p) {
    if (p.proposer >= keys_.size() || p.proposer != proposer(p.round) ||
        p.valid_round >= static_cast<std::int64_t>(p.round) || p.valid_round < -1)
        return false;
    auto& slot = rounds_[p.round].proposal;
    if (slot) return false;  // first proposal per round only
    if (!crypto_.verify(keys_[p.proposer], proposalSigningPayload(engine_.policy().network_id, set_digest_, p),
                        p.signature))
        return false;
    slot = p;
    values_.emplace(valueHash(p.value), p.value);
    return true;
}

void Node::buffer(const Message& message, const InstanceId& id, std::uint32_t from) {
    if (buffered_ >= kMaxBuffered) return;
    future_[id].emplace_back(message, from);
    ++buffered_;
}

void Node::requestSync(std::optional<std::uint32_t> to, std::uint64_t now, std::vector<Outgoing>& out) {
    auto& last = last_request_[{to.value_or(UINT32_MAX), id_}];
    if (last != 0 && now < last + timing_.base_ms) return;
    last = now + 1;
    out.push_back({to, SyncRequest{id_}});
}

void Node::serve(std::uint32_t to, const InstanceId& from, std::uint64_t now, std::vector<Outgoing>& out) {
    auto& last = last_serve_[{to, from}];
    if (last != 0 && now < last + timing_.base_ms) return;
    last = now + 1;
    std::size_t sent = 0;
    for (auto it = decisions_.lower_bound(from); it != decisions_.end() && sent < kServeBatch; ++it, ++sent)
        out.push_back({to, it->second});
}

void Node::receive(const Message& message, std::uint32_t from, std::uint64_t now, std::vector<Outgoing>& out) {
    auto route = [&](const InstanceId& id) {  // 0: current, 1: handled (past or future)
        if (id < id_) {
            serve(from, id, now, out);
            return 1;
        }
        if (id_ < id) {
            buffer(message, id, from);
            requestSync(from, now, out);
            return 1;
        }
        return 0;
    };
    // Submissions are handed to the engine once; rejected ones (e.g. anchored to a tip this node has
    // not reached yet) are not remembered, so a re-gossiped copy is tried again later.
    auto submit = [&](const Bytes& encoded, auto&& accept) {
        const Hash256 h = sha3(encoded);
        if (submitted_.count(h) || !accept()) return false;
        submitted_.insert(h);
        return true;
    };
    std::string error;
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Proposal>) {
                if (route(m.id)) return;
                if (acceptProposal(m)) {
                    if (relay_) broadcast(m, out);
                    evaluate(now, out);
                }
            } else if constexpr (std::is_same_v<T, Vote>) {
                if (route(m.id)) return;
                if (acceptVote(m, relay_, out)) evaluate(now, out);
            } else if constexpr (std::is_same_v<T, Decision>) {
                if (m.id < id_) return;
                if (id_ < m.id) {
                    buffer(message, m.id, from);
                    return;
                }
                const Hash256 h = valueHash(m.value);
                if (m.certificate.id != id_ || m.certificate.value != h ||
                    !verifyCertificate(m.certificate, keys_, set_digest_) || !valid(m.value))
                    return;
                values_.emplace(h, m.value);
                decide(m.value, m.certificate, now, out);
            } else if constexpr (std::is_same_v<T, SyncRequest>) {
                serve(from, m.from, now, out);
            } else if constexpr (std::is_same_v<T, RangeSubmission>) {
                if (submit(primechain::next::encode(m), [&] { return engine_.submitRange(m, error); })) propose(now, out);
            } else if constexpr (std::is_same_v<T, PrimeSubmission>) {
                if (submit(primechain::next::encode(m), [&] { return engine_.submitPrime(m, error); })) propose(now, out);
            } else if constexpr (std::is_same_v<T, Transfer>) {
                submit(primechain::next::encode(m), [&] { return engine_.submitTransfer(m, error); });
            } else if constexpr (std::is_same_v<T, GovernanceTx>) {
                submit(primechain::next::encode(m), [&] { return engine_.submitGovernance(m, error); });
            }
        },
        message);
}

// ---------------------------------------------------------------- certificates and decisions

std::optional<Certificate> Node::buildCertificate(std::uint64_t round, const Hash256& value) const {
    const auto it = rounds_.find(round);
    if (it == rounds_.end()) return std::nullopt;
    Certificate c{id_, round, value, std::vector<std::uint8_t>((keys_.size() + 7) / 8, 0), {}};
    for (const auto& [signer, v] : it->second.precommits)  // std::map: validator order
        if (v.value == value) {
            c.signers[signer / 8] |= static_cast<std::uint8_t>(1u << (signer % 8));
            c.signatures.push_back(v.signature);
        }
    if (c.signatures.size() < Engine::quorum(keys_.size())) return std::nullopt;
    return c;
}

bool Node::verifyCertificate(const Certificate& c, const std::vector<Bytes>& keys, const Hash256& set_digest) const {
    return consensus::verifyCertificate(c, keys, set_digest, engine_.policy().network_id, crypto_);
}

bool verifyCertificate(const Certificate& c, const std::vector<Bytes>& keys, const Hash256& set_digest,
                       const std::string& network_id, const Crypto& crypto) {
    const std::size_t n = keys.size();
    if (c.signers.size() != (n + 7) / 8) return false;
    std::size_t next = 0;
    for (std::size_t i = 0; i < c.signers.size() * 8; ++i) {
        if (!(c.signers[i / 8] >> (i % 8) & 1)) continue;
        if (i >= n || next >= c.signatures.size()) return false;
        const Vote v{c.id, c.round, Phase::Precommit, c.value, static_cast<std::uint32_t>(i), {}};
        if (!crypto.verify(keys[i], voteSigningPayload(network_id, set_digest, v), c.signatures[next++]))
            return false;
    }
    return next == c.signatures.size() && next >= Engine::quorum(n);
}

bool Node::applyValue(const Value& value, std::string& error) {
    if (value.kind == Value::Kind::Range) return engine_.applyDecidedRange(value.range, error);
    submitted_.clear();  // every queued submission was anchored to the previous tip
    return engine_.applyDecidedPrime(value.prime, value.contents, error).has_value();
}

// A3-3: proposer-skip counters, derived only from agreed data (the previous instance's
// certificate embedded in the decided value): rounds before the deciding round were missed
// turns; the deciding round's proposer and every signer reset to zero.
void Node::updateMissed(const Certificate& c) {
    if (previous_candidates_.empty()) return;
    for (std::uint64_t r = 0; r < c.round; ++r) {
        const auto idx = previous_candidates_[(c.id.h + c.id.k + r) % previous_candidates_.size()];
        ++missed_[sha3(previous_keys_[idx])];
    }
    const auto winner = previous_candidates_[(c.id.h + c.id.k + c.round) % previous_candidates_.size()];
    missed_.erase(sha3(previous_keys_[winner]));
    for (std::size_t i = 0; i < previous_keys_.size(); ++i)
        if (c.signers[i / 8] >> (i % 8) & 1) missed_.erase(sha3(previous_keys_[i]));
}

bool Node::decide(const Value& value, const Certificate& certificate, std::uint64_t now, std::vector<Outgoing>& out) {
    std::string error;
    if (!applyValue(value, error))  // the value was validated against this exact state: a bug if it fails
        throw std::logic_error("decided value does not apply at " + describe(id_, certificate.round) + ": " + error);
    if (value.previous) updateMissed(*value.previous);
    decisions_[id_] = Decision{id_, value, certificate};
    certificates_[id_] = certificate;
    previous_id_ = id_;
    previous_value_ = certificate.value;
    previous_keys_ = keys_;
    previous_digest_ = set_digest_;
    previous_candidates_ = candidates_;
    if (value.kind == Value::Kind::Prime) {
        if (engine_.validatorSetDigest() != set_digest_) set_change_heights_.insert(id_.h);  // last block of the old set
        id_ = {id_.h + 1, 0};
    } else {
        id_ = {id_.h, id_.k + 1};
    }
    prune();
    beginInstance(now, out);
    return true;
}

// A3-3 retention: certificates are kept permanently for blocks where the validator set changes and
// every K blocks, and for a window of the last W blocks; decisions (values) only for the window.
void Node::prune() {
    const std::uint64_t tip = engine_.height();
    const std::uint64_t floor = tip > retention_.window ? tip - retention_.window : 0;
    std::erase_if(certificates_, [&](const auto& e) {
        const std::uint64_t h = e.first.h;
        // Both sides of a set change: the last block of the old set and the first block of the new one.
        const bool transition = set_change_heights_.count(h) || (h > 0 && set_change_heights_.count(h - 1));
        return h <= floor && h % retention_.checkpoint_interval != 0 && !transition &&
               (!previous_id_ || e.first != *previous_id_);
    });
    std::erase_if(decisions_, [&](const auto& e) { return e.first.h <= floor; });
}

}  // namespace primechain::next::consensus
