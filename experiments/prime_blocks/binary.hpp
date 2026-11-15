#pragma once

#include <algorithm>
#include <array>
#include <stdexcept>

#include "primechain/protocol/records.hpp"

namespace primechain::experiments::detail {

class Reader {
public:
    explicit Reader(const protocol::Bytes& bytes) : bytes_(bytes) {}
    std::uint64_t integer() {
        require(8);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(bytes_[position_++]) << (8 * i);
        return value;
    }
    std::size_t count(std::size_t limit) {
        const auto n = integer();
        if (n > limit) throw std::runtime_error("count exceeds experiment limit");
        return static_cast<std::size_t>(n);
    }
    protocol::Bytes blob(std::size_t limit) {
        const auto size = count(limit);
        require(size);
        protocol::Bytes result(bytes_.begin() + position_, bytes_.begin() + position_ + size);
        position_ += size;
        return result;
    }
    Address address() {
        const auto value = blob(128);
        return Address(value.begin(), value.end());
    }
    Hash256 hash() {
        require(32);
        Hash256 result{};
        std::copy_n(bytes_.begin() + position_, result.size(), result.begin());
        position_ += result.size();
        return result;
    }
    void magic(const std::array<std::uint8_t, 8>& expected) {
        require(expected.size());
        if (!std::equal(expected.begin(), expected.end(), bytes_.begin() + position_))
            throw std::runtime_error("incorrect experimental format tag");
        position_ += expected.size();
    }
    void finish() {
        if (position_ != bytes_.size()) throw std::runtime_error("trailing bytes");
    }
private:
    void require(std::size_t n) {
        if (n > bytes_.size() - position_) throw std::runtime_error("truncated data");
    }
    const protocol::Bytes& bytes_;
    std::size_t position_{0};
};

} // namespace primechain::experiments::detail
