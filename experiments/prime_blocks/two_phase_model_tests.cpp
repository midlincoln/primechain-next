// Executable model of the proposed two-phase round rules for PrimeChain Next.
//
// The rules follow Tendermint (Buchman, Kwon, Milosevic 2018, Algorithm 1) for
// one consensus position: rotating proposer, PREVOTE then PRECOMMIT, lock only
// on a quorum of prevotes (a "polka"), carry validValue/validRound forward,
// unlock when a later polka justifies another value, accept any quorum of
// signers, and decide on a quorum of precommits. The quorum is the inherited
// q = ceil(2n/3). See docs/round-change-audit.md (points 2-4).
//
// Checks:
//   1. The G2 scenario that stalls the inherited rules decides here without C.
//   2. With one validator permanently down, the other two always decide.
//   2a. A lock protects a value another validator may already have decided.
//   2b. A Byzantine proposer cannot unlock validators with an unproven valid round.
//   3. Random crash/omission schedules never decide two values (n = 3 and 4).
//   4. n = 4 with one equivocating (Byzantine) validator: random schedules never
//      decide two values.
//   5. n = 3 with one Byzantine validator: a scripted fork exists, as n >= 3f+1 predicts.
//
// A model of rules, not an implementation: no timers, signatures, or transport.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kNone = -1;
constexpr int kNil = -2;

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

enum class Type { Proposal, Prevote, Precommit };
enum class Step { Propose, Prevote, Precommit };

struct Message {
    Type type;
    int sender;
    int round;
    int value;        // kNil for nil votes
    int valid_round;  // proposals only, kNone if fresh
    unsigned known;   // bitmask of validators that have received it
};

struct Validator {
    int round{1};
    Step step{Step::Propose};
    int locked_value{kNone}, locked_round{kNone};
    int valid_value{kNone}, valid_round{kNone};
    int decided{kNone};
    bool alive{true};
    bool byzantine{false};
    std::vector<int> polka_done;  // rounds where the "first polka" rule fired
};

class TwoPhaseModel {
public:
    TwoPhaseModel(int n, int base) : n_(n), q_((2 * n + 2) / 3), base_(base), v_(n) {}

    int n() const { return n_; }
    int quorum() const { return q_; }
    int proposer(int r) const { return (base_ + r - 1) % n_; }
    Validator& validator(int i) { return v_[i]; }
    const std::vector<Message>& messages() const { return msgs_; }

    int send(int from, Type type, int round, int value, int valid_round = kNone) {
        msgs_.push_back({type, from, round, value, valid_round, 1u << from});
        return static_cast<int>(msgs_.size()) - 1;
    }
    void deliver(int to, int id) { if (v_[to].alive) msgs_[id].known |= 1u << to; }
    void deliverAll(int to, const std::vector<int>& from) {
        if (!v_[to].alive) return;
        for (auto& m : msgs_)
            for (int f : from) if (m.known >> f & 1u) m.known |= 1u << to;
    }

    // Distinct senders of matching messages known to validator `at`.
    int count(int at, Type type, int round, int value, bool any_value = false) const {
        unsigned senders = 0;
        for (const auto& m : msgs_)
            if ((m.known >> at & 1u) && m.type == type && m.round == round && (any_value || m.value == value))
                senders |= 1u << m.sender;
        return __builtin_popcount(senders);
    }
    // A proposal from the round's proposer known to `at` (first matching, any value).
    const Message* proposalFor(int at, int round, int value = kNone) const {
        for (const auto& m : msgs_)
            if ((m.known >> at & 1u) && m.type == Type::Proposal && m.round == round &&
                m.sender == proposer(round) && (value == kNone || m.value == value))
                return &m;
        return nullptr;
    }

    bool sent(int i, Type type, int round) const {
        for (const auto& m : msgs_) if (m.sender == i && m.type == type && m.round == round) return true;
        return false;
    }

