#include "primechain_next/assets.hpp"

#include "primechain_next/merkle.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace primechain::next {
namespace {

constexpr u128 kBp = 10000;

Amount checkedAdd(const Amount& a, const Amount& b) {
    const auto s = a.add(b);
    if (!s) throw std::overflow_error("amount overflow");
    return *s;
}

void writeSigned(Writer& w, i128 v) {
    w.u8(v < 0 ? 1 : 0);
    w.amount(Amount::fromRaw(static_cast<u128>(v < 0 ? -v : v)));
}

bool readSigned(Reader& r, i128& v) {
    std::uint8_t sign = 0;
    Amount mag;
    if (!r.u8(sign) || sign > 1 || !r.amount(mag)) return false;
    if (mag.raw() >> 126) return false;          // keeps -v representable
    if (sign == 1 && mag.raw() == 0) return false;  // no "negative zero"
    v = sign ? -static_cast<i128>(mag.raw()) : static_cast<i128>(mag.raw());
    return true;
}

// Equal parts of `total` over n receivers; the remainder one unit each to the first ones (as D17).
u128 part(u128 total, std::size_t n, std::size_t j) { return total / n + (j < total % n ? 1 : 0); }

}  // namespace

// ---------------------------------------------------------------- asset names

std::string AssetId::str() const { return "(" + std::to_string(ring) + ", " + q.get_str() + ", " + std::to_string(i) + ")"; }

bool operator<(const AssetId& a, const AssetId& b) {
    if (a.ring != b.ring) return a.ring < b.ring;
    if (a.q != b.q) return a.q < b.q;
    return a.i < b.i;
}
bool operator==(const AssetId& a, const AssetId& b) { return a.ring == b.ring && a.q == b.q && a.i == b.i; }

void encode(Writer& w, const AssetId& a) {
    w.varint(a.ring);
    w.integer(a.q);
    w.varint(a.i);
}

bool decode(Reader& r, AssetId& a) {
    std::uint64_t i = 0;
    if (!r.varint(a.ring) || !r.integer(a.q) || !r.varint(i) || i > 0xffffffffu) return false;
    a.i = static_cast<std::uint32_t>(i);
    return !(a.ring == 0 && a.i != 0);  // main-chain primes are (0, p, 0)
}

const char* refusalText(SpendRefusal r) {
    switch (r) {
        case SpendRefusal::None: return "ok";
        case SpendRefusal::UnknownAsset: return "no such prime";
        case SpendRefusal::Amount: return "amount";
        case SpendRefusal::NotEnough: return "not enough";
        case SpendRefusal::Overflow: return "overflow";
    }
    return "?";
}

// ---------------------------------------------------------------- what exists

void AssetLedger::touch(std::uint64_t height) {
    if (height < height_) throw std::logic_error("ledger: block below the last one applied");
    height_ = height;
}

bool AssetLedger::addMainPrime(const Integer& p, std::uint64_t height, const Amount& issued, std::string& error) {
    if (height < height_) { error = "main prime: block below the last one applied"; return false; }
    if (!main_.emplace(p, MainPrime{height, issued}).second) { error = "main prime recorded twice"; return false; }
    touch(height);
    return true;
}

bool AssetLedger::openRing(std::uint64_t number, const RingTerms& terms, std::uint64_t height, const Address& founder,
                           std::string& error) {
    if (number == 0 || rings_.count(number)) { error = "ring number in use"; return false; }
    if (!rings_.empty() && number != rings_.rbegin()->first + 1) { error = "ring numbers follow the order of opening"; return false; }
    if (rings_.empty() && number != 1) { error = "the first ring is number 1"; return false; }
    if (terms.upstream_bp + terms.founder_bp > kBp || terms.kept_bp > kBp) { error = "ring shares exceed 100%"; return false; }
    if (founder.empty()) { error = "ring founder missing"; return false; }
    if (height < height_) { error = "ring: block below the last one applied"; return false; }
    rings_.emplace(number, RingRecord{number, terms, height, {{height, founder}}});
    touch(height);
    return true;
}

