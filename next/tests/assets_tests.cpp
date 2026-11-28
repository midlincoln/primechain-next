// Tests for B3.3 assets and awards (R13): asset names, ownership history, computed awards,
// refused spending, restart and replay, each against a "pay everyone at once" ledger.
// Usage: primechain-next-assets-tests unit
//        primechain-next-assets-tests alice
//        primechain-next-assets-tests spend <experiments/nested_rings/spend_vectors.txt>
#include "primechain_next/assets.hpp"
#include "primechain_next/engine.hpp"
#include "primechain_next/merkle.hpp"
#include "primechain_next/rings.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace primechain::next;

namespace {

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

const Address kPool = "<pool>";
const std::vector<Address> kVals = {"val1", "val2", "val3"};
constexpr u128 M = 1000000;
constexpr std::uint64_t kEpoch = 100;

std::string str(u128 v) { return Amount::fromRaw(v).str(); }
std::string hex(const Hash256& h) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (auto b : h) { out += d[b >> 4]; out += d[b & 15]; }
    return out;
}
std::vector<std::uint64_t> primesTo(std::uint64_t n) {
    std::vector<bool> s(n + 1, true);
    std::vector<std::uint64_t> out;
    for (std::uint64_t i = 2; i <= n; ++i) {
        if (!s[i]) continue;
        out.push_back(i);
        for (std::uint64_t j = i * i; j <= n; j += i) s[j] = false;
    }
    return out;
}
const std::vector<std::uint64_t> PR = primesTo(8000);

// ---------------------------------------------------------------- the model of spend_test.py
// Blocks are rows, transfers, the main-chain reward, then the epoch payout (spend_test.apply).
struct RingInfo { std::int64_t d{0}; std::uint64_t lower{0}; u128 value{0}; };
struct RowSpec { std::uint64_t ring; std::uint64_t q; Address miner; std::vector<std::pair<std::uint32_t, std::int64_t>> primes; };  // (e, index below or -1)
struct Tx { Address sender; AssetId asset; std::vector<std::pair<Address, std::int64_t>> outs; AssetId fee; std::uint64_t nonce; };
struct BlockSpec {
    std::uint64_t b{0}, p{0};
    bool restart{false};
    std::vector<std::pair<Address, u128>> shares;
    std::vector<RowSpec> rows;
    std::vector<Tx> txs;
};
using Rings = std::map<std::uint64_t, RingInfo>;
using Decisions = std::vector<std::string>;

Address founder(std::uint64_t k) { return "founder" + std::to_string(k); }
RingTerms termsOf(const RingInfo& r) { RingTerms t; t.prime_value = r.value; return t; }  // 35% / 15% / half kept

AssetId belowOf(const Rings& rings, std::uint64_t ring, std::uint64_t q, std::int64_t j) {
    const auto lower = rings.at(ring).lower;
    if (lower == 0 || j < 0) return AssetId::main(Integer(static_cast<unsigned long>(q)));
    return AssetId{lower, Integer(static_cast<unsigned long>(q)), static_cast<std::uint32_t>(j)};
}

// A node: the asset ledger plus nonces (which belong to the engine's state).
struct Node {
    AssetLedger ledger;
    std::map<Address, std::uint64_t> nonce;

    explicit Node(const Rings& rings) {
        std::string error;
        ledger.setValidators(0, kVals);
        for (const auto& [k, r] : rings) require(ledger.openRing(k, termsOf(r), 0, founder(k), error), "open ring: " + error);
        require(ledger.addMainPrime(Integer(2), 1, Amount{}, error) && ledger.addMainPrime(Integer(3), 2, Amount{}, error), error);
    }
    Bytes snapshot() const {
        Writer w;
        ledger.encode(w);
        w.varint(nonce.size());
        for (const auto& [a, n] : nonce) { w.text(a); w.varint(n); }
        return w.data();
    }
    Hash256 digest() const { return sha3(snapshot()); }
    static Node restore(const Rings& rings, const Bytes& data) {
        Node n(rings);
        Reader r(data, 64);
        std::string error;
        require(n.ledger.decode(r, 256, error), "restart: " + error);
        n.nonce.clear();
        std::uint64_t count = 0;
        require(r.varint(count), "restart: nonces");
        for (std::uint64_t i = 0; i < count; ++i) {
            Address a; std::uint64_t v = 0;
            require(r.text(a, 256) && r.varint(v), "restart: nonce");
            n.nonce[a] = v;
        }
        require(r.done(), "restart: trailing bytes");
        return n;
    }
};

// spend_test.transfer: nonce, existence, amounts, then the ledger's balance check.
std::string transfer(Node& n, const Tx& t, std::uint64_t b) {
    if (t.nonce != n.nonce[t.sender] + 1) return "nonce";
    if (!n.ledger.exists(t.asset) || !n.ledger.exists(t.fee)) return "no_such_prime";
    for (const auto& [c, x] : t.outs) if (x <= 0) return "amount";
    std::vector<AssetLedger::Credit> credits;
    std::map<AssetId, Amount> need;
    for (const auto& [c, x] : t.outs) {
        credits.push_back({t.asset, c, Amount(static_cast<std::uint64_t>(x))});
        need[t.asset] = *need[t.asset].add(Amount(static_cast<std::uint64_t>(x)));
    }
    need[t.fee] = *need[t.fee].add(Amount(1));
    const std::vector<std::pair<AssetId, Amount>> debits(need.begin(), need.end());
    const SpendRefusal r = n.ledger.spend(t.sender, debits, credits, t.fee, Amount(1), b);
    if (r == SpendRefusal::NotEnough) return "not_enough";
    require(r == SpendRefusal::None, std::string("unexpected refusal: ") + refusalText(r));
    n.nonce[t.sender] = t.nonce;
    return "ok";
}

