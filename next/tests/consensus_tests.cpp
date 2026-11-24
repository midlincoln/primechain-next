// Tests for PrimeChain Next A3.1: the consensus core on a simulated network.
// Usage: primechain-next-consensus-tests chain|crash|g2|partition|byzantine4|byzantine3|governance|retention|header|fuzz [n]
#include "primechain/crypto/signature.hpp"
#include "primechain_next/consensus.hpp"
#include "primechain_next/merkle.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>

using namespace primechain::next;
using namespace primechain::next::consensus;
namespace crypto = primechain::crypto;
namespace fs = std::filesystem;

namespace {

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error("FAILED: " + message);
}

std::uint64_t smallestFactor(std::uint64_t n) {
    for (std::uint64_t d = 2; d * d <= n; ++d)
        if (n % d == 0) return d;
    return n;
}
bool isPrime(std::uint64_t n) { return n >= 2 && smallestFactor(n) == n; }
std::uint64_t nextPrime(std::uint64_t n) { do ++n; while (!isPrime(n)); return n; }
std::uint64_t primitiveRoot(std::uint64_t p) {
    std::vector<std::uint64_t> qs;
    for (std::uint64_t m = p - 1, d = 2; m > 1; ++d)
        if (m % d == 0) {
            qs.push_back(d);
            while (m % d == 0) m /= d;
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

struct Key {
    crypto::SignatureKeyPair pair;
    primechain::Address address;
};
Key newKey() {
    std::string error;
    auto pair = crypto::generateProtocolSignatureKeyPair(error);
    require(pair.has_value(), "key generation: " + error);
    return {*pair, crypto::addressFromProtocolPublicKey(pair->public_key)};
}
Bytes sign(const Key& k, const Bytes& payload) {
    std::string error;
    auto s = crypto::signProtocolMessage(k.pair.private_key, payload, error);
    require(s.has_value(), "signing: " + error);
    return *s;
}

struct Validator {
    Key consensus;
    Key payout;
    ValidatorRecord record;
};
Validator makeValidator(const std::string& network_id) {
    Validator v{newKey(), newKey(), {}};
    v.record.consensus_key = v.consensus.pair.public_key;
    v.record.payout_address = v.payout.address;
    v.record.payout_key = v.payout.pair.public_key;
    const Bytes payload = validatorBindingPayload(network_id, v.record);
    v.record.consensus_signature = sign(v.consensus, payload);
    v.record.payout_signature = sign(v.payout, payload);
    return v;
}

RangeSubmission makeRange(const Engine& e, const Key& m, std::uint64_t start, std::uint64_t end) {
    RangeSubmission r;
    r.network_id = e.policy().network_id;
    r.anchor = e.tip();
    r.start = start;
    r.end = end;
    for (auto n = start; n <= end; ++n) r.divisors.emplace_back(smallestFactor(n));
    r.provider = m.address;
    r.public_key = m.pair.public_key;
    r.signature = sign(m, rangeSigningPayload(r));
    return r;
}

PrimeSubmission makePrime(const Engine& e, const Key& m, std::uint64_t p) {
    PrimeSubmission s;
    s.network_id = e.policy().network_id;
    s.anchor = e.tip();
    s.prime = p;
    s.witness = primitiveRoot(p);
    s.provider = m.address;
    s.public_key = m.pair.public_key;
    s.signature = sign(m, primeSigningPayload(s));
    return s;
}

GovernanceTx governanceTx(const Key& signer, const GovernanceProposal& p, GovernanceTxKind kind) {
    GovernanceTx t;
    t.kind = kind;
    t.proposal = p;
    t.signer_key = signer.pair.public_key;
    t.signature = sign(signer, governanceSigningPayload(t));
    return t;
}

std::string tempDir() {
    std::string tmpl = (fs::temp_directory_path() / "pcnext-consensus-XXXXXX").string();
    require(mkdtemp(tmpl.data()) != nullptr, "temp dir");
    return tmpl;
}

// ---------------------------------------------------------------- simulated network

constexpr std::uint32_t kMinerId = 1000000;

struct Options {
    std::size_t validators{3};
    bool mldsa{true};            // consensus signatures; engines always verify ML-DSA submissions
    bool persistent{false};      // engines on a Store, so nodes can crash and restart
    std::uint64_t seed{1};
    std::uint64_t chunk{1000};   // largest range a miner submits (small values give several ranges per block)
    Policy policy{};
    Retention retention{};
};

struct Harness {
    struct Event {
        std::uint64_t at, seq;
        std::uint32_t to, from;
        Message message;
        bool operator>(const Event& o) const { return at != o.at ? at > o.at : seq > o.seq; }
    };
    using Tamper = std::function<void(Harness&, const Outgoing&, std::vector<std::pair<std::uint32_t, Message>>&)>;

    Options opt;
    Genesis genesis;
    Crypto crypt;
    std::vector<Validator> vals;           // genesis validators, then any added later
    std::vector<std::optional<Node>> nodes;  // node i uses vals[i]
    std::vector<std::optional<Durable>> saved;
    std::vector<std::string> dirs;
    std::string root;
    Key miner = newKey();
    std::mt19937_64 rng;
    double drop{0};
    std::uint64_t dmin{5}, dmax{50};
    std::function<bool(std::uint32_t, std::uint32_t, const Message&)> filter;
    std::map<std::uint32_t, Tamper> tamper;
    std::priority_queue<Event, std::vector<Event>, std::greater<>> queue;
    std::uint64_t seq{0}, now{0}, next_mine{0}, period{100}, events{0};
    std::vector<std::pair<Message, int>> gossip;  // extra messages re-sent on miner ticks
    std::optional<Message> last_submission;
    std::pair<Hash256, std::uint64_t> last_key{};
    std::map<InstanceId, Hash256> agreed;
    std::vector<std::optional<InstanceId>> checked;
    std::size_t conflicts{0};

    explicit Harness(Options o) : opt(o), crypt(o.mldsa ? mldsa() : fastMock()), rng(o.seed) {
        for (std::size_t i = 0; i < opt.validators; ++i) {
            vals.push_back(makeValidator(opt.policy.network_id));
            genesis.validators.push_back(vals.back().record);
        }
        if (opt.persistent) root = tempDir();
        for (std::size_t i = 0; i < opt.validators; ++i) addNode(i);
    }
    ~Harness() {
        if (!root.empty()) fs::remove_all(root);
    }

    Engine freshEngine(std::size_t i) {
        if (!opt.persistent) return Engine(opt.policy, genesis);
        std::string error;
        auto e = Engine::open(dirs[i], opt.policy, genesis, error);
        require(e.has_value(), "open store: " + error);
        return std::move(*e);
    }
    Bytes secret(std::size_t i) const { return opt.mldsa ? vals[i].consensus.pair.private_key : vals[i].consensus.pair.public_key; }

    void addNode(std::size_t i) {  // vals[i] must exist
        if (nodes.size() <= i) {
            nodes.resize(i + 1);
            saved.resize(i + 1);
            checked.resize(i + 1);
            dirs.resize(i + 1);
            if (opt.persistent) dirs[i] = (fs::path(root) / ("node" + std::to_string(i))).string();
        }
        nodes[i].emplace(vals[i].consensus.pair.public_key, secret(i), freshEngine(i), crypt, Timing{}, opt.retention);
        std::vector<Outgoing> out;
        nodes[i]->start(now, out);
        dispatch(static_cast<std::uint32_t>(i), out);
    }
    void crash(std::size_t i, bool keep = true) {
        if (keep) saved[i] = nodes[i]->durable();  // == state after its last event (crashes fall between events)
        nodes[i].reset();
    }
    void restart(std::size_t i) {
        nodes[i].emplace(Node::restore(vals[i].consensus.pair.public_key, secret(i), freshEngine(i), *saved[i], crypt,
                                       Timing{}, opt.retention));
        std::vector<Outgoing> out;
        nodes[i]->start(now, out);
        dispatch(static_cast<std::uint32_t>(i), out);
    }

    void send(std::uint32_t from, std::uint32_t to, const Message& m) {
        if (to >= nodes.size() || to == from) return;
        if (filter && !filter(from, to, m)) return;
        if (drop > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < drop) return;
        queue.push({now + dmin + rng() % (dmax - dmin + 1), seq++, to, from, m});
    }
    void dispatch(std::uint32_t from, const std::vector<Outgoing>& out) {
        for (const auto& o : out) {
            if (const auto t = tamper.find(from); t != tamper.end()) {
                std::vector<std::pair<std::uint32_t, Message>> sends;
                t->second(*this, o, sends);
                for (const auto& [to, m] : sends) send(from, to, m);
                continue;
            }
            if (o.to) send(from, *o.to, o.message);
            else
                for (std::uint32_t to = 0; to < nodes.size(); ++to) send(from, to, o.message);
        }
    }
    std::vector<std::uint32_t> peers(std::uint32_t self, const Outgoing& o) const {
        if (o.to) return {*o.to};
        std::vector<std::uint32_t> all;
        for (std::uint32_t i = 0; i < nodes.size(); ++i)
            if (i != self) all.push_back(i);
        return all;
    }

    std::uint32_t nodeOf(std::uint32_t validator_index) const {  // harness id of a validator-set index
        for (std::uint32_t i = 0; i < nodes.size(); ++i)
            if (nodes[i] && nodes[i]->selfIndex() == validator_index) return i;
        throw std::runtime_error("no live node with that validator index");
    }
    std::uint32_t indexOf(std::uint32_t node) const {  // validator-set index of a harness node (by key)
        const auto& set = reference()->engine().validators();
        for (std::uint32_t i = 0; i < set.size(); ++i)
            if (set[i].consensus_key == vals[node].record.consensus_key) return i;
        throw std::runtime_error("node is not an active validator");
    }

    const Node* reference() const {  // the most advanced live node
        const Node* best = nullptr;
        for (const auto& n : nodes)
            if (n && (!best || n->engine().height() > best->engine().height() ||
                      (n->engine().height() == best->engine().height() &&
                       n->engine().acceptedFrontier() > best->engine().acceptedFrontier())))
                best = &*n;
        return best;
    }

    void mine() {
        next_mine = now + period;
        const Node* ref = reference();
        if (!ref) return;
        const Engine& e = ref->engine();
        const auto accepted = e.acceptedFrontier().get_ui();
        const std::pair<Hash256, std::uint64_t> key{e.tip(), accepted};
        if (key != last_key || !last_submission) {
            last_key = key;
            const auto p = nextPrime(e.chainFrontier().get_ui());
            if (accepted + 1 == p) last_submission = makePrime(e, miner, p);
            else last_submission = makeRange(e, miner, accepted + 1, std::min(p - 1, accepted + opt.chunk));
        }
        for (std::uint32_t to = 0; to < nodes.size(); ++to) send(kMinerId, to, *last_submission);
        for (auto& [m, left] : gossip)
            if (left-- > 0)
                for (std::uint32_t to = 0; to < nodes.size(); ++to) send(kMinerId, to, m);
        std::erase_if(gossip, [](const auto& g) { return g.second <= 0; });
    }

    void checkSafety() {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i]) continue;
            const auto& d = nodes[i]->decisions();
            auto it = checked[i] ? d.upper_bound(*checked[i]) : d.begin();
            for (; it != d.end(); ++it) {
                const auto [slot, inserted] = agreed.emplace(it->first, it->second.certificate.value);
                if (!inserted && slot->second != it->second.certificate.value) ++conflicts;
                checked[i] = it->first;
            }
        }
    }

    void step() {
        ++events;
        std::optional<std::uint64_t> timer;
        for (const auto& n : nodes)
            if (n)
                if (auto d = n->nextDeadline(); d && (!timer || *d < *timer)) timer = d;
        const std::uint64_t msg = queue.empty() ? UINT64_MAX : queue.top().at;
        const std::uint64_t t = std::min({msg, timer.value_or(UINT64_MAX), next_mine});
        now = std::max(now, t);
        if (t == next_mine) {
            mine();
        } else if (timer && t == *timer) {
            for (std::uint32_t i = 0; i < nodes.size(); ++i)
                if (nodes[i] && nodes[i]->nextDeadline() && *nodes[i]->nextDeadline() <= now) {
                    std::vector<Outgoing> out;
                    nodes[i]->tick(now, out);
                    dispatch(i, out);
                }
        } else {
            Event ev = queue.top();
            queue.pop();
            if (nodes[ev.to]) {
                std::vector<Outgoing> out;
                nodes[ev.to]->receive(ev.message, ev.from, now, out);
                dispatch(ev.to, out);
            }
        }
        checkSafety();
    }

    template <typename P>
    bool runUntil(P done, std::uint64_t max_ms) {
        const std::uint64_t limit = now + max_ms;
        static const bool debug = std::getenv("PCNEXT_DEBUG") != nullptr;
        std::uint64_t report = now + 20'000;
        while (!done()) {
            if (now > limit) return false;
            step();
            if (debug && now >= report) {
                report = now + 20'000;
                std::cerr << "t=" << now / 1000 << "s events=" << events << " queue=" << queue.size();
                for (const auto& n : nodes)
                    if (n) std::cerr << " [h=" << n->engine().height() << " k=" << n->instance().k << " r=" << n->round()
                                     << " acc=" << n->engine().acceptedFrontier() << "]";
                    else std::cerr << " [down]";
                std::cerr << "\n";
            }
        }
        return true;
    }
    std::uint64_t minHeight(bool live_only = true) const {
        std::uint64_t h = UINT64_MAX;
        for (const auto& n : nodes)
            if (n) h = std::min(h, n->engine().height());
        (void)live_only;
        return h;
    }
    bool reached(std::uint64_t height) const { return minHeight() >= height; }

    // Every live node at the same instance (h, k) has the same tip and state; decisions agree.
    void requireConsistent(const std::string& what) const {
        require(conflicts == 0, what + ": no conflicting decisions");
        std::map<InstanceId, std::pair<Hash256, Hash256>> at;
        for (const auto& n : nodes) {
            if (!n) continue;
            const auto& e = n->engine();
            const auto [it, fresh] = at.emplace(n->instance(), std::make_pair(e.tip(), e.stateRoot()));
            require(fresh || it->second == std::make_pair(e.tip(), e.stateRoot()), what + ": identical engines");
        }
    }
};