bool AssetLedger::setPayee(std::uint64_t ring, std::uint64_t height, const Address& payee, std::string& error) {
    const auto it = rings_.find(ring);
    if (it == rings_.end()) { error = "unknown ring"; return false; }
    if (payee.empty()) { error = "payee missing"; return false; }
    auto& payees = it->second.payees;
    if (height < height_ || height < payees.back().first) { error = "payee: block below the last one applied"; return false; }
    if (payees.back().first == height) payees.back().second = payee;  // last change in a block counts
    else payees.emplace_back(height, payee);
    touch(height);
    return true;
}

void AssetLedger::setValidators(std::uint64_t height, std::vector<Address> payouts) {
    std::sort(payouts.begin(), payouts.end());
    touch(height);
    if (!validators_.empty() && validators_.back().first == height) validators_.back().second = std::move(payouts);
    else if (validators_.empty() || validators_.back().second != payouts) validators_.emplace_back(height, std::move(payouts));
}

const std::vector<Address>* AssetLedger::validatorsAt(std::uint64_t height) const {
    const std::vector<Address>* out = nullptr;
    for (const auto& [from, set] : validators_) {
        if (from > height) break;
        out = &set;
    }
    return out;
}

Address AssetLedger::payeeAt(const RingRecord& r, std::uint64_t height) const {
    Address out;
    for (const auto& [from, payee] : r.payees) {
        if (from > height) break;
        out = payee;
    }
    return out;
}

const Row* AssetLedger::row(std::uint64_t ring, const Integer& q) const {
    const auto it = rows_.find({ring, q});
    return it == rows_.end() ? nullptr : &it->second;
}

const RingRecord* AssetLedger::ring(std::uint64_t number) const {
    const auto it = rings_.find(number);
    return it == rings_.end() ? nullptr : &it->second;
}

bool AssetLedger::exists(const AssetId& x) const {
    if (x.isMain()) return x.i == 0 && main_.count(x.q);
    const Row* r = row(x.ring, x.q);
    return r && x.i < r->primes.size();
}

Amount AssetLedger::issued(const AssetId& x) const {
    if (x.isMain()) {
        const auto it = main_.find(x.q);
        return it == main_.end() || x.i != 0 ? Amount{} : it->second.issued;
    }
    const Row* r = row(x.ring, x.q);
    if (!r || x.i >= r->primes.size()) return Amount{};
    const auto v = Amount::fromRaw(rings_.at(x.ring).terms.prime_value).mul(r->primes[x.i].e);  // A4: e * v
    if (!v) throw std::overflow_error("ring prime value overflow");
    return *v;
}

bool AssetLedger::recordRow(const Row& r, std::string& error) {
    const auto ring_it = rings_.find(r.ring);
    if (r.ring == 0 || ring_it == rings_.end()) { error = "row: unknown ring"; return false; }
    if (r.height < ring_it->second.opened) { error = "row: before the ring opened"; return false; }
    // Rows of block h are recorded before anything moves in h (R13: payouts apply before the
    // block's transfers), so the history is never rewritten below a payout.
    if (r.height < height_ || (r.height == height_ && moved_in_ == height_)) {
        error = "row: block already has transfers or is below the last one applied";
        return false;
    }
    const auto q_it = main_.find(r.q);
    if (q_it == main_.end() || q_it->second.height >= r.height) { error = "row: q is not a main-chain prime before h"; return false; }
    if (rows_.count({r.ring, r.q})) { error = "row: q already recorded in this ring"; return false; }
    if (r.miner.empty()) { error = "row: miner missing"; return false; }
    for (const auto& p : r.primes) {
        if (p.e == 0) { error = "row: ramification index 0"; return false; }
        if (p.below.empty()) { error = "row: a prime needs the primes below"; return false; }
        for (const auto& l : p.below) {
            if (l.q != r.q || l.ring == r.ring || !exists(l)) { error = "row: prime below does not exist"; return false; }
            if (!l.isMain() && row(l.ring, l.q)->height >= r.height) { error = "row: prime below is not older than h"; return false; }
        }
    }
    rows_.emplace(std::make_pair(r.ring, r.q), r);
    touch(r.height);
    for (std::uint32_t i = 0; i < r.primes.size(); ++i) {  // the history of every prime below is kept at h - 1
        const auto s = split(AssetId{r.ring, r.q, i});
        for (const auto& [l, amount] : s->below) last_paid_[l] = r.height;
    }
    return true;
}

