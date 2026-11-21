// Executable model of the inherited v1 finalization/round-change rules.
//
// Mirrors, in abstract form, the rules in src/node/sync_server.cpp
// (finalizationCommitteeOrdered, makeLocalFinalizationVote,
// makeLocalRoundChangeVote, activeFinalizationRound) and
// src/protocol/records.cpp (verifyRoundChangeCertificate,
// verifyRecordFinalization) for one position (previous_hash, integer):
//
//   - committee of exactly q = ceil(2n/3) validators per round, rotated from a
//     position-derived base, first member is the proposer;
//   - non-proposers sign only the proposer's signed candidate for their active round;
//   - a validator's first signature is a permanent lock on that candidate;
//   - a round change needs q round-change votes, each reporting the sender's lock;
//   - a later-round candidate must equal the highest reported lock, and tied
//     conflicting highest locks are invalid;
//   - a record is final with signatures from the whole assigned committee.
//
// It is a model, not the live code: signatures, records, and transport are
// abstracted to integers and delivery sets. See docs/round-change-audit.md (G2).
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kN = 3;
constexpr int kQ = 2;  // ceil(2 * 3 / 3), as in core::requiredValidatorQuorum.
constexpr int kNone = -1;

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct Sig { int v; int c; int r; std::uint64_t cert; };      // cert: round-change ids carried by a proposal
struct RoundChange { int v; int r; int lock_c; int lock_r; };

struct Validator {
    int lock_c{kNone};
    int lock_r{0};
    std::uint64_t signed_rounds{0};
    std::uint64_t rc_rounds{0};
    std::uint64_t known_sigs{0};
    std::uint64_t known_rcs{0};
    bool alive{true};
};

class V1Model {
public:
    explicit V1Model(int base) : base_(base) {}

    std::array<int, kQ> committee(int r) const {
        const int start = (base_ + (r - 1)) % kN;
        std::array<int, kQ> members{};
        for (int i = 0; i < kQ; ++i) members[i] = (start + i) % kN;
        const int offset = ((r - 1) / kN) % kQ;
        std::rotate(members.begin(), members.begin() + offset, members.end());
        return members;
    }
    int proposer(int r) const { return committee(r).front(); }
    bool inCommittee(int v, int r) const {
        const auto members = committee(r);
        return std::find(members.begin(), members.end(), v) != members.end();
    }

    int activeRound(int v) const {
        int active = 1;
        for (int r = 2; r < 64; ++r) {
            int count = 0;
            for (std::size_t i = 0; i < rcs_.size(); ++i)
                if ((validators_[v].known_rcs >> i & 1u) && rcs_[i].r == r) ++count;
            if (count >= kQ) active = r;
        }
        return active;
    }

    // Returns the forced candidate, kNone when the proposer is free, or nullopt when invalid.
    std::optional<int> justified(int r, std::uint64_t cert) const {
        int count = 0, highest_r = 0, highest_c = kNone;
        bool tie = false;
        for (std::size_t i = 0; i < rcs_.size(); ++i) {
            if (!(cert >> i & 1u)) continue;
            if (rcs_[i].r != r) return std::nullopt;
            ++count;
            if (rcs_[i].lock_r > highest_r) { highest_r = rcs_[i].lock_r; highest_c = rcs_[i].lock_c; tie = false; }
            else if (rcs_[i].lock_r == highest_r && highest_r > 0 && rcs_[i].lock_c != highest_c) tie = true;
        }
        if (r == 1) return count == 0 ? std::optional<int>(kNone) : std::nullopt;
        if (count < kQ || tie) return std::nullopt;
        return highest_c;
    }

    // The assigned proposer signs a candidate for its active round. `fresh` models
    // the miner submission it would choose when no lock constrains it.
    std::optional<int> propose(int v, int fresh) {
        auto& self = validators_[v];
        const int r = activeRound(v);
        if (!self.alive || proposer(r) != v || (self.signed_rounds >> r & 1u)) return std::nullopt;
        std::uint64_t cert = 0;
        for (std::size_t i = 0; i < rcs_.size(); ++i)
            if ((self.known_rcs >> i & 1u) && rcs_[i].r == r) cert |= 1ull << i;
        const auto forced = justified(r, cert);
        if (!forced) return std::nullopt;
        const int c = *forced == kNone ? fresh : *forced;
        if (self.lock_c != kNone && self.lock_c != c) return std::nullopt;  // safety lock
        return sign(v, c, r, cert);
    }

