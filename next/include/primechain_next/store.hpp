#pragma once
// D19 persistence: an append-only block file with a height index, plus atomic state snapshots.
//
// blocks.dat   frames: 8-byte little-endian length || canonical block || SHA3-256(block)
// blocks.idx   8-byte little-endian offsets, one per frame (derived; rebuilt when inconsistent)
// snapshot.dat "PCNXSNAP" || version || height || length || state || SHA3-256(state)
//
// Opening scans blocks.dat. An incomplete final frame (an interrupted append) is truncated
// away; a complete frame with a bad checksum, or a corrupt snapshot, refuses to open.
#include "primechain_next/numbers.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace primechain::next {

class Store {
public:
    bool open(const std::string& directory, std::string& error);
    std::uint64_t blockCount() const { return offsets_.size(); }
    bool append(const Bytes& block, std::string& error);  // fsynced before returning
    bool read(std::uint64_t index, Bytes& block, std::string& error) const;
    bool writeSnapshot(std::uint64_t height, const Bytes& state, std::string& error);  // temp, fsync, rename
    bool loadSnapshot(std::uint64_t& height, Bytes& state, bool& found, std::string& error) const;
    bool truncatedOnOpen() const { return truncated_; }

private:
    bool writeIndex(std::string& error) const;
    std::string dir_;
    std::vector<std::uint64_t> offsets_;
    std::uint64_t end_{0};
    bool truncated_{false};
};

}  // namespace primechain::next
