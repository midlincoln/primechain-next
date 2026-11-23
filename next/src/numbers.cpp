#include "primechain_next/numbers.hpp"

#include <algorithm>
#include <stdexcept>

namespace primechain::next {

std::optional<Amount> Amount::add(const Amount& other) const {
    const u128 sum = value_ + other.value_;
    if (sum < value_) return std::nullopt;
    return fromRaw(sum);
}

std::optional<Amount> Amount::sub(const Amount& other) const {
    if (other.value_ > value_) return std::nullopt;
    return fromRaw(value_ - other.value_);
}

std::optional<Amount> Amount::mul(std::uint64_t factor) const {
    if (factor != 0 && value_ > (~static_cast<u128>(0)) / factor) return std::nullopt;
    return fromRaw(value_ * factor);
}

std::string Amount::str() const {
    if (value_ == 0) return "0";
    std::string out;
    for (u128 v = value_; v != 0; v /= 10) out.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
    std::reverse(out.begin(), out.end());
    return out;
}

Bytes integerMagnitude(const Integer& value) {
    if (value < 0) throw std::invalid_argument("negative integers are not encodable");
    if (value == 0) return {};
    const std::size_t size = (mpz_sizeinbase(value.get_mpz_t(), 2) + 7) / 8;
    Bytes out(size);
    std::size_t written = 0;
    mpz_export(out.data(), &written, 1, 1, 1, 0, value.get_mpz_t());
    out.resize(written);
    return out;
}

Integer integerFromMagnitude(const Bytes& magnitude) {
    Integer out;
    if (!magnitude.empty()) mpz_import(out.get_mpz_t(), magnitude.size(), 1, 1, 1, 0, magnitude.data());
    return out;
}

}  // namespace primechain::next
