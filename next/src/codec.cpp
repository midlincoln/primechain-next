#include "primechain_next/codec.hpp"

namespace primechain::next {

void Writer::varint128(u128 value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7f);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out_.push_back(byte);
    } while (value != 0);
}

void Writer::varint(std::uint64_t value) { varint128(value); }
void Writer::amount(const Amount& value) { varint128(value.raw()); }

void Writer::integer(const Integer& value) {
    const Bytes magnitude = integerMagnitude(value);
    varint(magnitude.size());
    out_.insert(out_.end(), magnitude.begin(), magnitude.end());
}

void Writer::bytes(const Bytes& value) {
    varint(value.size());
    out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::text(const std::string& value) {
    varint(value.size());
    out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::hash(const Hash256& value) { out_.insert(out_.end(), value.begin(), value.end()); }

bool Reader::u8(std::uint8_t& value) {
    if (pos_ >= data_.size()) return fail("truncated input");
    value = data_[pos_++];
    return true;
}

bool Reader::varint128(u128& value, unsigned max_bits) {
    value = 0;
    unsigned shift = 0;
    for (;;) {
        std::uint8_t byte = 0;
        if (!u8(byte)) return false;
        const u128 chunk = byte & 0x7f;
        if (shift >= max_bits || (shift > 0 && (chunk >> (max_bits - shift)) != 0)) return fail("varint overflow");
        value |= chunk << shift;
        if ((byte & 0x80) == 0) {
            if (byte == 0 && shift != 0) return fail("non-minimal varint");
            return true;
        }
        shift += 7;
    }
}

bool Reader::varint(std::uint64_t& value) {
    u128 wide = 0;
    if (!varint128(wide, 64)) return false;
    value = static_cast<std::uint64_t>(wide);
    return true;
}

bool Reader::amount(Amount& value) {
    u128 wide = 0;
    if (!varint128(wide, 128)) return false;
    value = Amount::fromRaw(wide);
    return true;
}

bool Reader::integer(Integer& value) {
    std::uint64_t size = 0;
    if (!varint(size)) return false;
    if (size > max_integer_bytes_) return fail("integer exceeds maximum size");
    if (size > data_.size() - pos_) return fail("truncated integer");
    if (size > 0 && data_[pos_] == 0) return fail("non-minimal integer encoding");
    value = integerFromMagnitude(Bytes(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                       data_.begin() + static_cast<std::ptrdiff_t>(pos_ + size)));
    pos_ += size;
    return true;
}

bool Reader::bytes(Bytes& value, std::size_t max_size) {
    std::uint64_t size = 0;
    if (!varint(size)) return false;
    if (size > max_size) return fail("byte string exceeds maximum size");
    if (size > data_.size() - pos_) return fail("truncated byte string");
    value.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + size));
    pos_ += size;
    return true;
}

bool Reader::text(std::string& value, std::size_t max_size) {
    Bytes raw;
    if (!bytes(raw, max_size)) return false;
    value.assign(raw.begin(), raw.end());
    return true;
}

bool Reader::hash(Hash256& value) {
    if (data_.size() - pos_ < value.size()) return fail("truncated hash");
    for (auto& byte : value) byte = data_[pos_++];
    return true;
}

}  // namespace primechain::next