Decisions apply(Node& n, const Rings& rings, const BlockSpec& B) {
    std::string error;
    for (const auto& r : B.rows) {
        Row row;
        row.ring = r.ring; row.q = Integer(static_cast<unsigned long>(r.q)); row.height = B.b; row.miner = r.miner;
        for (const auto& [e, j] : r.primes) row.primes.push_back({e, {belowOf(rings, r.ring, r.q, j)}});
        require(n.ledger.recordRow(row, error), "row (" + std::to_string(r.ring) + ", " + std::to_string(r.q) + "): " + error);
    }
    Decisions out;
    for (const auto& t : B.txs) out.push_back(transfer(n, t, B.b));
    const Integer p(static_cast<unsigned long>(B.p));
    for (const auto& [a, x] : B.shares) n.ledger.credit(AssetId::main(p), a, Amount::fromRaw(x), B.b);
    require(n.ledger.addMainPrime(p, B.b, Amount::fromRaw(M), error), error);
    if (B.b % kEpoch == 0) n.ledger.distributePool(kVals, B.b);
    return out;
}

// The reference: every owner is credited at once (spend_test.Push), written independently of
// the module: balances in one map, the pool as the address "<pool>".
struct Push {
    const Rings& rings;
    std::map<AssetId, std::map<Address, u128>> bal;
    std::map<std::pair<std::uint64_t, std::uint64_t>, RowSpec> rows;
    std::map<Address, std::uint64_t> nonce;
    std::map<std::uint64_t, bool> mainp{{2, true}, {3, true}};
    u128 dust{0}, made{0};

    explicit Push(const Rings& r) : rings(r) {}
    u128 issued(const AssetId& x) const {
        if (x.isMain()) return x.q < 5 ? 0 : M;
        return rings.at(x.ring).value * rows.at({x.ring, x.q.get_ui()}).primes[x.i].first;
    }
    AssetId below(const AssetId& x) const {
        return belowOf(rings, x.ring, x.q.get_ui(), rows.at({x.ring, x.q.get_ui()}).primes[x.i].second);
    }
    bool exists(const AssetId& x) const {
        if (x.isMain()) return x.i == 0 && mainp.count(x.q.get_ui());
        const auto it = rows.find({x.ring, x.q.get_ui()});
        return it != rows.end() && x.i < it->second.primes.size();
    }
    u128 owned(const AssetId& x, const Address& a) const {
        const auto it = bal.find(x);
        if (it == bal.end()) return 0;
        const auto j = it->second.find(a);
        return j == it->second.end() ? 0 : j->second;
    }
    void rowsOf(const BlockSpec& B) {
        std::vector<std::tuple<AssetId, Address, u128>> todo;
        for (const auto& r : B.rows) rows[{r.ring, r.q}] = r;
        for (const auto& r : B.rows)
            for (std::uint32_t i = 0; i < r.primes.size(); ++i) {
                const AssetId x{r.ring, Integer(static_cast<unsigned long>(r.q)), i};
                const u128 I = issued(x), up = I * 35 / 100, fo = I * 15 / 100, mi = I - up - fo;
                made += I;
                std::vector<std::pair<Address, u128>> out{{r.miner, mi}, {founder(r.ring), fo}};
                std::vector<std::pair<AssetId, u128>> recs;
                u128 S = up;
                AssetId y = x;
                while (true) {
                    y = below(y);
                    if (y.isMain()) { recs.emplace_back(y, S); break; }
                    recs.emplace_back(y, S / 2);
                    S -= S / 2;
                }
                if (r.q < 5) {
                    const u128 val = recs.back().second;
                    recs.pop_back();
                    for (std::size_t j = 0; j < 3; ++j) out.emplace_back(kVals[j], val / 3 + (j < val % 3 ? 1 : 0));
                }
                for (const auto& [l, amt] : recs)
                    if (const auto it = bal.find(l); it != bal.end())
                        for (const auto& [a, v] : it->second)
                            if (a != kPool) out.emplace_back(a, amt * v / issued(l));
                u128 paid = 0;
                for (const auto& [a, v] : out) if (v) { todo.emplace_back(x, a, v); paid += v; }
                dust += I - paid;
            }
        for (const auto& [x, a, v] : todo) bal[x][a] += v;
    }
    std::string transfer(const Tx& t) {
        if (t.nonce != nonce[t.sender] + 1) return "nonce";
        if (!exists(t.asset) || !exists(t.fee)) return "no_such_prime";
        for (const auto& [c, x] : t.outs) if (x <= 0) return "amount";
        std::map<AssetId, u128> need;
        for (const auto& [c, x] : t.outs) need[t.asset] += static_cast<u128>(x);
        need[t.fee] += 1;
        for (const auto& [y, x] : need) if (owned(y, t.sender) < x) return "not_enough";
        nonce[t.sender] = t.nonce;
        for (const auto& [y, x] : need) {
            auto& holders = bal[y];
            if ((holders[t.sender] -= x) == 0) holders.erase(t.sender);
        }
        for (const auto& [c, x] : t.outs) bal[t.asset][c] += static_cast<u128>(x);
        bal[t.fee][kPool] += 1;
        return "ok";
    }
    Decisions apply(const BlockSpec& B) {
        rowsOf(B);
        Decisions out;
        for (const auto& t : B.txs) out.push_back(transfer(t));
        const AssetId p = AssetId::main(Integer(static_cast<unsigned long>(B.p)));
        for (const auto& [a, x] : B.shares) if (x) bal[p][a] += x;
        mainp[B.p] = true;
        made += M;
        if (B.b % kEpoch == 0)
            for (auto& [x, holders] : bal) {
                const auto it = holders.find(kPool);
                if (it == holders.end()) continue;
                const u128 v = it->second;
                holders.erase(it);
                for (std::size_t j = 0; j < 3; ++j)
                    if (const u128 part = v / 3 + (j < v % 3 ? 1 : 0)) holders[kVals[j]] += part;
            }
        return out;
    }
    // Sorted lines "ring q i address amount" of every nonzero balance, as export_spend_vectors.py.
    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        for (const auto& [x, holders] : bal)
            for (const auto& [a, v] : holders)
                if (v) out.push_back(std::to_string(x.ring) + " " + x.q.get_str() + " " + std::to_string(x.i) + " " + a + " " + str(v) + "\n");
        return out;
    }
};