// Certificates are checked against the set active at their instance. The set at height h is the
// set after sealing block h-1; tests track it as the chain advances via setAt().
struct SetTracker {
    std::map<std::uint64_t, std::pair<std::vector<Bytes>, Hash256>> at;  // height of change -> set
    void observe(const Engine& e) {
        std::vector<Bytes> keys;
        for (const auto& v : e.validators()) keys.push_back(v.consensus_key);
        const auto h = e.height() + 1;
        if (at.empty() || at.rbegin()->second.second != e.validatorSetDigest()) at[h] = {keys, e.validatorSetDigest()};
    }
    const std::pair<std::vector<Bytes>, Hash256>& setAt(std::uint64_t h) const { return std::prev(at.upper_bound(h))->second; }
};

std::size_t verifyAll(const Harness& net, std::size_t i, const SetTracker& sets) {
    std::size_t verified = 0;
    for (const auto& [id, c] : net.nodes[i]->certificates()) {
        const auto& [keys, digest] = sets.setAt(id.h);
        require(c.id == id && verifyCertificate(c, keys, digest, net.opt.policy.network_id, net.crypt),
                "certificate verifies at h=" + std::to_string(id.h) + " k=" + std::to_string(id.k));
        ++verified;
    }
    return verified;
}

bool signed_by(const Certificate& c, std::size_t index) { return c.signers[index / 8] >> (index % 8) & 1; }

double seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------- (a) n = 3, long chain
void chainTest(std::uint64_t target) {
    const auto start = std::chrono::steady_clock::now();
    Options o;
    o.chunk = 6;  // gaps longer than 6 need several range decisions
    Harness net(o);
    net.dmin = 2;
    net.dmax = 40;
    SetTracker sets;
    sets.observe(net.nodes[0]->engine());
    bool transferred = false;
    primechain::Address receiver = newKey().address;
    require(net.runUntil(
                [&] {
                    const Engine& e = net.nodes[0]->engine();
                    if (!transferred && e.height() >= 30) {  // one transfer, gossiped like submissions
                        Integer asset;
                        for (std::uint64_t p = 5; p <= e.chainFrontier().get_ui(); p = nextPrime(p))
                            if (!(e.balance(net.miner.address, p) < Amount(1000))) { asset = p; break; }
                        Transfer t;
                        t.network_id = e.policy().network_id;
                        t.sender = net.miner.address;
                        t.inputs = {{asset, Amount(1000)}};
                        t.outputs = {{asset, Amount(999), receiver}};
                        t.fee_asset = asset;
                        t.fee = Amount(1);
                        t.nonce = e.nonce(net.miner.address) + 1;
                        t.signature = sign(net.miner, transferSigningPayload(t));
                        net.gossip.push_back({t, 30});
                        transferred = true;
                    }
                    return net.reached(target);
                },
                10'000'000'000ull),
            "n=3 chain reaches height " + std::to_string(target));
    net.requireConsistent("chain");
    std::size_t instances = 0, verified = 0, received = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        verified += verifyAll(net, i, sets);
        instances = std::max(instances, net.nodes[i]->certificates().size());
    }
    const Engine& e = net.nodes[0]->engine();
    for (const auto& b : e.blocks()) received += b.transfers.size();
    require(received == 1, "the gossiped transfer was included exactly once");
    for (std::size_t i = 1; i < 3; ++i)
        require(net.nodes[i]->engine().blocks().size() == e.blocks().size() &&
                    encode(net.nodes[i]->engine().blocks().back()) == encode(e.blocks().back()),
                "same blocks");
    std::cout << "consensus chain: PASS (n=3, height " << e.height() << ", frontier " << e.chainFrontier()
              << ", decisions held " << instances << ", certificates verified " << verified << ", logical "
              << net.now / 1000 << " s, events " << net.events << ", " << seconds(start) << " s wall, " << checks
              << " checks)\n";
}

