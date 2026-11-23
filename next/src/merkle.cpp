#include "primechain_next/merkle.hpp"

#include "primechain/crypto/hash.hpp"

namespace primechain::next {
namespace {

using primechain::Hash256;

Hash256 tagged(std::uint8_t tag, const Bytes& body) {
    Bytes data;
    data.reserve(body.size() + 1);
    data.push_back(tag);
    data.insert(data.end(), body.begin(), body.end());
    return primechain::crypto::sha3_256(data);
}

Hash256 subtree(const std::vector<Bytes>& leaves, std::size_t begin, std::size_t end) {
    if (end - begin == 1) return tagged(0x00, leaves[begin]);
    std::size_t split = 1;
    while (split * 2 < end - begin) split *= 2;
    const Hash256 left = subtree(leaves, begin, begin + split);
    const Hash256 right = subtree(leaves, begin + split, end);
    Bytes body(left.begin(), left.end());
    body.insert(body.end(), right.begin(), right.end());
    return tagged(0x01, body);
}

}  // namespace

Hash256 sha3(const Bytes& data) { return primechain::crypto::sha3_256(data); }

Hash256 merkleRoot(const std::vector<Bytes>& leaves) {
    if (leaves.empty()) return tagged(0x02, {});
    return subtree(leaves, 0, leaves.size());
}

}  // namespace primechain::next