std::vector<std::string> ledgerLines(const AssetLedger& l) {
    std::vector<std::pair<std::pair<AssetId, Address>, u128>> all;
    for (const auto& x : l.assets()) {
        for (const auto& [a, v] : l.holders(x)) all.push_back({{x, a}, v.raw()});
        if (const Amount v = l.pool(x); v != Amount{}) all.push_back({{x, kPool}, v.raw()});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        return a.first.first < b.first.first || (a.first.first == b.first.first && a.first.second < b.first.second);
    });
    std::vector<std::string> out;
    for (const auto& [k, v] : all)
        out.push_back(std::to_string(k.first.ring) + " " + k.first.q.get_str() + " " + std::to_string(k.first.i) + " " + k.second + " " + str(v) + "\n");
    return out;
}
std::string linesDigest(const std::vector<std::string>& lines) {
    std::string all;
    for (const auto& l : lines) all += l;
    return hex(sha3(Bytes(all.begin(), all.end())));
}

// Nodes A (never restarts), B (restarts from its snapshot when asked), C (replays the log at the
// end) and the reference must decide every transfer alike; A and B must have the same digest
// after every block (spend_test.Net).
struct RunResult { std::vector<Decisions> decisions; std::vector<std::string> lines; u128 dust{0}, made{0}; std::size_t entries{0}, written{0}; };

RunResult run(const Rings& rings, const std::vector<BlockSpec>& blocks) {
    Node A(rings), B(rings);
    Push ref(rings);
    RunResult res;
    for (const auto& blk : blocks) {
        const Decisions a = apply(A, rings, blk), b = apply(B, rings, blk), r = ref.apply(blk);
        require(a == b && b == r, "block " + std::to_string(blk.b) + ": nodes decide differently");
        if (blk.restart) B = Node::restore(rings, B.snapshot());
        require(A.digest() == B.digest(), "block " + std::to_string(blk.b) + ": state digests differ");
        res.decisions.push_back(a);
    }
    Node C(rings);
    for (const auto& blk : blocks) apply(C, rings, blk);
    require(C.digest() == A.digest(), "replay differs");
    res.lines = ledgerLines(A.ledger);
    require(res.lines == ref.lines(), "computed balances differ from the pay-at-once ledger");
    require(ledgerLines(B.ledger) == res.lines, "restarted node's balances differ");
    u128 held = 0;
    for (const auto& [x, holders] : ref.bal) for (const auto& [a, v] : holders) held += v;
    require(held + ref.dust == ref.made, "held + rounding loss = issued");
    res.dust = ref.dust;
    res.made = ref.made;
    res.entries = A.ledger.historyEntries();
    for (const auto& [x, holders] : ref.bal) res.written += holders.size();
    return res;
}

