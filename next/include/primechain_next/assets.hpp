#pragma once
// B3.3: assets and awards. Specification: docs/rings-design.md (R8, R11, R13),
// docs/ring-records.md (sections 1 and 4), docs/b3-plan.md (item 5: main-chain balances also
// keep a history, so one rule serves all assets). Reference model:
// experiments/nested_rings/spend_test.py.
//
// - An asset is (ring number, q, i); main-chain prime p is (0, p, 0).
// - Ring awards are computed from the rows, never written (R13):
//       owned(X, a, t) = due(X, a) + net transfers of X by a up to the end of block t
//       due(X, a)      = miner share + founder share + for each prime L below X:
//                        floor(amount(L) * owned(L, a, h - 1) / issued(L))   (h = the row's block)
// - The ledger keeps, per asset and address, a history (block, net). A new entry replaces the
//   last one unless the asset paid something (was below a row) after the last entry's block.
// - Units held by nobody (the fee pool) earn nothing; rounding remainders are never issued.
//
// Pure state, no signatures, no consensus: the Engine (main chain) and later the ring batches
// (B3.5) drive it. Rows live in memory here; the on-disk index comes with B3.5.
#include "primechain_next/model.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace primechain::next {

__extension__ typedef __int128 i128;

struct AssetId {
    std::uint64_t ring{0};  // 0 = main chain; ring numbers follow the order of opening
    Integer q;              // the main-chain prime below
    std::uint32_t i{0};     // 0 = smaller root (or the ramified prime), 1 = larger; 0 on the main chain
    static AssetId main(const Integer& p) { return AssetId{0, p, 0}; }
    bool isMain() const { return ring == 0; }
    std::string str() const;  // "(ring, q, i)"
};
bool operator<(const AssetId& a, const AssetId& b);
bool operator==(const AssetId& a, const AssetId& b);
inline bool operator!=(const AssetId& a, const AssetId& b) { return !(a == b); }
void encode(Writer& w, const AssetId& a);
bool decode(Reader& r, AssetId& a);

// R6, rule 4 of protocol-parameters.md: the terms of a ring are copied from the parameters when
// it opens and never change for that ring. Shares in basis points; the miner gets the rest.
struct RingTerms {
    u128 prime_value{100000};   // [17] value v of a new ring prime; a prime with index e issues e * v (A4)
    u128 upstream_bp{3500};     // [21] owners of the primes below
    u128 founder_bp{1500};      // [22] founder's payee
    u128 kept_bp{5000};         // [23] kept by holders of a ring prime on the way down (R11)
    bool operator==(const RingTerms&) const = default;
};

struct RingRecord {
    std::uint64_t number{0};
    RingTerms terms;
    std::uint64_t opened{0};                                // main block of opening
    std::vector<std::pair<std::uint64_t, Address>> payees;  // (from block, payee); the first is the founder
    bool operator==(const RingRecord&) const = default;
};

// One prime of norm q above q (R12: only f = 1 primes are assets).
struct RowPrime {
    std::uint32_t e{1};          // ramification index
    std::vector<AssetId> below;  // the primes directly below, one per embedding (R11); for a
                                 // quadratic ring just (0, q, 0)
    bool operator==(const RowPrime&) const = default;
};

// A recorded row: ring `ring`, main-chain prime q, decided in (and counting from) main block h.
// primes[i] is the asset (ring, q, i). An inert row has no primes and pays nothing (R9).
struct Row {
    std::uint64_t ring{0};
    Integer q;
    std::uint64_t height{0};
    Address miner;
    std::vector<RowPrime> primes;
    bool operator==(const Row&) const = default;
};

// The recipe of one new ring prime's issuance, computed from its row (R11). Every share is rounded
// down; the remainders and what would reach holders of nobody are never issued.
struct AwardSplit {
    Amount issued;                                  // e * v
    Amount miner, founder;
    std::vector<std::pair<AssetId, Amount>> below;  // amount reaching the holders of each prime below
    Amount validators;                              // the part reaching genesis primes 2 and 3 (D7)
};

enum class SpendRefusal { None, UnknownAsset, Amount, NotEnough, Overflow };
const char* refusalText(SpendRefusal r);

