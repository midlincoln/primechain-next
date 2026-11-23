#pragma once
// Binary Merkle tree over SHA3-256 (RFC 6962 shape: split at the largest power of two below n,
// no leaf duplication). Leaf = H(0x00 || data), node = H(0x01 || left || right), empty = H(0x02).
#include "primechain_next/numbers.hpp"
#include "primechain/types.hpp"

#include <vector>

namespace primechain::next {

primechain::Hash256 sha3(const Bytes& data);
primechain::Hash256 merkleRoot(const std::vector<Bytes>& leaves);

}  // namespace primechain::next