// R11: of whatever reaches a ring prime, its holders receive the kept share (rounded down) and the
// rest goes in equal parts to the primes directly below; a main-chain prime keeps all of it.
std::optional<AwardSplit> AssetLedger::split(const AssetId& x) const {
    if (x.isMain() || !exists(x)) return std::nullopt;
    const Row& r = *row(x.ring, x.q);
    const RingTerms& t = rings_.at(x.ring).terms;
    AwardSplit s;
    s.issued = issued(x);
    const u128 total = s.issued.raw(), up = total * t.upstream_bp / kBp, fo = total * t.founder_bp / kBp;
    s.founder = Amount::fromRaw(fo);
    s.miner = Amount::fromRaw(total - up - fo);
    // Depth-first over the primes below (a DAG; the same prime may be reached more than once).
    struct Item { const std::vector<AssetId>* below; u128 amount; };
    std::vector<Item> todo{{&r.primes[x.i].below, up}};
    while (!todo.empty()) {
        const Item it = todo.back();
        todo.pop_back();
        const std::size_t n = it.below->size();
        for (std::size_t j = 0; j < n; ++j) {
            const AssetId& l = (*it.below)[j];
            const u128 reach = part(it.amount, n, j);
            if (l.isMain()) {
                if (issued(l) == Amount{}) s.validators = checkedAdd(s.validators, Amount::fromRaw(reach));  // 2 and 3
                else s.below.emplace_back(l, Amount::fromRaw(reach));
                continue;
            }
            const u128 kept = reach * t.kept_bp / kBp;
            s.below.emplace_back(l, Amount::fromRaw(kept));
            const Row& lower = *row(l.ring, l.q);
            todo.push_back({&lower.primes[l.i].below, reach - kept});
        }
    }
    return s;
}

// ---------------------------------------------------------------- balances

i128 AssetLedger::netAt(const AssetId& x, const Address& a, std::optional<std::uint64_t> t) const {
    const auto it = history_.find(x);
    if (it == history_.end()) return 0;
    const auto h = it->second.find(a);
    if (h == it->second.end() || h->second.empty()) return 0;
    if (!t) return h->second.back().net;
    const auto& v = h->second;  // last entry with block <= t
    auto pos = std::upper_bound(v.begin(), v.end(), *t, [](std::uint64_t b, const Entry& e) { return b < e.block; });
    return pos == v.begin() ? 0 : std::prev(pos)->net;
}

Amount AssetLedger::due(const AssetId& x, const Address& a) const {
    const auto s = split(x);
    if (!s) return Amount{};
    const Row& r = *row(x.ring, x.q);
    Amount out;
    if (a == r.miner) out = checkedAdd(out, s->miner);
    if (a == payeeAt(rings_.at(x.ring), r.height)) out = checkedAdd(out, s->founder);
    for (const auto& [l, amount] : s->below) {
        const Amount held = ownedAt(l, a, r.height - 1);  // at the end of the block before h
        if (held == Amount{}) continue;
        const Amount nominal = issued(l);
        if (amount.raw() > (~static_cast<u128>(0)) / held.raw()) throw std::overflow_error("award overflow");
        out = checkedAdd(out, Amount::fromRaw(amount.raw() * held.raw() / nominal.raw()));
    }
    if (s->validators != Amount{})
        if (const auto* vals = validatorsAt(r.height); vals && !vals->empty())
            for (std::size_t j = 0; j < vals->size(); ++j)
                if ((*vals)[j] == a) out = checkedAdd(out, Amount::fromRaw(part(s->validators.raw(), vals->size(), j)));
    return out;
}