class AssetLedger {
public:
    // --- what exists
    // A main-chain prime recorded in block `height` with its nominal issue (the main reward in
    // force then). Genesis primes 2 and 3 are recorded with 0: they have no holders (D7).
    bool addMainPrime(const Integer& p, std::uint64_t height, const Amount& issued, std::string& error);
    // B3.4 will call these when a ring opens and when its payee changes (RING_PAYEE).
    bool openRing(std::uint64_t number, const RingTerms& terms, std::uint64_t height, const Address& founder,
                  std::string& error);
    bool setPayee(std::uint64_t ring, std::uint64_t height, const Address& payee, std::string& error);
    // Validators' payout addresses in force from `height` on (sorted; they receive the part of
    // rows that reaches 2 and 3, R11). Call at genesis and on every change of the set.
    void setValidators(std::uint64_t height, std::vector<Address> payouts);
    // Record a decided row in main block h (rows of a block are recorded before its transfers).
    // Checks: the ring is open, q is a main-chain prime, no row yet, every prime below exists,
    // e >= 1, h is not before the current block.
    bool recordRow(const Row& row, std::string& error);

    bool exists(const AssetId& x) const;
    const Row* row(std::uint64_t ring, const Integer& q) const;
    const RingRecord* ring(std::uint64_t number) const;
    Amount issued(const AssetId& x) const;          // nominal amount (0 if unknown)
    std::optional<AwardSplit> split(const AssetId& x) const;

    // --- balances
    // owned(X, a): now (after the last block applied). ownedAt: at the end of block t.
    Amount owned(const AssetId& x, const Address& a) const;
    Amount ownedAt(const AssetId& x, const Address& a, std::uint64_t t) const;
    Amount due(const AssetId& x, const Address& a) const;  // award from the row (0 for main primes)
    Amount pool(const AssetId& x) const;                    // validator fee pool, held by nobody
    // Every address with a nonzero balance of x, awards included (for wallets and the awards file).
    std::map<Address, Amount> holders(const AssetId& x) const;
    std::vector<AssetId> assets() const;            // every asset with a row prime or a history or a pool

    // --- changes in block `height` (never below the last block applied)
    // A transfer's balance part: `debits` (one per asset, the outputs plus the fee of that asset)
    // must be owned by the sender; then credits are paid and the fee goes to the pool.
    struct Credit { AssetId asset; Address receiver; Amount amount; };
    SpendRefusal spend(const Address& sender, const std::vector<std::pair<AssetId, Amount>>& debits,
                       const std::vector<Credit>& credits, const AssetId& fee_asset, const Amount& fee,
                       std::uint64_t height);
    void credit(const AssetId& x, const Address& a, const Amount& amount, std::uint64_t height);  // new main units
    void creditPool(const AssetId& x, const Amount& amount);
    // D17: the pool of every asset to `payouts` in equal shares, the remainder one unit each to
    // the first ones. Returns the assets touched. No payouts: the pool keeps accumulating.
    std::vector<AssetId> distributePool(const std::vector<Address>& payouts, std::uint64_t height);

    // --- state
    std::uint64_t height() const { return height_; }
    std::size_t historyEntries() const;
    Hash256 assetDigest(const AssetId& x) const;    // the histories and pool of one asset
    Hash256 digest() const;                         // everything
    void encode(Writer& w) const;
    bool decode(Reader& r, std::size_t max_text, std::string& error);
    bool operator==(const AssetLedger& o) const;

private:
    struct Entry { std::uint64_t block; i128 net; };
    struct MainPrime { std::uint64_t height; Amount issued; };
    using History = std::vector<Entry>;

    void move(const AssetId& x, const Address& a, i128 delta, std::uint64_t height);
    void touch(std::uint64_t height);
    i128 netAt(const AssetId& x, const Address& a, std::optional<std::uint64_t> t) const;
    Address payeeAt(const RingRecord& r, std::uint64_t height) const;
    const std::vector<Address>* validatorsAt(std::uint64_t height) const;

    std::uint64_t height_{0};                         // the last block applied
    std::uint64_t moved_in_{0};                       // the last block in which a balance moved
    std::map<Integer, MainPrime> main_;
    std::map<std::uint64_t, RingRecord> rings_;
    std::map<std::pair<std::uint64_t, Integer>, Row> rows_;
    std::map<AssetId, std::map<Address, History>> history_;
    std::map<AssetId, std::uint64_t> last_paid_;      // latest row block that paid holders of the asset
    std::map<AssetId, Amount> pool_;
    std::vector<std::pair<std::uint64_t, std::vector<Address>>> validators_;
};

}  // namespace primechain::next