    // ---- honest rules (Tendermint Algorithm 1, lines noted) ----
    bool propose(int i, int fresh) {  // lines 14-19
        auto& s = v_[i];
        if (!honestActive(i) || proposer(s.round) != i || sent(i, Type::Proposal, s.round)) return false;
        const int value = s.valid_value != kNone ? s.valid_value : fresh;
        send(i, Type::Proposal, s.round, value, s.valid_round);
        return true;
    }
    bool prevoteOnProposal(int i) {  // lines 22-33
        auto& s = v_[i];
        if (!honestActive(i) || s.step != Step::Propose) return false;
        const Message* p = proposalFor(i, s.round);
        if (!p) return false;
        int vote;
        if (p->valid_round == kNone) {
            vote = (s.locked_round == kNone || s.locked_value == p->value) ? p->value : kNil;
        } else if (p->valid_round < s.round && count(i, Type::Prevote, p->valid_round, p->value) >= q_) {
            vote = (s.locked_round <= p->valid_round || s.locked_value == p->value) ? p->value : kNil;
        } else {
            return false;
        }
        send(i, Type::Prevote, s.round, vote);
        s.step = Step::Prevote;
        return true;
    }
    bool prevoteNilOnTimeout(int i) {  // lines 57-60
        auto& s = v_[i];
        if (!honestActive(i) || s.step != Step::Propose) return false;
        send(i, Type::Prevote, s.round, kNil);
        s.step = Step::Prevote;
        return true;
    }
    bool polka(int i) {  // lines 36-43: lock only on a quorum of prevotes
        auto& s = v_[i];
        if (!honestActive(i) || s.step == Step::Propose) return false;
        if (std::find(s.polka_done.begin(), s.polka_done.end(), s.round) != s.polka_done.end()) return false;
        for (const auto& m : msgs_) {
            if (!(m.known >> i & 1u) || m.type != Type::Proposal || m.round != s.round || m.sender != proposer(s.round))
                continue;
            if (count(i, Type::Prevote, s.round, m.value) < q_) continue;
            const int value = m.value;  // copy: send() below may reallocate msgs_
            s.polka_done.push_back(s.round);
            if (s.step == Step::Prevote) {
                s.locked_value = value;
                s.locked_round = s.round;
                send(i, Type::Precommit, s.round, value);
                s.step = Step::Precommit;
            }
            s.valid_value = value;
            s.valid_round = s.round;
            return true;
        }
        return false;
    }
    bool precommitNil(int i) {  // lines 34-35 + 44-46 + 61-64
        auto& s = v_[i];
        if (!honestActive(i) || s.step != Step::Prevote) return false;
        if (count(i, Type::Prevote, s.round, 0, true) < q_) return false;
        send(i, Type::Precommit, s.round, kNil);
        s.step = Step::Precommit;
        return true;
    }
    bool decide(int i) {  // lines 49-54: any round, any quorum of precommits
        auto& s = v_[i];
        if (!honestActive(i)) return false;
        for (const auto& m : msgs_) {
            if (!(m.known >> i & 1u) || m.type != Type::Proposal || m.sender != proposer(m.round)) continue;
            if (count(i, Type::Precommit, m.round, m.value) >= q_) { s.decided = m.value; return true; }
        }
        return false;
    }
    bool nextRound(int i, int max_round) {  // lines 47-48 + 65-67
        auto& s = v_[i];
        if (!honestActive(i) || s.round >= max_round) return false;
        if (count(i, Type::Precommit, s.round, 0, true) < q_) return false;
        startRound(i, s.round + 1);
        return true;
    }
    bool skipRound(int i) {  // lines 55-56, threshold n - q + 1 (at least one honest sender)
        auto& s = v_[i];
        if (!honestActive(i)) return false;
        int best = 0;
        for (const auto& m : msgs_) {
            if (!(m.known >> i & 1u) || m.round <= s.round) continue;
            unsigned senders = 0;
            for (const auto& o : msgs_) if ((o.known >> i & 1u) && o.round == m.round) senders |= 1u << o.sender;
            if (__builtin_popcount(senders) >= n_ - q_ + 1) best = std::max(best, m.round);
        }
        if (!best) return false;
        startRound(i, best);
        return true;
    }

    // Every enabled honest action for i, in protocol order. Returns true if anything happened.
    bool stepAll(int i, int fresh) {
        bool any = false;
        any |= decide(i);
        any |= propose(i, fresh);
        any |= prevoteOnProposal(i);
        any |= polka(i);
        any |= decide(i);
        return any;
    }

