#pragma once
// Number types for PrimeChain Next (design decision D14).
//   Integer : arbitrary-precision, non-negative mathematical integers (GMP).
//   Amount  : unsigned 128-bit money, checked arithmetic (overflow is an error).
//   counters: plain uint64_t, varint-encoded by the codec.
#include <gmpxx.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace primechain::next {

using Bytes = std::vector<std::uint8_t>;
using Integer = mpz_class;
__extension__ typedef unsigned __int128 u128;

class Amount {
public:
    Amount() = default;
    explicit Amount(std::uint64_t value) : value_(value) {}
    static Amount fromRaw(u128 value) { Amount a; a.value_ = value; return a; }
    static Amount max() { return fromRaw(~static_cast<u128>(0)); }

    u128 raw() const { return value_; }
    std::optional<Amount> add(const Amount& other) const;
    std::optional<Amount> sub(const Amount& other) const;
    std::optional<Amount> mul(std::uint64_t factor) const;
    std::string str() const;

    bool operator==(const Amount& other) const { return value_ == other.value_; }
    bool operator!=(const Amount& other) const { return value_ != other.value_; }
    bool operator<(const Amount& other) const { return value_ < other.value_; }

private:
    u128 value_{0};
};

// Canonical byte form of a non-negative Integer: big-endian magnitude, no leading zero byte,
// zero = empty. Throws std::invalid_argument for negative values.
Bytes integerMagnitude(const Integer& value);
Integer integerFromMagnitude(const Bytes& magnitude);

}  // namespace primechain::next
