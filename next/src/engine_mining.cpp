// B3.5: mining rings. Specification: docs/ring-records.md section 3 (RING_WORK, segments and
// their checks, no padding, ring batch, ring block, extension_root) and section 5 (the row index
// on disk); docs/rings-design.md R7 (segments), R8/R9 (row types), R10 (no padding, generator size
// as a policy tool, parameter 65), R13 (the row is the payout record); docs/protocol-parameters.md
// rule A4 (issuance per ring prime, backlog share for primes older than the ring).
//
// Everything here runs for the next main block h = height_ + 1 with the parameters in force at h.
//
// Small rules chosen here (proposed, not agreed; listed in next/README.md):
//  - At most one ring batch per main block (open height h). R1 allows one between two main
//    decisions; ranges are main decisions too, but they do not change what a batch may record.
//  - A ring batch is applied before main block h's prime; its rows count from h, and the ring
//    records it advances (frontier, tip) are in block h's extension_root.
//  - Competing segments (R7, D4): at the ring's next prime the queued work reaching furthest wins,
//    ties by arrival; its segments from there on are accepted (the "tail"), then the next position.
//  - A batch carries the signed works its groups name; a group must name a work whose miner it
//    credits and whose consecutive segments it copies from the group's start. Unnamed works are
//    refused, so the value is canonical. The rings file keeps the attribution and the work hash,
//    not the signature (D15), as blocks do for ranges.
//  - Every row of a segment is recorded, inert rows too (one row per main-chain prime, R7).
//  - A work is checked in full when submitted and queued until each of its segments is recorded.
//  - The proposer fills a batch ring by ring in number order and stops at the first group that
//    would exceed the ring batch size limit (parameter 35).
#include "primechain_next/engine.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace primechain::next {

SegmentLimits Engine::segmentLimitsAt(std::uint64_t h) const {
    return {static_cast<std::uint64_t>(parameterAt(Param::SegmentPaidRows, h)),
            static_cast<std::uint64_t>(parameterAt(Param::SegmentInertRows, h)),
            static_cast<std::uint64_t>(parameterAt(Param::GeneratorBytes, h))};
}

Integer Engine::nextChainPrime(const Integer& q) const {
    const auto it = primes_.upper_bound(q);
    return it == primes_.end() ? Integer(0) : *it;
}

std::vector<Integer> Engine::chainPrimesFrom(const Integer& start, std::size_t n) const {
    std::vector<Integer> out;
    for (auto it = primes_.lower_bound(start); it != primes_.end() && out.size() < n; ++it) out.push_back(*it);
    return out;
}

// Consecutive complete segments of `ring` from `start` (R7, R9, R10, parameter 65).
bool Engine::checkSegments(const RingEntry& ring, const std::vector<Segment>& segments, const Integer& start,
                           const SegmentLimits& limits, std::vector<Integer>& lasts, std::string& error) const {
    lasts.clear();
    Integer pos = start;
    for (const auto& s : segments) {
        if (pos == 0 || s.start != pos) { error = "segments are not consecutive"; return false; }
        const auto primes = chainPrimesFrom(pos, s.rows.size() + 1);
        std::vector<RingPrime> found;
        if (!checkSegment(ring.d, ring.unit, ring.number, s, primes, limits, found, error)) return false;
        lasts.push_back(primes[s.rows.size() - 1]);
        pos = primes.size() > s.rows.size() ? primes[s.rows.size()] : Integer(0);
    }
    return true;
}

