#pragma once
// B3.4: the ring registry and the open commits. Specification: docs/ring-records.md section 2,
// docs/rings-design.md R2, R3, R5, R6. Pure state: the Engine checks signatures, deposits and
// certificates and drives it (engine_rings.cpp).
#include "primechain_next/assets.hpp"
#include "primechain_next/rings.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace primechain::next {

// A RING_COMMIT that is not settled yet: sealed, or revealed validly and waiting for every
// earlier commit to be revealed or to expire (R5). Keyed by (block, index in the block).
struct OpenCommit {
    std::uint64_t height{0};
    std::uint64_t index{0};
    Address discoverer;
    Hash256 commitment{};
    Integer deposit_asset;
    Amount deposit;
    std::uint64_t expires_after{0};  // the last block in which it may be revealed (height + window in force,
                                     // plus one for each later block under a pause of all rings, B3.6)
    bool revealed{false};
    // After a valid reveal (the deposit is already refunded):
    std::uint64_t revealed_in{0};
    Hash256 ring_id{};
    std::uint64_t certificate_type{0};
    std::uint64_t certificate_version{0};
    Hash256 certificate_hash{};
    Integer d;
    std::optional<QuadElement> unit;
    bool operator==(const OpenCommit&) const = default;
};

// The ring record (ring-records.md section 2).
struct RingEntry {
    std::uint64_t number{0};
    Hash256 ring_id{};
    std::uint64_t certificate_type{0};
    std::uint64_t certificate_version{0};
    Hash256 certificate_hash{};
    Integer d;                                              // identity (type 1)
    std::optional<QuadElement> unit;                        // the certificate's unit (D > 0), for B3.5
    Address founder;                                        // never changes
    std::vector<std::pair<std::uint64_t, Address>> payees;  // (from block, payee); the first is the founder
    RingTerms terms;                                        // reward schedule, fixed at opening (R6)
    std::uint64_t opened{0};                                // main block of opening
    std::uint64_t commit_height{0};                         // the winning commit
    std::uint64_t commit_index{0};
    Integer frontier;          // last main-chain prime with a row; 0 = none yet (advanced by ring blocks, B3.5)
    Hash256 tip{};             // hash of the latest ring block; the ring genesis hash before the first
    bool paused{false};        // R2 pause of this ring (B3.6): batches touching it are refused
    bool operator==(const RingEntry&) const = default;
};

// What happened to a commit in a block (for tests, wallets and explorers; not state, not in a
// block: rebuilt by replay).
struct RingOutcome {
    enum class Kind { Refunded, Forfeited, Expired, Opened, Lost };
    Kind kind{Kind::Refunded};
    std::uint64_t commit_height{0};
    std::uint64_t commit_index{0};
    std::uint64_t ring{0};   // Opened: the new ring number; Lost: the ring that went to an earlier commit
    std::string reason;      // Forfeited: why the reveal was invalid
};

// Proposed: the tip of a ring before its first ring block.
Hash256 ringGenesisHash(std::uint64_t number, const Hash256& ring_id, const Hash256& certificate_hash,
                        std::uint64_t opened);

class RingRegistry {
public:
    using Key = std::pair<std::uint64_t, std::uint64_t>;  // (block, index): commit order (R5)

    const std::map<Key, OpenCommit>& commits() const { return commits_; }
    OpenCommit* commit(std::uint64_t height, std::uint64_t index);
    const OpenCommit* commit(std::uint64_t height, std::uint64_t index) const;
    void addCommit(OpenCommit c);
    void eraseCommit(std::uint64_t height, std::uint64_t index);

    const std::map<std::uint64_t, RingEntry>& rings() const { return rings_; }
    const RingEntry* ring(std::uint64_t number) const;
    const RingEntry* byId(const Hash256& ring_id) const;
    std::uint64_t nextNumber() const { return rings_.size() + 1; }
    std::size_t openedSince(std::uint64_t height) const;  // rings opened in blocks >= height
    const RingEntry& open(RingEntry entry);                // number must be nextNumber()
    bool setPayee(std::uint64_t ring, std::uint64_t height, const Address& payee, std::string& error);
    // B3.5: a ring block was accepted; the ring is complete up to `frontier`, its tip is `tip`.
    void advance(std::uint64_t ring, const Integer& frontier, const Hash256& tip);
    // B3.6 (R2): pause or unpause one ring; false if there is no such ring.
    bool setPaused(std::uint64_t ring, bool paused);

    Bytes encodeEntry(const RingEntry& r) const;
    Bytes encodeCommit(const OpenCommit& c) const;
    std::vector<Bytes> leaves() const;  // ring records by number, then the open commits in order
    Hash256 digest() const;
    void encode(Writer& w) const;
    bool decode(Reader& r, std::size_t max_text, std::string& error);
    bool operator==(const RingRegistry& o) const { return commits_ == o.commits_ && rings_ == o.rings_; }

private:
    std::map<Key, OpenCommit> commits_;
    std::map<std::uint64_t, RingEntry> rings_;
    std::map<Hash256, std::uint64_t> by_id_;
};

// B3.6: the leaves of a main header's extension_root: the ring records by number, the open
// commits in order, then (if the block has one, nonzero hash) the ring batch decided for it.
std::vector<Bytes> extensionLeaves(const RingRegistry& registry, const Hash256& ring_batch_hash);

}  // namespace primechain::next