    std::vector<int> honestDecisions() const {
        std::vector<int> out;
        for (const auto& s : v_)
            if (!s.byzantine && s.decided != kNone && std::find(out.begin(), out.end(), s.decided) == out.end())
                out.push_back(s.decided);
        return out;
    }

private:
    bool honestActive(int i) const { return v_[i].alive && !v_[i].byzantine && v_[i].decided == kNone; }
    void startRound(int i, int r) { v_[i].round = r; v_[i].step = Step::Propose; }

    int n_, q_, base_;
    std::vector<Validator> v_;
    std::vector<Message> msgs_;
};

// Drive honest validators in `group` with full delivery among them until all decide or max_round.
void runGroup(TwoPhaseModel& m, const std::vector<int>& group, int max_round) {
    for (int iteration = 0; iteration < 8 * max_round; ++iteration) {
        bool progress = false;
        for (int i : group) m.deliverAll(i, group);
        for (int i : group) progress |= m.stepAll(i, 100 + i);
        for (int i : group) m.deliverAll(i, group);
        for (int i : group) progress |= m.polka(i) | m.decide(i);
        bool all = true;
        for (int i : group) all = all && m.validator(i).decided != kNone;
        if (all) return;
        if (!progress) {  // timeouts fire
            for (int i : group) m.prevoteNilOnTimeout(i);
            for (int i : group) m.deliverAll(i, group);
            for (int i : group) m.precommitNil(i);
            for (int i : group) m.deliverAll(i, group);
            for (int i : group) m.nextRound(i, max_round);
            for (int i : group) m.skipRound(i);
        }
    }
}

// 1. The inherited-rules G2 scenario: A's proposal X is lost, B proposes Y in round 2, C crashes.
void g2DecidesWithoutC() {
    for (int base = 0; base < 3; ++base) {
        TwoPhaseModel m(3, base);
        const int a = m.proposer(1), b = m.proposer(2), c = m.proposer(3);
        const int x = 100 + a, y = 100 + b;
        require(m.propose(a, x), "A proposes X in round 1");
        require(m.prevoteOnProposal(a), "A prevotes X");  // A's own vote, not a lock
        require(m.validator(a).locked_round == kNone, "a single prevote is not a lock");
        // B and C never receive X; they time out and prevote nil, then everyone precommits nil.
        require(m.prevoteNilOnTimeout(b) && m.prevoteNilOnTimeout(c), "B and C prevote nil");
        for (int i : {a, b, c}) m.deliverAll(i, {a, b, c});
        for (int i : {a, b, c}) require(m.precommitNil(i), "round 1 ends with nil precommits");
        for (int i : {a, b, c}) m.deliverAll(i, {a, b, c});
        for (int i : {a, b, c}) require(m.nextRound(i, 64), "all move to round 2");
        // Round 2: B proposes Y. C crashes before voting.
        require(m.propose(b, y), "B proposes Y");
        m.validator(c).alive = false;
        runGroup(m, {a, b}, 12);
        require(m.validator(a).decided == y && m.validator(b).decided == y,
                "G2 scenario: A and B decide Y without C");
    }
}

// 2. Any one validator permanently down from the start: the other two always decide.
void oneDownAlwaysDecides() {
    for (int base = 0; base < 3; ++base)
        for (int down = 0; down < 3; ++down) {
            TwoPhaseModel m(3, base);
            m.validator(down).alive = false;
            std::vector<int> group;
            for (int i = 0; i < 3; ++i) if (i != down) group.push_back(i);
            runGroup(m, group, 12);
            for (int i : group) require(m.validator(i).decided != kNone, "two live validators decide");
            require(m.honestDecisions().size() == 1, "and agree");
        }
}