// ---------------------------------------------------------------- (b) one validator crashed permanently
struct CrashResult {
    std::uint64_t time, timeouts, late_timeouts;  // late: timeouts in the second half of the run
};

CrashResult crashRun(std::uint64_t skip_after, std::uint64_t blocks) {
    Options o;
    o.mldsa = false;
    o.policy.proposer_skip_after = skip_after;
    Harness net(o);
    require(net.runUntil([&] { return net.reached(8); }, 600'000), "healthy start");
    net.crash(2, false);
    auto timeouts = [&] { return net.nodes[0]->timeoutsFired() + net.nodes[1]->timeoutsFired(); };
    const std::uint64_t t0 = net.now, h0 = net.minHeight(), c0 = timeouts();
    require(net.runUntil([&] { return net.reached(h0 + blocks / 2); }, 100'000'000), "chain continues without node 2");
    const std::uint64_t c1 = timeouts();
    require(net.runUntil([&] { return net.reached(h0 + blocks); }, 100'000'000), "chain continues without node 2");
    net.requireConsistent("crash");
    const auto skipped = net.nodes[0]->missedTurns().count(sha3(net.vals[2].record.consensus_key));
    require(skipped == 1, "the crashed validator's missed turns are counted");
    return {net.now - t0, timeouts() - c0, timeouts() - c1};
}

void crashTest() {
    const auto skip = crashRun(3, 60);
    const auto none = crashRun(1000000, 60);
    require(skip.timeouts < none.timeouts && skip.time < none.time, "proposer skip reduces timeouts and time");
    require(skip.late_timeouts == 0, "once skipped (after M=3 missed turns), the crashed proposer costs no timeouts");
    require(none.late_timeouts > 0, "without skipping, its turns keep costing timeouts");
    std::cout << "consensus crash: PASS (60 blocks with 1 of 3 down: skip M=3 -> " << skip.timeouts << " timeouts ("
              << skip.late_timeouts << " in the last 30 blocks), " << skip.time / 1000 << " s logical; no skip -> "
              << none.timeouts << " timeouts (" << none.late_timeouts << " in the last 30), " << none.time / 1000
              << " s logical; " << checks << " checks)\n";
}

// ---------------------------------------------------------------- (c) the G2 scenario
void g2Test() {
    for (int variant = 0; variant < 3; ++variant) {
        Options o;
        o.seed = 7 + variant;
        Harness net(o);
        require(net.runUntil([&] { return net.reached(5 + variant); }, 600'000), "healthy start");
        // The target instance is two blocks ahead, so its round-0 proposal X is filtered from the start.
        const InstanceId target{net.reference()->engine().height() + 3, 0};
        net.filter = [&](std::uint32_t, std::uint32_t, const Message& m) {
            if (const auto* p = std::get_if<Proposal>(&m)) return !(p->id == target && p->round == 0);
            return true;
        };
        require(net.runUntil([&] { return net.nodes[0]->instance() == target; }, 600'000), "reach target");
        // A proposes X in round 0 (lost); B proposes in round 1; C votes nil in round 0, then crashes.
        const std::uint32_t a = net.nodeOf(net.nodes[0]->proposer(0)), b = net.nodeOf(net.nodes[0]->proposer(1));
        std::uint32_t c = 0;
        while (c == a || c == b) ++c;
        const std::uint32_t c_index = net.indexOf(c);
        bool crashed = false;
        const bool ok = net.runUntil(
            [&] {
                if (!crashed)
                    for (const auto& n : net.nodes)
                        if (n && n->instance() == target && n->round() >= 1) {
                            net.crash(c, false);
                            crashed = true;
                            break;
                        }
                return crashed && net.nodes[a]->decisions().count(target) && net.nodes[b]->decisions().count(target);
            },
            600'000);
        require(ok && crashed, "G2: A and B decide without C");
        const auto& ca = net.nodes[a]->decisions().at(target).certificate;
        require(ca.round >= 1 && !signed_by(ca, c_index) && net.conflicts == 0, "G2 decided in a later round without C");
        require(net.runUntil([&] { return net.nodes[a]->engine().height() >= target.h + 5; }, 600'000),
                "chain continues after G2");
        net.requireConsistent("g2");
    }
    std::cout << "consensus g2: PASS (3 proposer rotations: lost proposal, C crashes, A+B decide in round >= 1; "
              << checks << " checks)\n";
}

// ---------------------------------------------------------------- (d) partitions and loss
void partitionTest() {
    Options o;
    o.validators = 4;
    o.mldsa = false;
    o.persistent = true;
    o.chunk = 5;
    Harness net(o);
    net.drop = 0.2;
    net.dmax = 200;
    require(net.runUntil([&] { return net.reached(10); }, 10'000'000), "progress under 20% loss");
    // 2|2 split: no side has a quorum (q = 3), so neither may decide anything.
    std::set<std::uint32_t> left{0, 1};
    net.filter = [&](std::uint32_t f, std::uint32_t t, const Message&) {
        return f == kMinerId || left.count(f) == left.count(t);
    };
    const auto decided = net.agreed.size();
    net.runUntil([] { return false; }, 120'000);
    require(net.agreed.size() == decided, "2|2 partition decides nothing");
    net.filter = nullptr;
    const auto h1 = net.reference()->engine().height();
    require(net.runUntil([&] { return net.reached(h1 + 5); }, 10'000'000), "progress after healing 2|2");
    // 3|1 split: the majority continues; the isolated node crashes, restarts, and catches up after healing.
    left = {3};
    net.filter = [&](std::uint32_t f, std::uint32_t t, const Message&) {
        return f == kMinerId || left.count(f) == left.count(t);
    };
    const auto h2 = net.nodes[0]->engine().height();
    require(net.runUntil([&] { return net.nodes[0]->engine().height() >= h2 + 10; }, 10'000'000), "majority continues");
    require(net.nodes[3]->engine().height() <= h2 + 1, "isolated node does not advance alone");
    net.crash(3);
    net.restart(3);
    net.filter = nullptr;
    const auto h3 = net.nodes[0]->engine().height();
    require(net.runUntil([&] { return net.reached(h3 + 3); }, 10'000'000), "isolated node catches up after healing");
    net.drop = 0;
    const auto h4 = net.reference()->engine().height();
    require(net.runUntil([&] { return net.reached(h4 + 1); }, 10'000'000), "converge");
    net.requireConsistent("partition");
    std::cout << "consensus partition: PASS (n=4, 20% loss, 2|2 split decides nothing, 3|1 split continues, restart "
              << "and catch-up; height " << net.minHeight() << ", " << checks << " checks)\n";
}

// ---------------------------------------------------------------- (e) n = 4, one Byzantine equivocator
void byzantine4Test() {
    Options o;
    o.validators = 4;
    o.mldsa = true;
    Harness net(o);
    const std::uint32_t byz = 3;
    net.tamper[byz] = [byz](Harness& h, const Outgoing& o, std::vector<std::pair<std::uint32_t, Message>>& sends) {
        const auto* v = std::get_if<Vote>(&o.message);
        for (const auto to : h.peers(byz, o)) {
            if (v && to == 2) {  // node 2 gets the opposite vote, signed by the same key
                std::optional<Hash256> alt;
                if (!v->value) alt = sha3(Bytes{1, 2, 3});
                sends.emplace_back(to, h.nodes[byz]->signVote(v->id, v->round, v->phase, alt));
            } else {
                sends.emplace_back(to, o.message);
            }
        }
    };
    require(net.runUntil([&] { return net.reached(30); }, 100'000'000), "n=4 progresses with one equivocator");
    net.requireConsistent("byzantine4");
    std::size_t evidence = 0;
    const Bytes& key = net.vals[byz].record.consensus_key;
    for (std::uint32_t i = 0; i < 3; ++i)
        for (const auto& ev : net.nodes[i]->evidence()) {
            const auto& set = net.nodes[i]->engine().validators();
            require(ev.first.signer == ev.second.signer && set[ev.first.signer].consensus_key == key &&
                        ev.first.value != ev.second.value && ev.first.round == ev.second.round &&
                        ev.first.phase == ev.second.phase,
                    "evidence names the equivocator at one (h, k, r, phase)");
            ++evidence;
        }
    require(evidence > 0, "equivocation evidence recorded");
    std::cout << "consensus byzantine4: PASS (n=4, 1 equivocator, height " << net.minHeight() << ", no conflicts, "
              << evidence << " evidence records at honest nodes; " << checks << " checks)\n";
}

// ---------------------------------------------------------------- (f) n = 3 with an equivocator: a fork
void byzantine3Test() {
    Options o;
    o.validators = 3;
    Harness net(o);
    require(net.runUntil([&] { return net.reached(4); }, 600'000), "healthy start");
    const std::uint32_t byz = 2;
    net.nodes[byz]->setRelay(false);
    std::optional<InstanceId> target;
    Hash256 value_a{}, value_b{};
    // The equivocator shows proposal A (and A votes) to node 0, proposal B (and B votes) to node 1;
    // honest nodes 0 and 1 cannot hear each other meanwhile.
    net.filter = [](std::uint32_t f, std::uint32_t t, const Message&) { return !((f == 0 && t == 1) || (f == 1 && t == 0)); };
    net.tamper[byz] = [&](Harness& h, const Outgoing& out, std::vector<std::pair<std::uint32_t, Message>>& sends) {
        Message m1 = out.message;
        if (const auto* p = std::get_if<Proposal>(&out.message)) {
            const Engine& e = h.nodes[byz]->engine();
            if (!target && p->value.kind == Value::Kind::Range && p->value.range.end > e.acceptedFrontier() + 1) {
                Proposal b = *p;
                b.value.range = makeRange(e, h.miner, p->value.range.start.get_ui(), p->value.range.end.get_ui() - 1);
                h.nodes[byz]->signProposal(b);
                target = p->id;
                value_a = valueHash(p->value);
                value_b = valueHash(b.value);
                m1 = b;
            }
        } else if (const auto* v = std::get_if<Vote>(&out.message)) {
            if (target && v->id == *target && v->value == value_a)
                m1 = h.nodes[byz]->signVote(v->id, v->round, v->phase, value_b);
        }
        for (const auto to : h.peers(byz, out)) sends.emplace_back(to, to == 1 ? m1 : out.message);
    };
    require(net.runUntil(
                [&] {
                    return target && net.nodes[0]->decisions().count(*target) && net.nodes[1]->decisions().count(*target);
                },
                10'000'000),
            "both honest nodes decide the target instance");
    const auto& c0 = net.nodes[0]->decisions().at(*target).certificate;
    const auto& c1 = net.nodes[1]->decisions().at(*target).certificate;
    require(c0.value != c1.value && net.conflicts > 0, "known limitation: n=3 forks under one Byzantine validator");
    // Evidence from the two conflicting certificates: the equivocator signed both.
    const std::uint32_t bi = net.indexOf(byz);
    require(signed_by(c0, bi) && signed_by(c1, bi), "the equivocator signed both certificates");
    auto nth = [](const Certificate& c, std::size_t index) {
        std::size_t k = 0;
        for (std::size_t i = 0; i < index; ++i) k += signed_by(c, i);
        return c.signatures[k];
    };
    const Vote first{c0.id, c0.round, Phase::Precommit, c0.value, bi, nth(c0, bi)};
    const Vote second{c1.id, c1.round, Phase::Precommit, c1.value, bi, nth(c1, bi)};
    const Evidence ev{first, second};
    const auto& e = net.nodes[0]->engine();
    // Both votes were signed under the set active at the target instance (unchanged since genesis).
    const Hash256 digest = Engine(o.policy, net.genesis).validatorSetDigest();
    const Bytes& key = net.vals[byz].record.consensus_key;
    require(net.crypt.verify(key, voteSigningPayload(e.policy().network_id, digest, ev.first), ev.first.signature) &&
                net.crypt.verify(key, voteSigningPayload(e.policy().network_id, digest, ev.second), ev.second.signature),
            "evidence: two valid conflicting precommits by the equivocator");
    std::cout << "consensus byzantine3: PASS (documented fork at h=" << target->h << " k=" << target->k
              << " round " << c0.round << "/" << c1.round << "; evidence captured from the two certificates; "
              << checks << " checks)\n";
}

// ---------------------------------------------------------------- (g) governance
void governanceTest() {
    Options o;
    o.validators = 3;
    o.policy.governance_notice = 5;
    o.policy.snapshot_interval = 7;  // restarts below load snapshots that carry governance state
    o.persistent = true;
    o.retention = {1000, 8};
    Harness net(o);
    SetTracker sets;
    auto observe = [&] { sets.observe(net.reference()->engine()); };
    observe();
    // D follows the chain as an observer from genesis (in-process catch-up only reaches back W blocks).
    net.vals.push_back(makeValidator(o.policy.network_id));
    const std::size_t d = 3;
    net.addNode(d);
    auto run = [&](auto done, const std::string& what) {
        require(net.runUntil([&] { observe(); return done(); }, 100'000'000), what);
    };
    run([&] { return net.reached(5); }, "start");
    const auto& network = o.policy.network_id;
    auto proposal = [&](GovernanceKind kind, std::uint64_t sequence, std::uint64_t effective_in) {
        const Engine& e = net.reference()->engine();
        GovernanceProposal p;
        p.network_id = network;
        p.kind = kind;
        p.effective_height = e.height() + effective_in;
        p.expiry_height = e.height() + 60;
        p.sequence = sequence;
        p.validator_set_digest = e.validatorSetDigest();
        return p;
    };
    auto vote = [&](const GovernanceProposal& p, std::initializer_list<std::size_t> voters) {
        for (auto v : voters) net.gossip.push_back({governanceTx(net.vals[v].consensus, p, GovernanceTxKind::Vote), 50});
    };
    auto all = [&](auto pred) {
        for (const auto& n : net.nodes)
            if (n && !pred(n->engine())) return false;
        return true;
    };

    // 1. Lapse: one vote of three, expiring soon, never takes effect and its tally is dropped.
    auto lapse = proposal(GovernanceKind::FeeChange, 1, 0);
    lapse.new_min_fee = 99;
    lapse.expiry_height = net.reference()->engine().height() + 3;
    vote(lapse, {0});
    run([&] { return net.reached(lapse.expiry_height + 2); }, "lapse window passes");
    require(all([&](const Engine& e) { return e.minFee() == 1 && e.votesFor(proposalId(lapse)) == 0; }),
            "a proposal without quorum lapses at expiry");

    // 2. Fee change with 2 of 3 votes, effective at max(effective height, inclusion + notice).
    auto fee = proposal(GovernanceKind::FeeChange, 2, 0);
    fee.new_min_fee = 7;
    vote(fee, {0, 1});
    const auto h_fee = net.reference()->engine().height();
    run([&] { return all([](const Engine& e) { return e.minFee() == 7; }); }, "fee change takes effect");
    require(net.reference()->engine().height() >= h_fee + o.policy.governance_notice, "fee change waited for notice");
    net.requireConsistent("fee");

    // 3. Add validator D, with its ready message.
    auto add = proposal(GovernanceKind::ValidatorAdd, 3, 12);
    add.validator = net.vals[d].record;
    vote(add, {0, 2});
    net.gossip.push_back({governanceTx(net.vals[d].consensus, add, GovernanceTxKind::Ready), 50});
    run([&] { return all([](const Engine& e) { return e.validators().size() == 4; }); }, "D is added");
    const auto h_add = net.reference()->engine().height();
    require(h_add + 1 >= add.effective_height, "add waited for its effective height (the set switches when block H - 1 is sealed)");
    run([&] {
        const auto& certs = net.nodes[0]->certificates();
        if (certs.empty() || certs.rbegin()->first.h <= h_add + 1) return false;
        const auto& set = net.nodes[0]->engine().validators();
        for (std::size_t i = 0; i < set.size(); ++i)
            if (set[i].consensus_key == net.vals[d].record.consensus_key) return signed_by(certs.rbegin()->second, i);
        return false;
    }, "D signs certificates");
    net.requireConsistent("add");

    // 4. Remove D with 3 of 4 votes (q = 3 now).
    auto remove = proposal(GovernanceKind::ValidatorRemove, 4, 0);
    remove.remove_key_hash = sha3(net.vals[d].record.consensus_key);
    vote(remove, {0, 1, d});
    run([&] { return all([](const Engine& e) { return e.validators().size() == 3; }); }, "D is removed");
    const auto h_removed = net.reference()->engine().height();
    run([&] { return net.reached(h_removed + 4); }, "chain continues with three");

    // 5. Add E without a ready message: the quorum passes but E never activates.
    const auto e_val = makeValidator(network);
    auto add_e = proposal(GovernanceKind::ValidatorAdd, 5, 0);
    add_e.validator = e_val.record;
    vote(add_e, {0, 1});
    run([&] {
        return all([](const Engine& e) { return e.scheduledChanges() == 0; }) && net.minHeight() > h_removed + 24;
    }, "add without ready settles");
    require(all([](const Engine& e) { return e.validators().size() == 3; }), "an add without a ready message does not activate");
    net.requireConsistent("governance");

    // 6. Add F with a ready message but only one vote: the proposal lapses, and its ready entry is pruned
    //    from every node's governance state (and so from the state root and snapshots).
    const auto f_val = makeValidator(network);
    auto add_f = proposal(GovernanceKind::ValidatorAdd, 6, 0);
    add_f.validator = f_val.record;
    add_f.expiry_height = net.reference()->engine().height() + 6;
    require(all([](const Engine& e) { return e.readyCount() == 0; }), "no ready entries before F");
    vote(add_f, {0});
    net.gossip.push_back({governanceTx(f_val.consensus, add_f, GovernanceTxKind::Ready), 50});
    bool recorded = false;
    run([&] {
        recorded |= net.reference()->engine().readyCount() == 1;
        return recorded;
    }, "F's ready message is recorded");
    run([&] { return net.reached(add_f.expiry_height + 2); }, "F's proposal lapses");
    require(all([](const Engine& e) { return e.readyCount() == 0 && e.validators().size() == 3; }),
            "ready entry of a lapsed proposal is pruned");
    net.requireConsistent("ready pruning");

    // Certificates at the two set changes are retained beyond the window and verify against their sets.
    const auto& changes = net.nodes[0]->setChangeHeights();
    require(changes.size() == 2, "two validator-set changes recorded");
    for (auto h : changes) {
        bool kept = false;
        for (const auto& [id, c] : net.nodes[0]->certificates()) kept |= id.h == h;
        require(kept, "certificate at a set change is retained");
    }
    const auto h_end = net.reference()->engine().height();
    run([&] { return net.reached(h_end + 10); }, "move beyond the window");
    std::size_t verified = 0;
    for (std::size_t i = 0; i < 3; ++i) verified += verifyAll(net, i, sets);
    // Restart node 0 from its Store (snapshot + tail): governance state and the changed set survive.
    net.crash(0);
    net.restart(0);
    const auto h_restart = net.reference()->engine().height();
    run([&] { return net.reached(h_restart + 3); }, "progress after restart");
    net.requireConsistent("governance restart");
    require(net.nodes[0]->engine().minFee() == 7 && net.nodes[0]->engine().validators().size() == 3,
            "restored governance state");
    std::cout << "consensus governance: PASS (lapse, fee 1->7 after notice, add D with ready at h=" << h_add
              << " and D signs, remove D at h=" << h_removed << ", add without ready ignored, lapsed ready pruned; " << verified
              << " certificates verified across set changes; " << checks << " checks)\n";
}

// ---------------------------------------------------------------- (g2) the certificate binds the block header
// A Byzantine proposer (n = 4) signs prime values whose header hash differs from the block they
// seal. Honest validators recompute the header in their dry run, prevote nil, and never decide
// such a value; every decided prime value names the header the honest engines actually sealed.
void headerTest() {
    Options o;
    o.validators = 4;
    o.chunk = 7;  // several ranges per block, so prime instances are spread over all proposers
    Harness net(o);
    const std::uint32_t byz = 3;
    std::set<Hash256> forged;
    std::size_t own_checks = 0;
    net.tamper[byz] = [&](Harness& h, const Outgoing& out, std::vector<std::pair<std::uint32_t, Message>>& sends) {
        Message m = out.message;
        if (const auto* p = std::get_if<Proposal>(&out.message); p && p->value.kind == Value::Kind::Prime) {
            const Engine& e = h.nodes[byz]->engine();
            std::string error;
            require(e.checkPrimeValue(p->value.prime, p->value.contents, error), "honest prime value checks: " + error);
            Proposal bad = *p;
            bad.value.contents.header_hash[0] ^= 1;
            require(!e.checkPrimeValue(bad.value.prime, bad.value.contents, error) &&
                        error.find("header hash") != std::string::npos,
                    "a prime value with a wrong header hash is rejected by the engine");
            ++own_checks;
            h.nodes[byz]->signProposal(bad);
            forged.insert(valueHash(bad.value));
            m = bad;
        }
        for (const auto to : h.peers(byz, out)) sends.emplace_back(to, m);
    };
    require(net.runUntil([&] { return net.reached(12) && !forged.empty(); }, 100'000'000),
            "progress, with at least one forged prime proposal");
    net.requireConsistent("header");
    for (std::uint32_t i = 0; i < 3; ++i)
        for (const auto& [id, d] : net.nodes[i]->decisions())
            require(!forged.count(d.certificate.value), "a value with a forged header hash is never decided");
    // Every sealed block's header hash is the one certified in its prime value.
    std::map<std::uint64_t, Hash256> sealed;
    for (const auto& b : net.nodes[0]->engine().blocks()) sealed[b.header.height] = blockHash(b.header);
    std::size_t matched = 0;
    for (const auto& [id, d] : net.nodes[0]->decisions())
        if (d.value.kind == Value::Kind::Prime && sealed.count(id.h)) {
            require(sealed.at(id.h) == d.value.contents.header_hash, "certified header hash matches the block");
            ++matched;
        }
    require(matched > 0, "some certified headers compared");
    std::cout << "consensus header: PASS (n=4, " << forged.size() << " forged prime proposals rejected, " << own_checks
              << " engine checks, " << matched << " certified headers matched; height " << net.minHeight() << "; "
              << checks << " checks)\n";
}

// ---------------------------------------------------------------- (h) certificate retention
void retentionTest() {
    Options o;
    o.mldsa = false;
    o.retention = {10, 5};
    Harness net(o);
    require(net.runUntil([&] { return net.reached(63); }, 100'000'000), "chain to 63");
    for (std::size_t i = 0; i < 3; ++i) {
        const Node& n = *net.nodes[i];
        const auto tip = n.engine().height();
        std::set<std::uint64_t> heights, decided;
        for (const auto& [id, c] : n.certificates()) heights.insert(id.h);
        for (const auto& [id, d] : n.decisions()) decided.insert(id.h);
        for (std::uint64_t h = 3; h <= tip + 1; ++h) {
            const bool want = h > tip - 5 || h % 10 == 0;
            require(heights.count(h) == (want ? 1u : 0u) || (h == tip + 1), "retained iff checkpoint or window, h=" + std::to_string(h));
            require(decided.count(h) == (h > tip - 5 ? 1u : 0u) || h == tip + 1, "decisions kept for the window only");
        }
    }
    std::cout << "consensus retention: PASS (K=10, W=5 at height " << net.minHeight() << "; " << checks << " checks)\n";
}

// ---------------------------------------------------------------- (i) seeded random-schedule safety fuzz
void fuzzTest(std::uint64_t seeds) {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t decisions = 0, crashes = 0, partitions = 0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        std::mt19937_64 r(seed * 7919);
        Options o;
        o.validators = 3 + r() % 3;  // 3..5
        o.mldsa = false;
        o.persistent = true;
        o.seed = seed;
        o.chunk = 2 + r() % 6;
        Harness net(o);
        const std::size_t n = o.validators, f = (n - 1) / 3 == 0 ? 1 : (n - 1) / 3;  // crash budget
        net.drop = (r() % 30) / 100.0;
        net.dmax = 20 + r() % 400;
        std::set<std::uint32_t> side;
        std::vector<std::size_t> down;
        for (int phase = 0; phase < 12; ++phase) {
            switch (r() % 4) {
            case 0:  // random partition
                side.clear();
                for (std::uint32_t i = 0; i < n; ++i)
                    if (r() % 2) side.insert(i);
                net.filter = [&side](std::uint32_t a, std::uint32_t b, const Message&) {
                    return a == kMinerId || side.count(a) == side.count(b);
                };
                ++partitions;
                break;
            case 1:
                net.filter = nullptr;
                break;
            case 2:
                if (down.size() < f) {
                    const std::size_t victim = r() % n;
                    if (net.nodes[victim]) {
                        net.crash(victim);
                        down.push_back(victim);
                        ++crashes;
                    }
                }
                break;
            default:
                if (!down.empty()) {
                    net.restart(down.back());
                    down.pop_back();
                }
            }
            net.runUntil([] { return false; }, 2'000 + r() % 20'000);
            require(net.conflicts == 0, "fuzz seed " + std::to_string(seed) + ": no conflicting decisions");
        }
        // Heal: everyone back, no loss; the chain must make progress and converge.
        net.filter = nullptr;
        net.drop = 0;
        for (auto i : down) net.restart(i);
        const auto h = net.reference()->engine().height();
        require(net.runUntil([&] { return net.reached(h + 3); }, 50'000'000),
                "fuzz seed " + std::to_string(seed) + ": progress after healing");
        net.requireConsistent("fuzz seed " + std::to_string(seed));
        decisions += net.agreed.size();
    }
    std::cout << "consensus fuzz: PASS (" << seeds << " seeds, n=3..5, loss up to 29%, delays up to 420 ms, "
              << partitions << " partitions, " << crashes << " crash/restarts, " << decisions
              << " instances decided, no conflicts; " << seconds(start) << " s wall, " << checks << " checks)\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const std::uint64_t arg = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
    try {
        if (mode == "chain") chainTest(arg ? arg : 2000);
        else if (mode == "crash") crashTest();
        else if (mode == "g2") g2Test();
        else if (mode == "partition") partitionTest();
        else if (mode == "byzantine4") byzantine4Test();
        else if (mode == "byzantine3") byzantine3Test();
        else if (mode == "governance") governanceTest();
        else if (mode == "retention") retentionTest();
        else if (mode == "header") headerTest();
        else if (mode == "fuzz") fuzzTest(arg ? arg : 20);
        else {
            std::cerr << "usage: primechain-next-consensus-tests chain|crash|g2|partition|byzantine4|byzantine3|"
                         "governance|retention|header|fuzz [n]\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