    // A committee member co-signs a proposal it has received.
    bool cosign(int v, int sig_id) {
        auto& self = validators_[v];
        const auto proposal = sigs_[sig_id];
        if (!self.alive || !(self.known_sigs >> sig_id & 1u) || proposal.v != proposer(proposal.r) ||
            proposal.v == v || !inCommittee(v, proposal.r) || (self.signed_rounds >> proposal.r & 1u))
            return false;
        self.known_rcs |= proposal.cert;  // importCertifiedRoundChanges
        const auto forced = justified(proposal.r, proposal.cert);
        if (!forced || (*forced != kNone && *forced != proposal.c)) return false;
        if (activeRound(v) != proposal.r) return false;
        if (self.lock_c != kNone && self.lock_c != proposal.c) return false;  // safety lock
        sign(v, proposal.c, proposal.r, 0);
        return true;
    }

    bool roundChange(int v, int max_round) {
        auto& self = validators_[v];
        const int r = activeRound(v) + 1;
        if (!self.alive || r > max_round || (self.rc_rounds >> r & 1u)) return false;
        const bool report = self.lock_c != kNone && self.lock_r < r;
        rcs_.push_back({v, r, report ? self.lock_c : kNone, report ? self.lock_r : 0});
        self.rc_rounds |= 1ull << r;
        self.known_rcs |= 1ull << (rcs_.size() - 1);
        return true;
    }

    void deliverAll(int to, const std::vector<int>& from) {
        auto& target = validators_[to];
        if (!target.alive) return;
        for (int f : from) {
            target.known_sigs |= validators_[f].known_sigs;
            target.known_rcs |= validators_[f].known_rcs;
        }
    }
    void deliverSig(int to, int sig_id) { if (validators_[to].alive) validators_[to].known_sigs |= 1ull << sig_id; }
    void deliverRc(int to, int rc_id) { if (validators_[to].alive) validators_[to].known_rcs |= 1ull << rc_id; }

    // Values certified by a full assigned committee in some round.
    std::vector<int> decisions() const {
        std::vector<int> out;
        for (int r = 1; r < 64; ++r) {
            const auto members = committee(r);
            for (const auto& s : sigs_) {
                if (s.r != r || s.v != members.front()) continue;
                bool all = true;
                for (int m : members) {
                    bool found = false;
                    for (const auto& t : sigs_) found = found || (t.v == m && t.r == r && t.c == s.c);
                    all = all && found;
                }
                if (all && std::find(out.begin(), out.end(), s.c) == out.end()) out.push_back(s.c);
            }
        }
        return out;
    }

    Validator& validator(int v) { return validators_[v]; }
    const std::vector<Sig>& sigs() const { return sigs_; }
    const std::vector<RoundChange>& rcs() const { return rcs_; }

private:
    int sign(int v, int c, int r, std::uint64_t cert) {
        require(sigs_.size() < 64 && rcs_.size() < 64, "model message bound");
        auto& self = validators_[v];
        sigs_.push_back({v, c, r, cert});
        self.signed_rounds |= 1ull << r;
        self.known_sigs |= 1ull << (sigs_.size() - 1);
        self.lock_c = c;
        self.lock_r = std::max(self.lock_r, r);
        return c;
    }

    int base_;
    std::array<Validator, kN> validators_{};
    std::vector<Sig> sigs_;
    std::vector<RoundChange> rcs_;
};

int lastRc(const V1Model& m) { return static_cast<int>(m.rcs().size()) - 1; }