bool Engine::checkRingWork(const RingWork& w, PendingWork& out, std::string& error) const {
    const std::uint64_t h = height_ + 1;
    if (w.version != kVersion1) { error = "unsupported ring work version"; return false; }
    if (w.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (w.miner.empty()) { error = "missing miner"; return false; }
    const RingEntry* ring = registry_.byId(w.ring_id);
    if (!ring) { error = "unknown ring"; return false; }
    if (w.segments.empty() || w.segments.size() > parameterAt(Param::SegmentsPerSignature, h)) {
        error = "segments per signature out of bounds";
        return false;
    }
    if (encode(w).size() > parameterAt(Param::MiningMessageBytes, h)) { error = "ring work exceeds the size limit"; return false; }
    for (const auto& s : w.segments)
        if (s.version != kVersion1 || s.ring_id != w.ring_id) { error = "segment is for another ring"; return false; }
    if (w.segments.front().start != w.start) { error = "first segment does not begin at the work's start"; return false; }
    if (!checkSigned(ringWorkSigningPayload(w), w.miner, w.public_key, w.signature, error)) return false;
    if (!checkSegments(*ring, w.segments, w.start, segmentLimitsAt(h), out.lasts, error)) return false;
    out.work = w;
    out.hash = ringWorkHash(w);
    out.ring = ring->number;
    out.starts.clear();
    for (const auto& s : w.segments) out.starts.push_back(s.start);
    return true;
}

bool Engine::submitRingWork(const RingWork& w, std::string& error) {
    PendingWork p;
    if (!checkRingWork(w, p, error)) return false;
    if (p.lasts.back() <= registry_.ring(p.ring)->frontier) { error = "every segment is already recorded"; return false; }
    for (const auto& q : pending_works_)
        if (q.hash == p.hash) { error = "ring work already queued"; return false; }
    p.arrival = ++arrivals_works_;
    pending_works_.push_back(std::move(p));
    return true;
}

std::optional<Engine::RingBatchValue> Engine::buildRingBatch() const {
    const std::uint64_t h = height_ + 1;
    if (last_ring_batch_ >= h || parameterAt(Param::RingsPaused, h) != 0) return std::nullopt;
    const std::size_t limit = static_cast<std::size_t>(parameterAt(Param::RingBatchBytes, h));
    const SegmentLimits limits = segmentLimitsAt(h);
    RingBatchValue v;
    v.batch.network_id = policy_.network_id;
    v.batch.open_height = h;
    v.batch.previous_main = tip_;
    std::vector<Hash256> used;
    std::set<Hash256> excluded;  // works no longer valid (for example after a vote on parameter 65)
    bool full = false;
    for (const auto& [number, ring] : registry_.rings()) {
        if (full) break;
        if (ring.paused) continue;
        RingBlock block;
        block.ring = number;
        block.previous = ring.tip;
        block.start = nextChainPrime(ring.frontier);
        Integer pos = block.start;
        while (pos != 0 && !full) {
            const PendingWork* best = nullptr;
            std::size_t best_j = 0;
            for (const auto& p : pending_works_) {
                if (p.ring != number || excluded.count(p.hash)) continue;
                for (std::size_t j = 0; j < p.starts.size(); ++j) {
                    if (p.starts[j] != pos) continue;
                    if (!best || p.lasts.back() > best->lasts.back() ||
                        (p.lasts.back() == best->lasts.back() && p.arrival < best->arrival)) {
                        best = &p;
                        best_j = j;
                    }
                }
            }
            if (!best) break;
            RingGroup g;
            g.miner = best->work.miner;
            g.work_hash = best->hash;
            g.segments.assign(best->work.segments.begin() + static_cast<std::ptrdiff_t>(best_j), best->work.segments.end());
            std::vector<Integer> lasts;
            std::string why;
            if (!checkSegments(ring, g.segments, pos, limits, lasts, why)) {
                excluded.insert(best->hash);
                continue;
            }
            RingBatch trial = v.batch;
            RingBlock with = block;
            with.groups.push_back(g);
            trial.blocks.push_back(with);
            if (encode(trial).size() > limit) {
                full = true;
                break;
            }
            block.groups.push_back(std::move(g));
            if (std::find(used.begin(), used.end(), best->hash) == used.end()) used.push_back(best->hash);
            pos = nextChainPrime(lasts.back());
        }
        if (!block.groups.empty()) v.batch.blocks.push_back(std::move(block));
    }
    if (v.batch.blocks.empty()) return std::nullopt;
    for (const auto& hash : used)
        for (const auto& p : pending_works_)
            if (p.hash == hash) { v.works.push_back(p.work); break; }
    return v;
}

bool Engine::checkRingBatch(const RingBatchValue& value, std::string& error) const {
    Engine copy = scratch();
    return copy.applyDecidedRingBatch(value, error);
}

bool Engine::applyDecidedRingBatch(const RingBatchValue& value, std::string& error) {
    const std::uint64_t h = height_ + 1;
    // The signed works behind the groups: each attached once, each named by a group.
    std::map<Hash256, PendingWork> works;
    for (const auto& w : value.works) {
        PendingWork p;
        if (!checkRingWork(w, p, error)) { error = "attached ring work: " + error; return false; }
        if (!works.emplace(p.hash, std::move(p)).second) { error = "ring work attached twice"; return false; }
    }
    std::set<Hash256> named;
    for (const auto& b : value.batch.blocks) {
        const RingEntry* ring = registry_.ring(b.ring);
        if (!ring) { error = "unknown ring"; return false; }
        Integer pos = b.start;
        for (const auto& g : b.groups) {
            const auto it = works.find(g.work_hash);
            if (it == works.end()) { error = "a group names a work that is not attached"; return false; }
            const PendingWork& p = it->second;
            if (p.work.miner != g.miner) { error = "wrong miner credit: the work was signed by another miner"; return false; }
            if (p.ring != b.ring) { error = "the work is for another ring"; return false; }
            std::size_t j = 0;
            while (j < p.starts.size() && p.starts[j] != pos) ++j;
            if (j == p.starts.size() || g.segments.empty() || j + g.segments.size() > p.starts.size()) {
                error = "the group's segments are not in the work";
                return false;
            }
            for (std::size_t k = 0; k < g.segments.size(); ++k) {
                const Segment& s = p.work.segments[j + k];
                if (s.rows != g.segments[k].rows || s.generators != g.segments[k].generators) {
                    error = "the group's segments differ from the signed work";
                    return false;
                }
            }
            named.insert(g.work_hash);
            pos = nextChainPrime(p.lasts[j + g.segments.size() - 1]);
        }
    }
    if (named.size() != works.size()) { error = "a work is attached that no group names"; return false; }
    (void)h;
    return applyRingBatch(value.batch, /*append=*/true, error);
}

bool Engine::applyRingBatch(const RingBatch& batch, bool append, std::string& error) {
    const std::uint64_t h = height_ + 1;
    if (batch.version != kVersion1) { error = "unsupported ring batch version"; return false; }
    if (batch.network_id != policy_.network_id) { error = "wrong network"; return false; }
    if (batch.open_height != h) { error = "ring batch is not for the next main block"; return false; }
    if (batch.previous_main != tip_) { error = "ring batch does not follow main block h - 1"; return false; }
    if (last_ring_batch_ >= h) { error = "one ring batch per main block"; return false; }
    if (parameterAt(Param::RingsPaused, h) != 0) { error = "all rings are paused"; return false; }
    if (batch.blocks.empty()) { error = "empty ring batch"; return false; }
    if (encode(batch).size() > parameterAt(Param::RingBatchBytes, h)) { error = "ring batch exceeds the size limit"; return false; }
    const SegmentLimits limits = segmentLimitsAt(h);
    const std::uint64_t per_signature = static_cast<std::uint64_t>(parameterAt(Param::SegmentsPerSignature, h));
    struct Planned {
        std::uint64_t ring{0};
        Integer frontier;
        Hash256 tip{};
        std::vector<Row> rows;
    };
    std::vector<Planned> plan;
    std::uint64_t previous_ring = 0;
    for (const auto& b : batch.blocks) {
        const std::string where = "ring " + std::to_string(b.ring) + ": ";
        if (b.version != kVersion1) { error = where + "unsupported ring block version"; return false; }
        if (b.ring <= previous_ring) { error = where + "ring blocks out of order (one per ring)"; return false; }
        previous_ring = b.ring;
        const RingEntry* r = registry_.ring(b.ring);
        if (!r) { error = where + "unknown ring"; return false; }
        if (r->paused) { error = where + "ring is paused"; return false; }
        if (b.previous != r->tip) { error = where + "wrong previous ring block hash"; return false; }
        const Integer next = nextChainPrime(r->frontier);
        if (next == 0 || b.start != next) { error = where + "wrong start prime"; return false; }
        if (b.groups.empty()) { error = where + "empty ring block"; return false; }
        Planned p;
        p.ring = b.ring;
        p.tip = ringBlockHash(b);
        Integer pos = b.start;
        for (const auto& g : b.groups) {
            if (g.miner.empty() || g.segments.empty()) { error = where + "empty group"; return false; }
            if (g.segments.size() > per_signature) { error = where + "group exceeds the segments per signature"; return false; }
            for (Segment s : g.segments) {
                if (pos == 0) { error = where + "segment beyond the chain frontier"; return false; }
                s.ring_id = r->ring_id;
                s.start = pos;
                const auto primes = chainPrimesFrom(pos, s.rows.size() + 1);
                std::vector<RingPrime> found;
                if (!checkSegment(r->d, r->unit, r->number, s, primes, limits, found, error)) {
                    error = where + error;
                    return false;
                }
                for (std::size_t k = 0; k < s.rows.size(); ++k) {
                    Row row;
                    row.ring = r->number;
                    row.q = primes[k];
                    row.height = h;
                    row.miner = g.miner;
                    const RowPrime over{s.rows[k] == RowType::Ramified ? 2u : 1u, {AssetId::main(primes[k])}};
                    if (s.rows[k] == RowType::Split) row.primes = {over, over};
                    else if (s.rows[k] == RowType::Ramified) row.primes = {over};
                    p.rows.push_back(std::move(row));
                }
                p.frontier = primes[s.rows.size() - 1];
                pos = primes.size() > s.rows.size() ? primes[s.rows.size()] : Integer(0);
            }
        }
        plan.push_back(std::move(p));
    }
    // Everything is checked: record the rows (R13: the row is the payout record) and advance the
    // ring records. The ledger refusing a checked row is a bug.
    for (const auto& p : plan) {
        for (const auto& row : p.rows)
            if (!ledger_.recordRow(row, error)) throw std::logic_error("ledger refused a checked row: " + error);
        registry_.advance(p.ring, p.frontier, p.tip);
    }
    last_ring_batch_ = h;
    RingBatch kept = batch;  // as written to the rings file: segments without ring ID and start
    for (auto& b : kept.blocks)
        for (auto& g : b.groups)
            for (auto& s : g.segments) { s.ring_id = {}; s.start = 0; }
    ring_batches_.push_back(std::move(kept));
    std::erase_if(pending_works_, [&](const PendingWork& w) { return w.lasts.back() <= registry_.ring(w.ring)->frontier; });
    // D19 order: the batch is durable in the rings file before its rows reach the row index.
    if (append && store_ && !store_->rings().append(h, encode(batch), error))
        throw std::runtime_error("persistence failure: " + error);
    if (persist_rows_ && !ledger_.flushRows(error)) throw std::runtime_error("persistence failure: " + error);
    return true;
}

Engine::StoreBytes Engine::storeBytes() const {
    StoreBytes out;
    if (!store_) return out;
    out.blocks = store_->blockBytes();
    out.rings = store_->rings().bytes();
    out.rows = store_->rowIndex() ? store_->rowIndex()->bytes() : 0;
    return out;
}

std::optional<Engine> Engine::replay(const Policy& policy, const Genesis& genesis, const std::vector<Block>& blocks,
                                     const std::vector<RingBatch>& batches, std::string& error) {
    Engine e(policy, genesis);
    if (blocks.size() < 2 || encode(blocks[0]) != encode(e.blocks_[0]) || encode(blocks[1]) != encode(e.blocks_[1])) {
        error = "genesis mismatch";
        return std::nullopt;
    }
    std::size_t next = 0;
    auto batchFor = [&](std::uint64_t h) {
        if (next < batches.size() && batches[next].open_height == h) {
            if (!e.applyRingBatch(batches[next], false, error)) {
                error = "ring batch at open height " + std::to_string(h) + ": " + error;
                return false;
            }
            ++next;
        }
        return true;
    };
    for (std::size_t i = 2; i < blocks.size(); ++i)
        if (!batchFor(e.height_ + 1) || !e.applyBlock(blocks[i], error)) return std::nullopt;
    if (!batchFor(e.height_ + 1)) return std::nullopt;
    if (next != batches.size()) {
        error = "ring batches do not fit the blocks";
        return std::nullopt;
    }
    return e;
}

}  // namespace primechain::next