Amount AssetLedger::ownedAt(const AssetId& x, const Address& a, std::uint64_t t) const {
    i128 v = netAt(x, a, t);
    if (!x.isMain())
        if (const Row* r = row(x.ring, x.q); r && r->height <= t) v += static_cast<i128>(due(x, a).raw());
    if (v < 0) throw std::logic_error("ledger: negative balance");
    return Amount::fromRaw(static_cast<u128>(v));
}

Amount AssetLedger::owned(const AssetId& x, const Address& a) const {
    i128 v = netAt(x, a, std::nullopt);
    if (!x.isMain()) v += static_cast<i128>(due(x, a).raw());
    if (v < 0) throw std::logic_error("ledger: negative balance");
    return Amount::fromRaw(static_cast<u128>(v));
}

Amount AssetLedger::pool(const AssetId& x) const {
    const auto it = pool_.find(x);
    return it == pool_.end() ? Amount{} : it->second;
}

std::map<Address, Amount> AssetLedger::holders(const AssetId& x) const {
    std::set<Address> candidates;
    std::vector<AssetId> todo{x};
    std::set<AssetId> seen;
    while (!todo.empty()) {  // whoever can own x: its history, its row's miner, payee and validators, holders below
        const AssetId y = todo.back();
        todo.pop_back();
        if (!seen.insert(y).second) continue;
        if (const auto it = history_.find(y); it != history_.end())
            for (const auto& [a, h] : it->second) candidates.insert(a);
        if (const auto s = split(y)) {
            const Row& r = *row(y.ring, y.q);
            candidates.insert(r.miner);
            candidates.insert(payeeAt(rings_.at(y.ring), r.height));
            if (const auto* vals = validatorsAt(r.height)) candidates.insert(vals->begin(), vals->end());
            for (const auto& [l, amount] : s->below) todo.push_back(l);
        }
    }
    std::map<Address, Amount> out;
    for (const auto& a : candidates)
        if (const Amount v = owned(x, a); v != Amount{}) out.emplace(a, v);
    return out;
}

std::vector<AssetId> AssetLedger::assets() const {
    std::set<AssetId> out;
    for (const auto& [x, h] : history_) out.insert(x);
    for (const auto& [x, v] : pool_) out.insert(x);
    for (const auto& [key, r] : rows_)
        for (std::uint32_t i = 0; i < r.primes.size(); ++i) out.insert(AssetId{r.ring, r.q, i});
    return {out.begin(), out.end()};
}

// ---------------------------------------------------------------- changes

// R13 / ring-records section 4: a new entry replaces the last one unless the asset paid
// something after the last entry's block. Entries that change nothing are dropped.
void AssetLedger::move(const AssetId& x, const Address& a, i128 delta, std::uint64_t height) {
    if (delta == 0) return;
    touch(height);
    moved_in_ = height;
    auto& holders = history_[x];
    auto& h = holders[a];
    const i128 cur = h.empty() ? 0 : h.back().net;
    const auto paid = last_paid_.find(x);
    if (!h.empty() && (paid == last_paid_.end() || paid->second <= h.back().block)) {
        h.back() = {height, cur + delta};
        if (h.size() >= 2 && h[h.size() - 2].net == h.back().net) h.pop_back();
    } else {
        h.push_back({height, cur + delta});
    }
    if (h.size() == 1 && h.back().net == 0) holders.erase(a);
    if (holders.empty()) history_.erase(x);
}