// G2: A locks X without a certificate, B locks Y in the next round, C crashes.
// A and B are honest, online, and form a 2-of-3 quorum, yet cannot finalize.
void g2PermanentLockStall() {
    for (int base = 0; base < kN; ++base) {
        V1Model m(base);
        const int a = m.proposer(1);
        const int b = m.committee(1)[1];
        const int c = 3 - a - b;
        const int x = 100 + a, y = 100 + b;

        // Round 1: A proposes X; the message to B is lost. A is now locked on X@1.
        require(m.propose(a, x) == x, "round-one proposer signs X");
        require(m.decisions().empty(), "X is not certified");

        // B and C time out and certify round 2 without A's lock.
        require(m.roundChange(b, 2) && m.roundChange(c, 2), "B and C sign round change");
        m.deliverRc(c, lastRc(m) - 1);
        m.deliverRc(b, lastRc(m));
        require(m.activeRound(b) == 2 && m.activeRound(c) == 2, "round 2 active at B and C");
        require(m.committee(2)[0] == b && m.committee(2)[1] == c, "round-2 committee is {B, C} with B proposing");

        // Round 2: no lock is reported, so B is free and proposes Y. C crashes before co-signing.
        require(m.propose(b, y) == y, "round-two proposer signs Y");
        m.validator(c).alive = false;
        require(m.decisions().empty(), "Y is not certified");
        require(m.validator(a).lock_c == x && m.validator(b).lock_c == y, "A locked on X@1, B locked on Y@2");

        // A and B now see every message from each other and try every enabled action
        // for 30 further rounds. Committees cycle with period n * q = 6.
        int rounds_with_ab_committee = 0;
        for (int step = 0; step < 30; ++step) {
            m.deliverAll(a, {b});
            m.deliverAll(b, {a});
            for (int v : {a, b}) m.propose(v, 100 + v);
            m.deliverAll(a, {b});
            m.deliverAll(b, {a});
            for (int v : {a, b})
                for (int s = 0; s < static_cast<int>(m.sigs().size()); ++s) m.cosign(v, s);
            const int r = m.activeRound(a);
            if (m.inCommittee(a, r) && m.inCommittee(b, r)) ++rounds_with_ab_committee;
            require(m.decisions().empty(), "G2: A and B never certify while C is down");
            m.roundChange(a, 64);
            m.roundChange(b, 64);
        }
        require(rounds_with_ab_committee >= 5, "{A, B} committee came up repeatedly and still failed");
        require(m.activeRound(a) >= 25, "rounds kept advancing (liveness of round change, not of decisions)");
        // Every round-change certificate carries B's Y@2, so every candidate is Y, and A's X lock refuses Y.

        // C returns. The next {B, C} committee certifies Y; C was never locked.
        m.validator(c).alive = true;
        for (int step = 0; step < 12 && m.decisions().empty(); ++step) {
            for (int v = 0; v < kN; ++v) m.deliverAll(v, {0, 1, 2});
            for (int v = 0; v < kN; ++v) m.propose(v, 100 + v);
            for (int v = 0; v < kN; ++v) m.deliverAll(v, {0, 1, 2});
            for (int v = 0; v < kN; ++v)
                for (int s = 0; s < static_cast<int>(m.sigs().size()); ++s) m.cosign(v, s);
            if (!m.decisions().empty()) break;
            for (int v = 0; v < kN; ++v) m.roundChange(v, 64);
            for (int v = 0; v < kN; ++v) m.deliverAll(v, {0, 1, 2});
        }
        const auto decided = m.decisions();
        require(decided.size() == 1 && decided.front() == y, "after C returns, exactly Y is certified");
    }
}

// Randomized schedules (omission and crash faults only, no equivocating signer):
// the inherited rules never certify two different values for one position.
void randomSafety(std::uint32_t runs) {
    std::mt19937 rng(20261121);
    std::size_t decided_runs = 0, multi_round_decisions = 0;
    for (std::uint32_t run = 0; run < runs; ++run) {
        V1Model m(static_cast<int>(rng() % kN));
        const int max_round = 2 + static_cast<int>(rng() % 6);
        for (int step = 0; step < 120; ++step) {
            const int v = static_cast<int>(rng() % kN);
            switch (rng() % 6) {
            case 0: m.propose(v, 100 + static_cast<int>(rng() % 3)); break;
            case 1: if (!m.sigs().empty()) m.cosign(v, static_cast<int>(rng() % m.sigs().size())); break;
            case 2: m.roundChange(v, max_round); break;
            case 3: if (!m.sigs().empty()) m.deliverSig(v, static_cast<int>(rng() % m.sigs().size())); break;
            case 4: if (!m.rcs().empty()) m.deliverRc(v, static_cast<int>(rng() % m.rcs().size())); break;
            case 5: if (rng() % 40 == 0) m.validator(v).alive = !m.validator(v).alive; break;
            }
            require(m.decisions().size() <= 1, "v1 rules certified two values under crash/omission faults");
        }
        if (!m.decisions().empty()) ++decided_runs;
        for (const auto& s : m.sigs()) if (s.r > 1 && m.decisions().size() == 1 && s.c == m.decisions().front()) { ++multi_round_decisions; break; }
    }
    require(decided_runs > runs / 10, "random schedules reach decisions often enough to be meaningful");
    require(multi_round_decisions > 0, "random schedules exercise later-round decisions");
    std::cout << "random schedules: " << runs << ", with a decision: " << decided_runs
              << ", decided value signed in a later round: " << multi_round_decisions << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::uint32_t runs = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 20000;
        g2PermanentLockStall();
        randomSafety(runs);
        std::cout << "round_change_v1_model: PASS\nchecks: " << checks
                  << "\nexpected: G2 stall reproduced for every committee base; no conflicting"
                     " certificates under crash/omission faults\n"
                  << "model of inherited rules, NOT the live code or a Byzantine-safety proof\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "round-change model test failed: " << e.what() << '\n';
        return 1;
    }
}
