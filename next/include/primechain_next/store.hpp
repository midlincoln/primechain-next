#pragma once
// D19 persistence: an append-only block file with a height index, plus atomic state snapshots.
//
// blocks.dat   frames: 8-byte little-endian length || canonical block || SHA3-256(block)
// blocks.idx   8-byte little-endian offsets, one per frame (derived; rebuilt when inconsistent)
// snapshot.dat "PCNXSNAP" || version || height || length || state || SHA3-256(state)
//
// Opening scans blocks.dat. An incomplete final frame (an interrupted append) is truncated
// away; a complete frame with a bad checksum, or a corrupt snapshot, refuses to open.
//
// B3.5 adds the rings file and the on-disk index of rows (see RingFile and RowIndex below):
// rings.dat, rings.idx, rows.dat, miners.dat. Both are checksummed; corruption refuses to open and
// nothing is silently recreated (D19).
#include "primechain_next/codec.hpp"
#include "primechain_next/numbers.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace primechain::next {

// The rings file: every decided ring batch, append-only, in order of open height.
// rings.dat  frames: 8-byte LE length n || 8-byte LE open height || batch (n bytes) || SHA3-256(height || batch)
// rings.idx  24-byte entries: offset || open height || first 8 bytes of SHA3("rings-idx" || offset || height)
// Opening scans rings.dat (an incomplete final frame is truncated away, a bad checksum refuses).
// The index must match the frames exactly; the only repair is the one entry an append interrupted
// between the two writes (the last frame); anything else refuses. Heights strictly increase.
class RingFile {
public:
    bool open(const std::string& directory, std::string& error);
    std::uint64_t count() const { return frames_.size(); }
    std::uint64_t height(std::uint64_t index) const { return frames_.at(index).second; }
    bool append(std::uint64_t open_height, const Bytes& batch, std::string& error);  // fsynced
    bool read(std::uint64_t index, Bytes& batch, std::string& error) const;
    std::uint64_t bytes() const { return end_; }

private:
    std::string dir_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> frames_;  // (offset, open height)
    std::uint64_t end_{0};
};

// The on-disk index of rows (ring-records section 5: it grows with rings x rows, so it lives on
// disk). A row of ring k for main-chain prime q is stored at slot j = the main block of q, in
// chunks of kRowsPerChunk slots of one ring; only a small directory (one entry per chunk) and the
// table of miner addresses stay in memory.
// rows.dat   header "PCNXROWS" || version || slots per chunk || check; then chunks:
//            "PCNXROWC" || ring || chunk number || check, then kRowsPerChunk records of 32 bytes:
//            height || miner id || shape || 7 zero bytes || check (first 8 bytes of a SHA3 over the
//            ring, the slot and the fields); an all-zero record is an empty slot.
// miners.dat frames: 4-byte LE length || address || check; miner ids count from 1.
// Opening verifies every chunk header, every record and every miner frame; a bad one refuses. An
// incomplete final chunk or miner frame (an interrupted append) is truncated away.
class RowIndex {
public:
    static constexpr std::uint64_t kRowsPerChunk = 1024;
    static constexpr std::size_t kRecordBytes = 32;
    enum Shape : std::uint8_t { Empty = 0, Inert = 1, Split = 2, Ramified = 3 };  // quadratic rows
    struct Record {
        std::uint64_t height{0};  // the main block the row counts from
        Address miner;
        std::uint8_t shape{Empty};
        bool operator==(const Record&) const = default;
    };

    RowIndex() = default;
    RowIndex(const RowIndex&) = delete;
    RowIndex& operator=(const RowIndex&) = delete;
    ~RowIndex();

    bool open(const std::string& directory, std::string& error);
    // nullopt for an empty slot. A record that fails its check throws std::runtime_error (fail-stop).
    std::optional<Record> get(std::uint64_t ring, std::uint64_t slot) const;
    bool put(std::uint64_t ring, std::uint64_t slot, const Record& record, std::string& error);  // not synced
    bool sync(std::string& error);
    void forEach(const std::function<void(std::uint64_t ring, std::uint64_t slot, const Record&)>& f) const;
    std::uint64_t bytes() const;       // rows.dat + miners.dat
    std::size_t chunks() const { return chunks_.size(); }

private:
    bool readRecord(std::uint64_t offset, std::uint64_t ring, std::uint64_t slot, std::optional<Record>& out,
                    std::string& error) const;
    std::string dir_;
    int rows_fd_{-1};
    int miners_fd_{-1};
    std::uint64_t rows_end_{0};
    std::uint64_t miners_end_{0};
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> chunks_;  // (ring, chunk) -> offset
    std::vector<Address> miners_;                                              // id - 1 -> address
    std::map<Address, std::uint64_t> miner_ids_;
};

class Store {
public:
    bool open(const std::string& directory, std::string& error);
    std::uint64_t blockCount() const { return offsets_.size(); }
    bool append(const Bytes& block, std::string& error);  // fsynced before returning
    bool read(std::uint64_t index, Bytes& block, std::string& error) const;
    bool writeSnapshot(std::uint64_t height, const Bytes& state, std::string& error);  // temp, fsync, rename
    bool loadSnapshot(std::uint64_t& height, Bytes& state, bool& found, std::string& error) const;
    bool truncatedOnOpen() const { return truncated_; }
    // B3.5: the rings file and the row index in the same directory.
    RingFile& rings() { return rings_; }
    const RingFile& rings() const { return rings_; }
    const std::shared_ptr<RowIndex>& rowIndex() const { return rows_; }
    std::uint64_t blockBytes() const { return end_; }

private:
    bool writeIndex(std::string& error) const;
    std::string dir_;
    std::vector<std::uint64_t> offsets_;
    std::uint64_t end_{0};
    bool truncated_{false};
    RingFile rings_;
    std::shared_ptr<RowIndex> rows_;
};

}  // namespace primechain::next
