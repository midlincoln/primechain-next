#pragma once
// Canonical, strict binary codec (D12/D14). Unsigned LEB128 varints (minimal form only),
// length-prefixed minimal integers, length-prefixed byte strings, raw 32-byte hashes.
#include "primechain/types.hpp"
#include "primechain_next/numbers.hpp"

#include <cstddef>
#include <string>

namespace primechain::next {

using primechain::Address;
using primechain::Hash256;

class Writer {
public:
    void u8(std::uint8_t value) { out_.push_back(value); }
    void varint(std::uint64_t value);
    void amount(const Amount& value);
    void integer(const Integer& value);
    void bytes(const Bytes& value);
    void text(const std::string& value);
    void hash(const Hash256& value);
    const Bytes& data() const { return out_; }

private:
    void varint128(u128 value);
    Bytes out_;
};

class Reader {
public:
    Reader(const Bytes& data, std::size_t max_integer_bytes) : data_(data), max_integer_bytes_(max_integer_bytes) {}
    bool u8(std::uint8_t& value);
    bool varint(std::uint64_t& value);
    bool amount(Amount& value);
    bool integer(Integer& value);
    bool bytes(Bytes& value, std::size_t max_size);
    bool text(std::string& value, std::size_t max_size);
    bool hash(Hash256& value);
    bool done() const { return pos_ == data_.size(); }
    const std::string& error() const { return error_; }

private:
    bool varint128(u128& value, unsigned max_bits);
    bool fail(const std::string& message) { if (error_.empty()) error_ = message; return false; }
    const Bytes& data_;
    std::size_t pos_{0};
    std::size_t max_integer_bytes_;
    std::string error_;
};

}  // namespace primechain::next