// The lock protects a value that may already be decided (n = 3, no faults beyond loss).
// A decides X in round 1; B is locked on X but never learns the decision; the round-2
// proposer C never saw X and proposes Z. B must refuse Z, and B and C must decide X.
void lockProtectsPossibleDecision() {
    for (int base = 0; base < 3; ++base) {
        TwoPhaseModel m(3, base);
        const int a = m.proposer(1), c = m.proposer(2), b = m.proposer(3);
        const int x = 100 + a, z = 100 + c;
        auto last = [&] { return static_cast<int>(m.messages().size()) - 1; };
        require(m.propose(a, x), "A proposes X");
        m.deliver(b, last());
        require(m.prevoteOnProposal(a) && m.prevoteOnProposal(b), "A and B prevote X");
        const int b_prevote = last();
        m.deliverAll(a, {b});
        m.deliverAll(b, {a});
        require(m.polka(a) && m.polka(b), "A and B see a polka and lock X");
        const int b_precommit = last();
        m.deliver(a, b_precommit);
        require(m.decide(a) && m.validator(a).decided == x, "A decides X");
        // C never saw the proposal; B never sees A's precommit.
        require(m.prevoteNilOnTimeout(c), "C prevotes nil");
        m.deliver(c, b_prevote);
        require(m.precommitNil(c), "C precommits nil");
        m.deliver(b, last());
        m.deliver(c, b_precommit);
        require(m.nextRound(b, 64) && m.nextRound(c, 64), "B and C reach round 2");
        require(m.propose(c, z), "C proposes Z without knowing X");
        m.deliver(b, last());
        require(m.prevoteOnProposal(b), "B votes on C's proposal");
        require(m.messages().back().value == kNil, "locked B prevotes nil, not Z");
        runGroup(m, {b, c}, 16);
        require(m.validator(b).decided == x && m.validator(c).decided == x,
                "B and C decide X, the value A already decided");
    }
}

// n = 4, one Byzantine proposer D cannot unlock honest validators by claiming a
// valid round that has no quorum of prevotes behind it.
void fakeValidRoundRejected() {
    for (int base = 0; base < 4; ++base) {
        TwoPhaseModel m(4, base);
        const int a = m.proposer(1), d = m.proposer(2);
        std::vector<int> rest;
        for (int i = 0; i < 4; ++i) if (i != a && i != d) rest.push_back(i);
        const int b = rest[0], c = rest[1];
        m.validator(d).byzantine = true;
        const int x = 100, y = 101;
        require(m.propose(a, x), "A proposes X");
        m.deliverAll(b, {a});
        m.deliverAll(c, {a});
        for (int i : {a, b, c}) require(m.prevoteOnProposal(i), "honest prevote X");
        for (int i : {a, b, c}) m.deliverAll(i, {a, b, c});
        for (int i : {a, b, c}) require(m.polka(i) && m.validator(i).locked_value == x, "honest lock X");
        m.deliverAll(a, {b, c});
        require(m.decide(a) && m.validator(a).decided == x, "A decides X with 3 of 4 precommits");
        // B and C miss A's precommit; Byzantine D precommits nil to move them on.
        const int d_nil = m.send(d, Type::Precommit, 1, kNil);
        for (int i : {b, c}) { m.deliver(i, d_nil); m.deliverAll(i, {b, c}); }
        for (int i : {b, c}) require(m.nextRound(i, 64), "B and C reach round 2");
        // D proposes Y claiming valid round 1, backed only by its own fake prevote.
        const int fake = m.send(d, Type::Prevote, 1, y);
        const int proposal = m.send(d, Type::Proposal, 2, y, 1);
        const int d_vote = m.send(d, Type::Prevote, 2, y);
        for (int i : {b, c}) for (int id : {fake, proposal, d_vote}) m.deliver(i, id);
        for (int i : {b, c})
            require(!m.prevoteOnProposal(i), "unproven valid round does not unlock an honest validator");
        for (int i : {b, c}) require(m.prevoteNilOnTimeout(i), "they time out and prevote nil");
        for (int i : {b, c}) m.deliverAll(i, {b, c});
        for (int i : {b, c}) require(!m.polka(i) && m.validator(i).locked_value == x, "lock on X survives");
        const auto decided = m.honestDecisions();
        require(decided.size() == 1 && decided.front() == x, "no honest validator decides Y");
    }
}