SpendRefusal AssetLedger::spend(const Address& sender, const std::vector<std::pair<AssetId, Amount>>& debits,
                                const std::vector<Credit>& credits, const AssetId& fee_asset, const Amount& fee,
                                std::uint64_t height) {
    for (const auto& [x, amount] : debits) if (!exists(x)) return SpendRefusal::UnknownAsset;
    for (const auto& c : credits) if (!exists(c.asset)) return SpendRefusal::UnknownAsset;
    if (!exists(fee_asset)) return SpendRefusal::UnknownAsset;
    for (const auto& c : credits) if (c.amount == Amount{} || c.receiver.empty()) return SpendRefusal::Amount;
    // The debits must be exactly the credits plus the fee, per asset, each asset once.
    std::map<AssetId, Amount> need;
    try {
        for (const auto& c : credits) need[c.asset] = checkedAdd(need[c.asset], c.amount);
        if (fee != Amount{}) need[fee_asset] = checkedAdd(need[fee_asset], fee);
    } catch (const std::overflow_error&) {
        return SpendRefusal::Overflow;
    }
    std::map<AssetId, Amount> given;
    for (const auto& [x, amount] : debits)
        if (!given.emplace(x, amount).second) return SpendRefusal::Amount;
    if (given.size() != need.size()) return SpendRefusal::Amount;
    for (const auto& [x, amount] : need) {
        const auto it = given.find(x);
        if (it == given.end() || it->second != amount) return SpendRefusal::Amount;
    }
    for (const auto& [x, amount] : need)
        if (owned(x, sender) < amount) return SpendRefusal::NotEnough;
    touch(height);
    for (const auto& [x, amount] : need) move(x, sender, -static_cast<i128>(amount.raw()), height);
    for (const auto& c : credits) move(c.asset, c.receiver, static_cast<i128>(c.amount.raw()), height);
    if (fee != Amount{}) creditPool(fee_asset, fee);
    return SpendRefusal::None;
}

void AssetLedger::credit(const AssetId& x, const Address& a, const Amount& amount, std::uint64_t height) {
    if (amount.raw() >> 126) throw std::overflow_error("credit too large");
    move(x, a, static_cast<i128>(amount.raw()), height);
}

void AssetLedger::creditPool(const AssetId& x, const Amount& amount) {
    if (amount == Amount{}) return;
    Amount& slot = pool_[x];
    slot = checkedAdd(slot, amount);
}

std::vector<AssetId> AssetLedger::distributePool(const std::vector<Address>& payouts, std::uint64_t height) {
    std::vector<AssetId> touched;
    if (payouts.empty()) return touched;
    for (auto& [x, amount] : pool_) {
        if (amount == Amount{}) continue;
        for (std::size_t j = 0; j < payouts.size(); ++j)
            if (const u128 paid = part(amount.raw(), payouts.size(), j); paid != 0) credit(x, payouts[j], Amount::fromRaw(paid), height);
        amount = Amount{};
        touched.push_back(x);
    }
    std::erase_if(pool_, [](const auto& e) { return e.second == Amount{}; });
    return touched;
}

// ---------------------------------------------------------------- state

std::size_t AssetLedger::historyEntries() const {
    std::size_t n = 0;
    for (const auto& [x, holders] : history_)
        for (const auto& [a, h] : holders) n += h.size();
    return n;
}

Hash256 AssetLedger::assetDigest(const AssetId& x) const {
    Writer w;
    w.text("primechain-next/asset-history");
    primechain::next::encode(w, x);
    const auto it = history_.find(x);
    w.varint(it == history_.end() ? 0 : it->second.size());
    if (it != history_.end())
        for (const auto& [a, h] : it->second) {
            w.text(a);
            w.varint(h.size());
            for (const auto& e : h) {
                w.varint(e.block);
                writeSigned(w, e.net);
            }
        }
    w.amount(pool(x));
    return sha3(w.data());
}