// ---------------------------------------------------------------- spend: replay of spend_test.py
std::vector<std::string> words(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

void spendTest(const std::string& path) {
    std::ifstream in(path);
    require(bool(in), "cannot open " + path);
    Rings rings;
    struct Run { std::string name; std::vector<BlockSpec> blocks; std::vector<Decisions> expected; std::string final_line; };
    std::vector<Run> runs;
    std::string line;
    while (std::getline(in, line)) {
        const auto w = words(line);
        if (w.empty()) continue;
        if (w[0] == "RING") rings[std::stoull(w[1])] = {std::stoll(w[2]), std::stoull(w[3]), std::stoull(w[4])};
        else if (w[0] == "RUN") runs.push_back({w[1], {}, {}, ""});
        else if (w[0] == "BLOCK") runs.back().blocks.push_back({std::stoull(w[1]), std::stoull(w[2]), w[3] == "1", {}, {}, {}});
        else if (w[0] == "SHARE") runs.back().blocks.back().shares.emplace_back(w[1], std::stoull(w[2]));
        else if (w[0] == "ROW") {
            RowSpec r{std::stoull(w[1]), std::stoull(w[2]), w[3], {}};
            const std::size_t n = std::stoull(w[4]);
            for (std::size_t i = 0; i < n; ++i) r.primes.emplace_back(std::stoul(w[5 + 2 * i]), std::stoll(w[6 + 2 * i]));
            runs.back().blocks.back().rows.push_back(r);
        } else if (w[0] == "TX") {
            auto id = [&](std::size_t k) {
                return AssetId{std::stoull(w[k]), Integer(w[k + 1]), static_cast<std::uint32_t>(std::stoul(w[k + 2]))};
            };
            Tx t{w[1], id(2), {}, id(5), std::stoull(w[8])};
            const std::size_t m = std::stoull(w[9]);
            for (std::size_t i = 0; i < m; ++i) t.outs.emplace_back(w[10 + 2 * i], std::stoll(w[11 + 2 * i]));
            runs.back().blocks.back().txs.push_back(t);
        } else if (w[0] == "RES") {
            runs.back().expected.emplace_back(w.begin() + 1, w.end());
        } else if (w[0] == "FINAL") {
            runs.back().final_line = line;
        }
    }
    require(rings.size() == 10 && runs.size() == 8, "vectors: 10 rings and 8 runs");

    // Quadratic row shapes must be what B3.2's Kronecker symbol says (split: 2 primes, ramified: 1 with e = 2).
    std::size_t shapes = 0;
    for (const auto& r : runs)
        for (const auto& b : r.blocks)
            for (const auto& row : b.rows) {
                const auto d = rings.at(row.ring).d;
                if (d == 0) continue;
                const RowType t = rowType(Integer(static_cast<long>(d)), Integer(static_cast<unsigned long>(row.q)));
                const std::size_t n = t == RowType::Split ? 2 : t == RowType::Ramified ? 1 : 0;
                require(row.primes.size() == n, "row shape of ring " + std::to_string(row.ring) + " at " + std::to_string(row.q));
                for (const auto& [e, j] : row.primes) require(e == (t == RowType::Ramified ? 2u : 1u) && j == -1, "row index");
                ++shapes;
            }

    std::size_t accepted = 0, refused = 0, compared = 0, blocks = 0, restarts = 0;
    for (const auto& r : runs) {
        const RunResult res = run(rings, r.blocks);
        require(res.decisions == r.expected, r.name + ": decisions differ from spend_test.py");
        const auto f = words(r.final_line);
        require(f.size() == 5, r.name + ": FINAL line");
        require(std::to_string(res.lines.size()) == f[1], r.name + ": number of balances " + std::to_string(res.lines.size()) + " vs " + f[1]);
        require(str(res.dust) == f[2] && str(res.made) == f[3], r.name + ": rounding loss or issue differ");
        require(linesDigest(res.lines) == f[4], r.name + ": balances differ from spend_test.py");
        std::map<std::string, std::size_t> why;
        std::size_t acc = 0, ref = 0;
        for (const auto& d : res.decisions) for (const auto& x : d) { if (x == "ok") ++acc; else { ++ref; ++why[x]; } }
        for (const auto& b : r.blocks) restarts += b.restart;
        std::cout << "  " << r.name << ": " << acc << " accepted, " << ref << " refused (";
        bool first = true;
        for (const auto& [k, v] : why) { std::cout << (first ? "" : ", ") << k << " " << v; first = false; }
        std::cout << "), " << res.lines.size() << " balances equal, loss " << str(res.dust) << " of " << str(res.made)
                  << ", " << res.entries << " history entries (written payouts would be " << res.written << ")\n";
        accepted += acc; refused += ref; compared += res.lines.size(); blocks += r.blocks.size();
    }
    std::cout << "spend_test.py replayed: " << runs.size() << " runs, " << blocks << " blocks, " << restarts << " restarts, "
              << shapes << " quadratic row shapes checked; " << accepted << " transfers accepted and " << refused
              << " refused as in spend_test.py; " << compared << " balances equal to the last unit\n";
}

// ---------------------------------------------------------------- alice: the Alice and Ovanes cases under A4
// spend_test.scenario with one ring Q(sqrt 17) (number 1) and the ring terms the engine gives a
// ring opened at genesis (A4: v = 100,000 per ring prime), or v = 500,000 to match spend_test.py.
struct Outcome { std::map<Address, u128> after, end; std::vector<std::pair<std::uint64_t, Decisions>> blocks; u128 dust; };

RowSpec quadRow(std::int64_t d, std::uint64_t ring, std::uint64_t q, const Address& miner) {
    const RowType t = rowType(Integer(static_cast<long>(d)), Integer(static_cast<unsigned long>(q)));
    RowSpec r{ring, q, miner, {}};
    if (t == RowType::Split) r.primes = {{1, -1}, {1, -1}};
    if (t == RowType::Ramified) r.primes = {{2, -1}};
    return r;
}

Outcome scenario(u128 v, std::uint64_t when, bool fee13) {
    const Rings rings{{1, {17, 0, v}}};
    const AssetId A13 = AssetId::main(Integer(13)), A11 = AssetId::main(Integer(11)), A17 = AssetId::main(Integer(17)),
                  A19 = AssetId::main(Integer(19)), x{1, Integer(13), 0};
    // The blocks depend on balances (Alice sells all she has), so they are built while running.
    Node probe(rings);
    Push ref(rings);
    std::vector<BlockSpec> blocks;
    Outcome o;
    Address who;
    AssetId F;
    std::uint64_t n = 0;
    for (std::uint64_t b = 3; b < 503; ++b) {
        BlockSpec B{b, PR[b - 1], b % 50 == 0 || b >= 498, {{"zed", M}}, {}, {}};
        if (b == 6) B.shares = {{"Alice", 500000}, {"Bob", 500000}};
        if (b == 7) B.shares = {{"Alice", M}};
        if (b == 8) B.shares = {{"Ovanes", M}};
        if (b == 300) {
            if (!fee13) B.txs.push_back({"zed", A11, {{"Bob", 10}}, A11, 1});
            B.txs.push_back({"Bob", A13, {{"Alice", 100000}}, fee13 ? A13 : A11, 1});
        }
        if (b == 499) B.rows = {quadRow(17, 1, 2, "Dan")};
        if (b == 500) B.rows = {quadRow(17, 1, 13, "Dan"), quadRow(17, 1, 17, "Dan"), quadRow(17, 1, 19, "Dan")};
        if (b == when) {
            const auto have = static_cast<std::int64_t>(ref.owned(A13, "Alice"));
            B.txs = {{"Alice", A13, {{"Ovanes", have - (fee13 ? 1 : 0)}}, fee13 ? A13 : A17, 1}};
        }
        if (b == 500 && when == 499) B.txs = {{"Alice", x, {{"Carol", 1}}, A17, 2}};  // the seller must have nothing
        if (b == 501) {
            for (const Address a : {"Dan", "founder1", "Alice", "Bob", "Ovanes"}) {
                o.after[a] = probe.ledger.owned(x, a).raw();
                require(o.after[a] == ref.owned(x, a), "after the split: computed = paid at once for " + a);
            }
            o.after[kPool] = probe.ledger.pool(x).raw();
            who = o.after["Ovanes"] ? "Ovanes" : "Alice";
            const auto m = static_cast<std::int64_t>(o.after[who]);
            n = ref.nonce[who];
            F = who == "Ovanes" ? A19 : A17;
            const auto all = static_cast<std::int64_t>(probe.ledger.issued(x).raw());
            B.txs = {{who, x, {{"Carol", all}}, F, n + 1},        // all of the new prime
                     {who, x, {{"Carol", m + 1}}, F, n + 1},      // one unit too many
                     {who, x, {{"Carol", m}}, x, n + 1},          // everything, fee in the same prime
                     {who, x, {{"Carol", m}}, F, n + 1},          // everything, fee in another prime
                     {who, x, {{"Carol", m}}, F, n + 2},          // again in the same block
                     {who, x, {{"Carol", m}}, F, n + 1},          // the same transfer replayed
                     {"Carol", x, {{"Eve", m - 1}}, x, 1}};       // the receiver spends at once
        }
        if (b == 502) B.txs = {{who, x, {{"Carol", 1}}, F, n + 2}};  // again, one block later
        const Decisions d = apply(probe, rings, B);
        require(d == ref.apply(B), "probe and reference decide alike");
        if (!B.txs.empty() && b != 300) o.blocks.emplace_back(b, d);
        if (b == 300) require(d.back() == "ok", "Bob sells 100,000 of 13 to Alice");
        blocks.push_back(B);
    }
    const RunResult res = run(rings, blocks);  // A, restarted B, replayed C and the reference again
    for (const Address& a : {who, Address("Carol"), Address("Eve")}) o.end[a] = probe.ledger.owned(x, a).raw();
    o.end[kPool] = probe.ledger.pool(x).raw();
    o.dust = res.dust;
    return o;
}

void aliceTests() {
    // The terms a ring gets from the engine's parameter table (rule A4 and R11 starting values).
    const Engine engine;
    const RingTerms t = engine.ringTermsAt(1);
    require(t.prime_value == 100000 && t.upstream_bp == 3500 && t.founder_bp == 1500 && t.kept_bp == 5000,
            "ring terms from the parameter table: v = 100,000, 35% / 15%, half kept");
    const Decisions split_block = {"not_enough", "not_enough", "not_enough", "ok", "not_enough", "nonce", "ok"};
    for (const u128 v : {t.prime_value, static_cast<u128>(500000)}) {
        const u128 up = v * 35 / 100, fo = v * 15 / 100, mi = v - up - fo;
        struct Case { std::uint64_t when; bool fee13; const char* text; };
        for (const Case c : {Case{499, false, "sold one block BEFORE the split, fee in another prime"},
                             Case{499, true, "the same, 1-unit fees paid in 13"},
                             Case{500, false, "sold in the SAME block as the split"}}) {
            const Outcome o = scenario(v, c.when, c.fee13);
            // Holders of 13 at the end of block 499 receive the 35% (R13): Ovanes if Alice sold
            // before the split, Alice (the seller) if she sold in the block of the split.
            const u128 alice13 = c.fee13 ? 599999 : 600000, bob13 = c.fee13 ? 399999 : 400000;
            const u128 buyer = up * alice13 / M, bob = up * bob13 / M;
            require(o.after.at("Dan") == mi && o.after.at("founder1") == fo && o.after.at("Bob") == bob, "miner, founder, Bob");
            require(o.after.at(c.when == 499 ? "Ovanes" : "Alice") == buyer && o.after.at(c.when == 499 ? "Alice" : "Ovanes") == 0,
                    "the award follows the holder at the end of block h - 1");
            for (const auto& [b, d] : o.blocks) {
                if (b == 499) require(d == Decisions{"ok"}, "Alice sells all her 13");
                if (b == 500) require(d == (c.when == 499 ? Decisions{"not_enough"} : Decisions{"ok"}), "block 500");
                if (b == 501) require(d == split_block, "refused and accepted spending in block 501");
                if (b == 502) require(d == Decisions{"not_enough"}, "spent twice, one block later");
            }
            const Address who = c.when == 499 ? "Ovanes" : "Alice";
            require(o.end.at(who) == 0 && o.end.at("Carol") == 0 && o.end.at("Eve") == buyer - 1 && o.end.at(kPool) == 1, "end");
            std::cout << "  v = " << str(v) << ", 13 " << c.text << ": miner " << str(mi) << ", founder " << str(fo)
                      << ", Bob " << str(bob) << ", " << who << " " << str(buyer) << "; block 501 refused 4 of 7; Eve ends with "
                      << str(o.end.at("Eve")) << ", rounding loss " << str(o.dust) << "\n";
        }
    }
}

// ---------------------------------------------------------------- unit tests
Integer I(unsigned long v) { return Integer(v); }

void unitTests() {
    std::string error;
    // Asset names (R8): ordering, encoding, (0, p, 0) only on the main chain.
    {
        const AssetId a = AssetId::main(I(13)), b{1, I(13), 0}, c{1, I(13), 1}, d{2, I(5), 0};
        require(a < b && b < c && c < d && !(b < a) && a == AssetId::main(I(13)) && b != c, "asset order: ring, q, i");
        require(b.str() == "(1, 13, 0)", "asset text");
        for (const AssetId& x : {a, b, c, d}) {
            Writer w; encode(w, x);
            Reader r(w.data(), 64); AssetId y;
            require(decode(r, y) && r.done() && y == x, "asset round trip");
        }
        Writer w; w.varint(0); w.integer(I(13)); w.varint(1);
        Reader r(w.data(), 64); AssetId y;
        require(!decode(r, y), "(0, p, 1) is not an asset");
    }
    // One quadratic ring, the validators, main primes 5..19 recorded in blocks 3..8.
    auto fresh = [&]() {
        AssetLedger l;
        l.setValidators(0, {"v2", "v1", "v3"});  // sorted inside
        require(l.openRing(1, RingTerms{}, 0, "F", error), error);
        require(l.addMainPrime(I(2), 1, Amount{}, error) && l.addMainPrime(I(3), 2, Amount{}, error), error);
        std::uint64_t b = 3;
        for (unsigned long p : {5ul, 7ul, 11ul, 13ul, 17ul, 19ul}) {
            l.credit(AssetId::main(I(p)), "zed", Amount(1000000), b);
            require(l.addMainPrime(I(p), b++, Amount(1000000), error), error);
        }
        return l;  // height 8
    };
    auto quad = [](std::uint64_t ring, unsigned long q, std::uint64_t h, const Address& miner, int n) {
        Row r; r.ring = ring; r.q = I(q); r.height = h; r.miner = miner;
        for (int i = 0; i < n; ++i) r.primes.push_back({n == 1 ? 2u : 1u, {AssetId::main(I(q))}});
        return r;
    };
    const AssetId m13 = AssetId::main(I(13)), x0{1, I(13), 0}, x1{1, I(13), 1};
    auto pay = [&](AssetLedger& l, const Address& from, const AssetId& x, const Address& to, std::uint64_t v, std::uint64_t b) {
        return l.spend(from, {{x, Amount(v)}}, {{x, to, Amount(v)}}, x, Amount{}, b);
    };
    // History (R13): an entry replaces the last unless the asset paid something in between.
    {
        AssetLedger l = fresh();
        const std::size_t base = l.historyEntries();
        require(pay(l, "zed", m13, "A", 400000, 9) == SpendRefusal::None, "pay A");
        require(pay(l, "A", m13, "B", 100000, 10) == SpendRefusal::None, "A pays B");
        require(l.historyEntries() == base + 2, "no payout in between: entries replaced (one per holder)");
        require(l.ownedAt(m13, "A", 10) == Amount(300000) && l.owned(m13, "zed") == Amount(600000), "balances");
        require(l.recordRow(quad(1, 13, 11, "D", 2), error), error);
        require(pay(l, "A", m13, "B", 100000, 11) == SpendRefusal::None, "A pays B again, in the block of the split");
        require(l.ownedAt(m13, "A", 10) == Amount(300000) && l.owned(m13, "A") == Amount(200000), "the value at h - 1 is kept");
        require(l.historyEntries() == base + 4, "a payout in between: new entries for A and B");
        // The award of x0 counts A's 300,000 at the end of block 10, not the 200,000 after the sale.
        require(l.due(x0, "A") == Amount(35000 * 3 / 10) && l.due(x0, "B") == Amount(3500) && l.due(x0, "zed") == Amount(21000),
                "holders at the end of block h - 1 earn 35% pro rata");
        require(l.due(x0, "D") == Amount(50000) && l.due(x0, "F") == Amount(15000), "miner 50%, founder 15%");
        require(l.owned(x1, "A") == Amount(10500), "both conjugates pay (R8)");
        const auto h = l.holders(x0);
        u128 sum = 0; for (const auto& [a, v] : h) sum += v.raw();
        require(h.size() == 5 && sum == 100000, "holders of a ring prime: everything issued, nothing written");
        // Ring primes are spent like main primes; the award is not written, only the transfer is.
        require(pay(l, "A", x0, "C", 10500, 12) == SpendRefusal::None && l.owned(x0, "A") == Amount{} &&
                l.owned(x0, "C") == Amount(10500), "spend a computed award");
        require(pay(l, "A", x0, "C", 1, 12) == SpendRefusal::NotEnough, "spent award cannot be spent again");
    }
    // Refused rows.
    {
        AssetLedger l = fresh();
        require(!l.recordRow(quad(2, 13, 9, "D", 2), error), "unknown ring");
        require(!l.recordRow(quad(1, 23, 9, "D", 2), error), "q is not a main-chain prime");
        require(!l.recordRow(quad(1, 19, 8, "D", 2), error), "q decided in the row's own block");
        Row zero = quad(1, 13, 9, "D", 2); zero.primes[0].e = 0;
        require(!l.recordRow(zero, error), "ramification index 0");
        Row bad = quad(1, 13, 9, "D", 2); bad.primes[0].below = {AssetId{1, I(13), 0}};
        require(!l.recordRow(bad, error), "a prime below that does not exist");
        Row nobelow = quad(1, 13, 9, "D", 2); nobelow.primes[0].below.clear();
        require(!l.recordRow(nobelow, error), "no prime below");
        require(!l.recordRow(quad(1, 13, 9, "", 2), error), "no miner");
        require(l.recordRow(quad(1, 13, 9, "D", 2), error), error);
        require(!l.recordRow(quad(1, 13, 10, "E", 2), error), "one row per q per ring (R10)");
        require(pay(l, "zed", m13, "A", 1, 10) == SpendRefusal::None, "a transfer in block 10");
        require(!l.recordRow(quad(1, 11, 10, "D", 2), error), "rows of a block come before its transfers");
        require(!l.recordRow(quad(1, 11, 9, "D", 2), error), "no rows for past blocks");
        require(l.recordRow(quad(1, 11, 11, "D", 1), error), error);
        require(l.exists(x0) && l.exists(x1) && !l.exists(AssetId{1, I(13), 2}) && !l.exists(AssetId{1, I(7), 0}) &&
                l.exists(AssetId{1, I(11), 0}) && !l.exists(AssetId{1, I(11), 1}), "assets exist by their rows");
        require(l.issued(AssetId{1, I(11), 0}) == Amount(200000) && l.issued(x0) == Amount(100000), "A4: e * v");
    }
    // Refused spending.
    {
        AssetLedger l = fresh();
        require(l.recordRow(quad(1, 13, 9, "D", 2), error), error);
        const AssetId m17 = AssetId::main(I(17));
        require(l.spend("D", {{x0, Amount(50000)}}, {{x0, "C", Amount(50000)}}, AssetId{1, I(17), 0}, Amount{}, 10) ==
                SpendRefusal::UnknownAsset, "unknown fee asset");
        require(l.spend("D", {{x0, Amount(1)}}, {{AssetId{1, I(13), 5}, "C", Amount(1)}}, x0, Amount{}, 10) == SpendRefusal::UnknownAsset, "unknown asset");
        require(l.spend("D", {}, {{x0, "C", Amount{}}}, x0, Amount{}, 10) == SpendRefusal::Amount, "zero amount");
        require(l.spend("D", {{x0, Amount(5)}}, {{x0, "C", Amount(4)}}, x0, Amount{}, 10) == SpendRefusal::Amount, "debits must match");
        require(l.spend("D", {{x0, Amount(50001)}}, {{x0, "C", Amount(50000)}}, x0, Amount(1), 10) == SpendRefusal::NotEnough,
                "the fee counts in the same asset");
        require(l.spend("D", {{x0, Amount(50000)}, {m17, Amount(1)}}, {{x0, "C", Amount(50000)}}, m17, Amount(1), 10) ==
                SpendRefusal::NotEnough, "fee in an asset the sender does not hold");
        require(l.spend("zed", {{m17, Amount(1)}}, {{m17, "D", Amount(1)}}, m17, Amount{}, 10) == SpendRefusal::None, "D gets 1 of 17");
        require(l.spend("D", {{x0, Amount(50000)}, {m17, Amount(1)}}, {{x0, "C", Amount(50000)}}, m17, Amount(1), 10) ==
                SpendRefusal::None && l.pool(m17) == Amount(1), "everything, fee in another prime");
        require(l.owned(x0, "D") == Amount{} && l.owned(x0, "C") == Amount(50000), "moved");
    }
    // Genesis primes 2 and 3 (R11): the 35% goes to the validators active at h, remainder to the first.
    {
        AssetLedger l = fresh();
        require(l.recordRow(quad(1, 2, 9, "D", 1), error), error);  // ramified: one prime, e = 2
        const AssetId y{1, I(2), 0};
        require(l.due(y, "v1") == Amount(23334) && l.due(y, "v2") == Amount(23333) && l.due(y, "v3") == Amount(23333),
                "validators share 70,000 equally, remainder to the first by payout address");
        l.setValidators(10, {"w"});
        require(l.recordRow(quad(1, 3, 10, "D", 2), error), error);
        require(l.due(AssetId{1, I(3), 0}, "w") == Amount(35000) && l.due(AssetId{1, I(3), 0}, "v1") == Amount{}, "the set active at h");
        require(l.due(y, "v1") == Amount(23334), "earlier rows keep the set of their own block");
    }
    // Payee changes (R6): the payee at h gets the founder share.
    {
        AssetLedger l = fresh();
        require(l.recordRow(quad(1, 13, 9, "D", 2), error), error);
        require(l.setPayee(1, 10, "P", error), error);
        require(l.recordRow(quad(1, 17, 10, "D", 1), error), error);
        require(l.due(x0, "F") == Amount(15000) && l.due(x0, "P") == Amount{}, "a later payee does not take earlier awards");
        require(l.due(AssetId{1, I(17), 0}, "P") == Amount(30000) && l.due(AssetId{1, I(17), 0}, "F") == Amount{}, "the payee at h");
        require(!l.setPayee(1, 9, "Q", error) && !l.setPayee(2, 11, "Q", error), "payee: no past blocks, known rings only");
    }
    // The fee pool earns nothing, and the remainders are never issued.
    {
        AssetLedger l = fresh();
        require(l.spend("zed", {{m13, Amount(1001)}}, {{m13, "A", Amount(1000)}}, m13, Amount(1), 9) == SpendRefusal::None, "fee");
        require(l.recordRow(quad(1, 13, 10, "D", 2), error), error);
        const auto h = l.holders(x0);
        u128 sum = 0; for (const auto& [a, v] : h) sum += v.raw();
        require(l.due(x0, "zed") == Amount(35000ull * 998999 / 1000000) && l.due(x0, "A") == Amount(35), "pro rata, rounded down");
        require(sum == 100000 - 1, "one unit not issued: the pool's share and the rounding");
        const auto touched = l.distributePool({"v1", "v2"}, 11);
        require(touched.size() == 1 && l.pool(m13) == Amount{} && l.owned(m13, "v1") == Amount(1), "epoch payout");
    }
    // Two levels (R11, as the tower in spend_test.py): a prime of ring 2 lies over a prime of ring 1.
    // Of the upstream share the holders of the ring-1 prime keep half; the main prime gets the rest.
    // A prime over two primes below (two embeddings) passes equal parts, remainder to the first.
    {
        AssetLedger l = fresh();
        // backlog 100%: 13 is older than ring 2, and this checks the split alone (A4 backlog: next_ring_mine)
        require(l.openRing(2, RingTerms{50000, 3500, 1500, 5000, 10000}, 8, "G", error), error);
        require(l.recordRow(quad(1, 13, 9, "D", 2), error), error);
        require(pay(l, "D", x0, "H", 50000, 9) == SpendRefusal::None, "D sells its award of x0 to H");
        Row r; r.ring = 2; r.q = I(13); r.height = 10; r.miner = "E";
        r.primes = {{1, {x0}}, {1, {x0, x1}}};
        require(l.recordRow(r, error), error);
        const AssetId y0{2, I(13), 0}, y1{2, I(13), 1};
        // y0: up 17,500; x0 holders keep 8,750 (H holds 50,000 of x0's 100,000; zed 35,000; F 15,000), 13 holders 8,750.
        require(l.due(y0, "H") == Amount(4375) && l.due(y0, "F") == Amount(1312) && l.due(y0, "G") == Amount(7500) &&
                l.due(y0, "E") == Amount(25000),
                "half kept by holders of the prime below, the rest to the main prime");
        require(l.due(y0, "zed") == Amount(35000ull * 8750 / 100000 + 8750), "zed holds x0 and 13");
        const auto s = l.split(y1);
        require(s && s->below.size() == 4 && s->below[0].second == Amount(4375) && s->below[2].second == Amount(4375),
                "two embeddings: equal parts");
        require(l.recordRow(quad(1, 17, 11, "D", 1), error), error);
        Row late; late.ring = 2; late.q = I(17); late.height = 11; late.miner = "E"; late.primes = {{2, {AssetId{1, I(17), 0}}}};
        require(!l.recordRow(late, error), "a prime below must be older than h (lower rings first)");
        late.height = 12;
        require(l.recordRow(late, error), error);
    }
    // State: snapshot round trip; every truncation and corruption is refused or changes the digest.
    {
        AssetLedger l = fresh();
        require(l.recordRow(quad(1, 13, 9, "D", 2), error), error);
        require(pay(l, "D", x0, "H", 7, 9) == SpendRefusal::None, "move");
        l.creditPool(m13, Amount(3));
        require(l.setPayee(1, 9, "P", error), error);
        Writer w; l.encode(w);
        const Bytes data = w.data();
        AssetLedger back;
        { Reader r(data, 64); require(back.decode(r, 256, error) && r.done() && back == l && back.digest() == l.digest(), "round trip"); }
        std::size_t refused = 0, changed = 0;
        for (std::size_t n = 0; n < data.size(); ++n) {
            AssetLedger t;
            Bytes cut(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
            Reader r(cut, 64);
            refused += !t.decode(r, 256, error) || !r.done();
        }
        require(refused == data.size(), "every truncation refused");
        for (std::size_t n = 0; n < data.size(); ++n) {
            Bytes bad = data; bad[n] ^= 0x01;
            AssetLedger t; Reader r(bad, 64);
            if (!t.decode(r, 256, error) || !r.done()) continue;
            Writer again; t.encode(again);
            changed += again.data() != data;
        }
        std::cout << "  snapshot " << data.size() << " bytes: " << refused << " truncations refused; "
                  << changed << " single-bit flips decoded to a different state, the rest refused\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "unit";
        if (mode == "unit") unitTests();
        else if (mode == "alice") aliceTests();
        else if (mode == "spend" && argc > 2) spendTest(argv[2]);
        else throw std::runtime_error("usage: unit | alice | spend <vectors>");
        std::cout << "PASS " << mode << " (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << " (after " << checks << " checks)\n";
        return 1;
    }
}