// 3/4. Random schedules. Byzantine validators send arbitrary, equivocating messages.
std::size_t randomRuns(int n, int byzantine, std::uint32_t runs, std::uint32_t seed, bool expect_safe) {
    std::mt19937 rng(seed);
    std::size_t decided = 0, violations = 0;
    for (std::uint32_t run = 0; run < runs; ++run) {
        TwoPhaseModel m(n, static_cast<int>(rng() % n));
        for (int b = 0; b < byzantine; ++b) m.validator(b).byzantine = true;
        const int max_round = 2 + static_cast<int>(rng() % 5);
        for (int step = 0; step < 300; ++step) {
            const int i = static_cast<int>(rng() % n);
            if (m.validator(i).byzantine) {
                const int r = 1 + static_cast<int>(rng() % max_round);
                const int value = (rng() % 4 == 0) ? kNil : 100 + static_cast<int>(rng() % 2);
                const auto type = static_cast<Type>(rng() % 3);
                if (type != Type::Proposal || m.proposer(r) == i)
                    m.send(i, type, r, type == Type::Proposal && value == kNil ? 100 : value,
                           type == Type::Proposal && rng() % 2 ? static_cast<int>(rng() % r) : kNone);
                continue;
            }
            switch (rng() % 12) {
            case 0: m.propose(i, 100 + static_cast<int>(rng() % 2)); break;
            case 1: m.prevoteOnProposal(i); break;
            case 2: if (rng() % 4 == 0) m.prevoteNilOnTimeout(i); break;  // timeouts fire less often
            case 3: m.polka(i); break;
            case 4: if (rng() % 4 == 0) m.precommitNil(i); break;
            case 5: m.decide(i); break;
            case 6: m.nextRound(i, max_round); m.skipRound(i); break;
            case 7: case 8:
                if (!m.messages().empty()) m.deliver(i, static_cast<int>(rng() % m.messages().size()));
                break;
            case 9: if (rng() % 50 == 0) m.validator(i).alive = !m.validator(i).alive; break;
            case 10: case 11: m.deliverAll(i, {static_cast<int>(rng() % n)}); break;  // gossip from one peer
            }
            if (m.honestDecisions().size() > 1) { ++violations; break; }
        }
        if (!m.honestDecisions().empty()) ++decided;
    }
    std::cout << "random n=" << n << " byzantine=" << byzantine << ": runs " << runs
              << ", decided " << decided << ", conflicting decisions " << violations << '\n';
    if (expect_safe) {
        require(violations == 0, "two-phase rules decided two values (n=" + std::to_string(n) +
                                 ", byzantine=" + std::to_string(byzantine) + ")");
        require(decided > runs / 20, "random schedules reach decisions often enough to be meaningful");
    }
    return violations;
}

// 5. n = 3 cannot tolerate one Byzantine validator: equivocating proposer B forks A and C.
void threeValidatorsByzantineFork() {
    TwoPhaseModel f(3, 1);  // base 1: validator 1 (B) proposes round 1
    const int A = 0, B = 1, C = 2;
    f.validator(B).byzantine = true;
    const int px = f.send(B, Type::Proposal, 1, 100), py = f.send(B, Type::Proposal, 1, 101);
    const int vx = f.send(B, Type::Prevote, 1, 100), vy = f.send(B, Type::Prevote, 1, 101);
    const int cx = f.send(B, Type::Precommit, 1, 100), cy = f.send(B, Type::Precommit, 1, 101);
    for (int id : {px, vx, cx}) f.deliver(A, id);
    for (int id : {py, vy, cy}) f.deliver(C, id);
    require(f.prevoteOnProposal(A) && f.prevoteOnProposal(C), "honest A and C prevote what they saw");
    require(f.polka(A) && f.polka(C), "each sees a 2-of-3 polka thanks to B's double vote");
    require(f.decide(A) && f.decide(C), "each sees 2-of-3 precommits");
    require(f.validator(A).decided == 100 && f.validator(C).decided == 101,
            "counterexample: n=3 forks with one Byzantine validator (n >= 3f+1 violated)");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::uint32_t runs = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 10000;
        g2DecidesWithoutC();
        oneDownAlwaysDecides();
        lockProtectsPossibleDecision();
        fakeValidRoundRejected();
        randomRuns(3, 0, runs, 1, true);
        randomRuns(4, 0, runs, 2, true);
        randomRuns(4, 1, runs, 3, true);
        threeValidatorsByzantineFork();
        std::cout << "round_change_two_phase_model: PASS\nchecks: " << checks
                  << "\nG2 decides without C; one-down liveness; no conflicting decisions for"
                     " n=3,4 crash/omission and n=4 with one Byzantine; n=3 Byzantine fork reproduced\n"
                  << "model of proposed rules, NOT an implementation or a proof\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "two-phase model test failed: " << e.what() << '\n';
        return 1;
    }
}