void AssetLedger::encode(Writer& w) const {
    w.text("primechain-next/assets");
    w.varint(kVersion1);
    w.varint(height_);
    w.varint(moved_in_);
    w.varint(main_.size());
    for (const auto& [p, m] : main_) {
        w.integer(p);
        w.varint(m.height);
        w.amount(m.issued);
    }
    w.varint(rings_.size());
    for (const auto& [n, r] : rings_) {
        w.varint(n);
        w.amount(Amount::fromRaw(r.terms.prime_value));
        w.amount(Amount::fromRaw(r.terms.upstream_bp));
        w.amount(Amount::fromRaw(r.terms.founder_bp));
        w.amount(Amount::fromRaw(r.terms.kept_bp));
        w.varint(r.opened);
        w.varint(r.payees.size());
        for (const auto& [from, a] : r.payees) {
            w.varint(from);
            w.text(a);
        }
    }
    w.varint(rows_.size());
    for (const auto& [key, r] : rows_) {
        w.varint(r.ring);
        w.integer(r.q);
        w.varint(r.height);
        w.text(r.miner);
        w.varint(r.primes.size());
        for (const auto& p : r.primes) {
            w.varint(p.e);
            w.varint(p.below.size());
            for (const auto& l : p.below) primechain::next::encode(w, l);
        }
    }
    w.varint(history_.size());
    for (const auto& [x, holders] : history_) {
        primechain::next::encode(w, x);
        w.varint(holders.size());
        for (const auto& [a, h] : holders) {
            w.text(a);
            w.varint(h.size());
            for (const auto& e : h) {
                w.varint(e.block);
                writeSigned(w, e.net);
            }
        }
    }
    w.varint(last_paid_.size());
    for (const auto& [x, b] : last_paid_) {
        primechain::next::encode(w, x);
        w.varint(b);
    }
    w.varint(pool_.size());
    for (const auto& [x, v] : pool_) {
        primechain::next::encode(w, x);
        w.amount(v);
    }
    w.varint(validators_.size());
    for (const auto& [from, set] : validators_) {
        w.varint(from);
        w.varint(set.size());
        for (const auto& a : set) w.text(a);
    }
}

bool AssetLedger::decode(Reader& r, std::size_t max_text, std::string& error) {
    auto fail = [&](const std::string& what) {
        error = "assets: " + (r.error().empty() ? what : r.error());
        return false;
    };
    AssetLedger out;
    std::string tag;
    std::uint64_t version = 0, count = 0;
    constexpr std::uint64_t kMaxCount = 1ull << 32;
    if (!r.text(tag, 64) || tag != "primechain-next/assets" || !r.varint(version) || version != kVersion1)
        return fail("bad header");
    if (!r.varint(out.height_) || !r.varint(out.moved_in_) || out.moved_in_ > out.height_ || !r.varint(count) || count > kMaxCount)
        return fail("bad heights");
    for (std::uint64_t k = 0; k < count; ++k) {
        Integer p;
        MainPrime m{};
        if (!r.integer(p) || !r.varint(m.height) || !r.amount(m.issued) || !out.main_.emplace(p, m).second)
            return fail("bad main prime");
    }
    if (!r.varint(count) || count > kMaxCount) return fail("bad rings");
    for (std::uint64_t k = 0; k < count; ++k) {
        RingRecord rec;
        Amount v, up, fo, kept;
        std::uint64_t payees = 0;
        if (!r.varint(rec.number) || !r.amount(v) || !r.amount(up) || !r.amount(fo) || !r.amount(kept) ||
            !r.varint(rec.opened) || !r.varint(payees) || payees == 0 || payees > kMaxCount)
            return fail("bad ring");
        rec.terms = {v.raw(), up.raw(), fo.raw(), kept.raw()};
        if (up.raw() + fo.raw() > kBp || kept.raw() > kBp) return fail("bad ring terms");
        for (std::uint64_t j = 0; j < payees; ++j) {
            std::uint64_t from = 0;
            Address a;
            if (!r.varint(from) || !r.text(a, max_text)) return fail("bad payee");
            rec.payees.emplace_back(from, a);
        }
        if (!out.rings_.emplace(rec.number, rec).second) return fail("ring twice");
    }
    if (!r.varint(count) || count > kMaxCount) return fail("bad rows");
    for (std::uint64_t k = 0; k < count; ++k) {
        Row row;
        std::uint64_t primes = 0;
        if (!r.varint(row.ring) || !r.integer(row.q) || !r.varint(row.height) || !r.text(row.miner, max_text) ||
            !r.varint(primes) || primes > 4096)
            return fail("bad row");
        for (std::uint64_t j = 0; j < primes; ++j) {
            RowPrime p;
            std::uint64_t e = 0, below = 0;
            if (!r.varint(e) || e == 0 || e > 0xffffffffu || !r.varint(below) || below == 0 || below > 4096) return fail("bad row prime");
            p.e = static_cast<std::uint32_t>(e);
            p.below.resize(below);
            for (auto& l : p.below) if (!primechain::next::decode(r, l)) return fail("bad prime below");
            row.primes.push_back(std::move(p));
        }
        if (!out.rings_.count(row.ring) || !out.rows_.emplace(std::make_pair(row.ring, row.q), row).second)
            return fail("bad row key");
    }
    if (!r.varint(count) || count > kMaxCount) return fail("bad histories");
    for (std::uint64_t k = 0; k < count; ++k) {
        AssetId x;
        std::uint64_t holders = 0;
        if (!primechain::next::decode(r, x) || !r.varint(holders) || holders == 0 || holders > kMaxCount) return fail("bad history asset");
        auto& map = out.history_[x];
        for (std::uint64_t j = 0; j < holders; ++j) {
            Address a;
            std::uint64_t n = 0;
            if (!r.text(a, max_text) || !r.varint(n) || n == 0 || n > kMaxCount) return fail("bad history holder");
            History h(n);
            for (std::uint64_t m = 0; m < n; ++m) {
                if (!r.varint(h[m].block) || !readSigned(r, h[m].net)) return fail("bad history entry");
                if (m > 0 && h[m].block <= h[m - 1].block) return fail("history out of order");
            }
            if (!map.emplace(a, std::move(h)).second) return fail("holder twice");
        }
    }
    if (!r.varint(count) || count > kMaxCount) return fail("bad payout marks");
    for (std::uint64_t k = 0; k < count; ++k) {
        AssetId x;
        std::uint64_t b = 0;
        if (!primechain::next::decode(r, x) || !r.varint(b) || !out.last_paid_.emplace(x, b).second) return fail("bad payout mark");
    }
    if (!r.varint(count) || count > kMaxCount) return fail("bad pool");
    for (std::uint64_t k = 0; k < count; ++k) {
        AssetId x;
        Amount v;
        if (!primechain::next::decode(r, x) || !r.amount(v) || v == Amount{} || !out.pool_.emplace(x, v).second) return fail("bad pool entry");
    }
    if (!r.varint(count) || count > kMaxCount) return fail("bad validator history");
    for (std::uint64_t k = 0; k < count; ++k) {
        std::uint64_t from = 0, n = 0;
        if (!r.varint(from) || !r.varint(n) || n > kMaxCount) return fail("bad validator entry");
        std::vector<Address> set(n);
        for (auto& a : set) if (!r.text(a, max_text)) return fail("bad validator address");
        out.validators_.emplace_back(from, std::move(set));
    }
    *this = std::move(out);
    return true;
}

Hash256 AssetLedger::digest() const {
    Writer w;
    encode(w);
    return sha3(w.data());
}

bool AssetLedger::operator==(const AssetLedger& o) const {
    Writer a, b;
    encode(a);
    o.encode(b);
    return a.data() == b.data();
}

}  // namespace primechain::next
