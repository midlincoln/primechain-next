#include "primechain_next/store.hpp"

#include "primechain_next/merkle.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace primechain::next {
namespace {

namespace fs = std::filesystem;
constexpr char kSnapshotMagic[8] = {'P', 'C', 'N', 'X', 'S', 'N', 'A', 'P'};
constexpr std::uint64_t kSnapshotVersion = 1;

void putU64(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint64_t getU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool readFile(const std::string& path, Bytes& out, std::string& error) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::uint8_t buffer[1 << 16];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = "read " + path + ": " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        out.insert(out.end(), buffer, buffer + n);
    }
    ::close(fd);
    return true;
}

bool writeAll(int fd, const Bytes& data, std::string& error) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = std::string("write: ") + std::strerror(errno);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool syncDirectory(const std::string& dir, std::string& error) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) {
        error = "fsync directory " + dir + ": " + std::strerror(errno);
        if (fd >= 0) ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

// Write a whole file atomically: temp file, fsync, rename, fsync directory.
bool replaceFile(const std::string& dir, const std::string& name, const Bytes& data, std::string& error) {
    const std::string tmp = dir + "/" + name + ".tmp", final_path = dir + "/" + name;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "create " + tmp + ": " + std::strerror(errno);
        return false;
    }
    if (!writeAll(fd, data, error) || ::fsync(fd) != 0) {
        if (error.empty()) error = "fsync " + tmp + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        error = "rename " + tmp + ": " + std::strerror(errno);
        return false;
    }
    return syncDirectory(dir, error);
}

}  // namespace

bool Store::open(const std::string& directory, std::string& error) {
    dir_ = directory;
    offsets_.clear();
    truncated_ = false;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        error = "create " + dir_ + ": " + ec.message();
        return false;
    }
    const std::string path = dir_ + "/blocks.dat";
    if (!fs::exists(path)) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            error = "create " + path + ": " + std::strerror(errno);
            return false;
        }
        ::close(fd);
        end_ = 0;
        if (!writeIndex(error) || !syncDirectory(dir_, error) || !rings_.open(dir_, error)) return false;
        rows_ = std::make_shared<RowIndex>();
        return rows_->open(dir_, error);
    }
    Bytes data;
    if (!readFile(path, data, error)) return false;
    std::size_t pos = 0;
    while (pos < data.size()) {
        if (data.size() - pos < 8) break;  // incomplete length: interrupted append
        const std::uint64_t length = getU64(data.data() + pos);
        if (length > data.size() || data.size() - pos - 8 < length + 32) break;  // incomplete frame
        const Bytes body(data.begin() + static_cast<std::ptrdiff_t>(pos + 8),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 8 + length));
        const Hash256 sum = sha3(body);
        if (std::memcmp(sum.data(), data.data() + pos + 8 + length, 32) != 0) {
            error = "block file corrupt: checksum mismatch in frame " + std::to_string(offsets_.size());
            return false;
        }
        offsets_.push_back(pos);
        pos += 8 + length + 32;
    }
    end_ = pos;
    if (pos != data.size()) {  // drop an incomplete final frame left by an interrupted append
        if (::truncate(path.c_str(), static_cast<off_t>(pos)) != 0) {
            error = "truncate " + path + ": " + std::strerror(errno);
            return false;
        }
        truncated_ = true;
    }
    Bytes index;
    std::string ignored;
    bool consistent = fs::exists(dir_ + "/blocks.idx") && readFile(dir_ + "/blocks.idx", index, ignored) &&
                      index.size() == offsets_.size() * 8;
    for (std::size_t i = 0; consistent && i < offsets_.size(); ++i)
        consistent = getU64(index.data() + 8 * i) == offsets_[i];
    if (!consistent && !writeIndex(error)) return false;  // the block index is derived; rebuilt when inconsistent
    if (!rings_.open(dir_, error)) return false;
    rows_ = std::make_shared<RowIndex>();
    return rows_->open(dir_, error);
}

bool Store::writeIndex(std::string& error) const {
    Bytes index;
    for (const auto offset : offsets_) putU64(index, offset);
    return replaceFile(dir_, "blocks.idx", index, error);
}

bool Store::append(const Bytes& block, std::string& error) {
    Bytes frame;
    putU64(frame, block.size());
    frame.insert(frame.end(), block.begin(), block.end());
    const Hash256 sum = sha3(block);
    frame.insert(frame.end(), sum.begin(), sum.end());
    const std::string path = dir_ + "/blocks.dat";
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    if (!writeAll(fd, frame, error) || ::fsync(fd) != 0) {
        if (error.empty()) error = "fsync " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    offsets_.push_back(end_);
    end_ += frame.size();
    // Append the new offset to the index (derived data; a stale index is rebuilt on open).
    Bytes entry;
    putU64(entry, offsets_.back());
    const int ifd = ::open((dir_ + "/blocks.idx").c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (ifd < 0 || !writeAll(ifd, entry, error)) {
        if (ifd >= 0) ::close(ifd);
        if (error.empty()) error = "append index";
        return false;
    }
    ::close(ifd);
    return true;
}

bool Store::read(std::uint64_t index, Bytes& block, std::string& error) const {
    if (index >= offsets_.size()) {
        error = "block index out of range";
        return false;
    }
    const int fd = ::open((dir_ + "/blocks.dat").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = std::string("open blocks.dat: ") + std::strerror(errno);
        return false;
    }
    std::uint8_t header[8];
    bool ok = ::pread(fd, header, 8, static_cast<off_t>(offsets_[index])) == 8;
    const std::uint64_t length = ok ? getU64(header) : 0;
    block.assign(length, 0);
    Hash256 stored{};
    ok = ok && ::pread(fd, block.data(), length, static_cast<off_t>(offsets_[index] + 8)) == static_cast<ssize_t>(length) &&
         ::pread(fd, stored.data(), 32, static_cast<off_t>(offsets_[index] + 8 + length)) == 32;
    ::close(fd);
    if (!ok || sha3(block) != stored) {
        error = "block file corrupt at frame " + std::to_string(index);
        return false;
    }
    return true;
}

bool Store::writeSnapshot(std::uint64_t height, const Bytes& state, std::string& error) {
    Bytes data(kSnapshotMagic, kSnapshotMagic + 8);
    putU64(data, kSnapshotVersion);
    putU64(data, height);
    putU64(data, state.size());
    data.insert(data.end(), state.begin(), state.end());
    const Hash256 sum = sha3(state);
    data.insert(data.end(), sum.begin(), sum.end());
    return replaceFile(dir_, "snapshot.dat", data, error);
}

bool Store::loadSnapshot(std::uint64_t& height, Bytes& state, bool& found, std::string& error) const {
    found = false;
    const std::string path = dir_ + "/snapshot.dat";
    if (!fs::exists(path)) return true;
    Bytes data;
    if (!readFile(path, data, error)) return false;
    if (data.size() < 8 + 24 + 32 || std::memcmp(data.data(), kSnapshotMagic, 8) != 0 ||
        getU64(data.data() + 8) != kSnapshotVersion) {
        error = "snapshot corrupt: bad header";
        return false;
    }
    height = getU64(data.data() + 16);
    const std::uint64_t length = getU64(data.data() + 24);
    if (length != data.size() - 32 - 32) {
        error = "snapshot corrupt: bad length";
        return false;
    }
    state.assign(data.begin() + 32, data.begin() + 32 + static_cast<std::ptrdiff_t>(length));
    const Hash256 sum = sha3(state);
    if (std::memcmp(sum.data(), data.data() + 32 + length, 32) != 0) {
        error = "snapshot corrupt: checksum mismatch";
        return false;
    }
    found = true;
    return true;
}

// ---------------------------------------------------------------- B3.5: the rings file

namespace {

std::uint64_t check8(const Bytes& data) {
    const Hash256 h = sha3(data);
    return getU64(h.data());
}

Bytes tagged(const char* tag) {
    Bytes b;
    for (const char* c = tag; *c; ++c) b.push_back(static_cast<std::uint8_t>(*c));
    return b;
}

std::uint64_t ringIndexCheck(std::uint64_t offset, std::uint64_t height) {
    Bytes b = tagged("primechain-next/rings-idx");
    putU64(b, offset);
    putU64(b, height);
    return check8(b);
}

bool createEmpty(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "create " + path + ": " + std::strerror(errno);
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) error = "fsync " + path + ": " + std::strerror(errno);
    return ok;
}

bool appendSynced(const std::string& path, const Bytes& data, std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    if (!writeAll(fd, data, error) || ::fsync(fd) != 0) {
        if (error.empty()) error = "fsync " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

bool pwriteAll(int fd, const std::uint8_t* data, std::size_t size, std::uint64_t offset, std::string& error) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pwrite(fd, data + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            error = std::string("pwrite: ") + std::strerror(errno);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

bool RingFile::open(const std::string& directory, std::string& error) {
    dir_ = directory;
    frames_.clear();
    end_ = 0;
    const std::string path = dir_ + "/rings.dat", idx = dir_ + "/rings.idx";
    const bool have_data = fs::exists(path), have_index = fs::exists(idx);
    if (!have_data && !have_index) return createEmpty(path, error) && createEmpty(idx, error) && syncDirectory(dir_, error);
    if (!have_data || !have_index) {
        error = "rings file or its index is missing (not recreated)";
        return false;
    }
    Bytes data;
    if (!readFile(path, data, error)) return false;
    std::size_t pos = 0;
    while (pos < data.size()) {
        if (data.size() - pos < 16) break;  // incomplete header: interrupted append
        const std::uint64_t length = getU64(data.data() + pos), height = getU64(data.data() + pos + 8);
        if (length > data.size() || data.size() - pos - 16 < length + 32) break;  // incomplete frame
        const Bytes body(data.begin() + static_cast<std::ptrdiff_t>(pos + 8),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 16 + length));  // height || batch
        const Hash256 sum = sha3(body);
        if (std::memcmp(sum.data(), data.data() + pos + 16 + length, 32) != 0) {
            error = "rings file corrupt: checksum mismatch in frame " + std::to_string(frames_.size());
            return false;
        }
        if (!frames_.empty() && height <= frames_.back().second) {
            error = "rings file corrupt: open heights out of order";
            return false;
        }
        frames_.emplace_back(pos, height);
        pos += 16 + length + 32;
    }
    end_ = pos;
    if (pos != data.size() && ::truncate(path.c_str(), static_cast<off_t>(pos)) != 0) {
        error = "truncate " + path + ": " + std::strerror(errno);
        return false;
    }
    Bytes index;
    if (!readFile(idx, index, error)) return false;
    if (index.size() % 24 != 0) {
        error = "rings index corrupt: bad length";
        return false;
    }
    const std::size_t entries = index.size() / 24;
    if (entries > frames_.size() || entries + 1 < frames_.size()) {
        error = "rings index does not match the rings file";
        return false;
    }
    for (std::size_t i = 0; i < entries; ++i) {
        const std::uint8_t* e = index.data() + 24 * i;
        const std::uint64_t offset = getU64(e), height = getU64(e + 8);
        if (getU64(e + 16) != ringIndexCheck(offset, height)) {
            error = "rings index corrupt: checksum mismatch in entry " + std::to_string(i);
            return false;
        }
        if (offset != frames_[i].first || height != frames_[i].second) {
            error = "rings index does not match the rings file";
            return false;
        }
    }
    if (entries + 1 == frames_.size()) {  // an append interrupted between the frame and its entry
        Bytes entry;
        putU64(entry, frames_.back().first);
        putU64(entry, frames_.back().second);
        putU64(entry, ringIndexCheck(frames_.back().first, frames_.back().second));
        return appendSynced(idx, entry, error);
    }
    return true;
}

bool RingFile::append(std::uint64_t open_height, const Bytes& batch, std::string& error) {
    if (!frames_.empty() && open_height <= frames_.back().second) {
        error = "rings file: open heights must increase";
        return false;
    }
    Bytes body;
    putU64(body, open_height);
    body.insert(body.end(), batch.begin(), batch.end());
    Bytes frame;
    putU64(frame, batch.size());
    frame.insert(frame.end(), body.begin(), body.end());
    const Hash256 sum = sha3(body);
    frame.insert(frame.end(), sum.begin(), sum.end());
    if (!appendSynced(dir_ + "/rings.dat", frame, error)) return false;
    frames_.emplace_back(end_, open_height);
    end_ += frame.size();
    Bytes entry;
    putU64(entry, frames_.back().first);
    putU64(entry, open_height);
    putU64(entry, ringIndexCheck(frames_.back().first, open_height));
    return appendSynced(dir_ + "/rings.idx", entry, error);
}

bool RingFile::read(std::uint64_t index, Bytes& batch, std::string& error) const {
    if (index >= frames_.size()) {
        error = "ring batch index out of range";
        return false;
    }
    const int fd = ::open((dir_ + "/rings.dat").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = std::string("open rings.dat: ") + std::strerror(errno);
        return false;
    }
    const std::uint64_t at = frames_[index].first;
    std::uint8_t head[16];
    bool ok = ::pread(fd, head, 16, static_cast<off_t>(at)) == 16;
    const std::uint64_t length = ok ? getU64(head) : 0;
    ok = ok && length < (1ull << 40);
    Bytes body(ok ? 8 + length : 0, 0);
    Hash256 stored{};
    ok = ok && ::pread(fd, body.data(), body.size(), static_cast<off_t>(at + 8)) == static_cast<ssize_t>(body.size()) &&
         ::pread(fd, stored.data(), 32, static_cast<off_t>(at + 16 + length)) == 32;
    ::close(fd);
    if (!ok || sha3(body) != stored || getU64(body.data()) != frames_[index].second) {
        error = "rings file corrupt at frame " + std::to_string(index);
        return false;
    }
    batch.assign(body.begin() + 8, body.end());
    return true;
}

// ---------------------------------------------------------------- B3.5: the row index

namespace {

constexpr char kRowsMagic[8] = {'P', 'C', 'N', 'X', 'R', 'O', 'W', 'S'};
constexpr char kChunkMagic[8] = {'P', 'C', 'N', 'X', 'R', 'O', 'W', 'C'};
constexpr std::uint64_t kRowsVersion = 1;
constexpr std::size_t kHeaderBytes = 32;
constexpr std::uint64_t kChunkBytes = kHeaderBytes + RowIndex::kRowsPerChunk * RowIndex::kRecordBytes;

std::uint64_t headerCheck(const char magic[8], std::uint64_t a, std::uint64_t b) {
    Bytes x = tagged("primechain-next/rows-header");
    x.insert(x.end(), magic, magic + 8);
    putU64(x, a);
    putU64(x, b);
    return check8(x);
}

Bytes headerBytes(const char magic[8], std::uint64_t a, std::uint64_t b) {
    Bytes h(magic, magic + 8);
    putU64(h, a);
    putU64(h, b);
    putU64(h, headerCheck(magic, a, b));
    return h;
}

std::uint64_t recordCheck(std::uint64_t ring, std::uint64_t slot, std::uint64_t height, std::uint64_t miner,
                          std::uint8_t shape) {
    Bytes x = tagged("primechain-next/row-record");
    putU64(x, ring);
    putU64(x, slot);
    putU64(x, height);
    putU64(x, miner);
    x.push_back(shape);
    return check8(x);
}

std::uint64_t minerCheck(const Address& a) {
    Bytes x = tagged("primechain-next/row-miner");
    x.insert(x.end(), a.begin(), a.end());
    return check8(x);
}

bool isZero(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

}  // namespace

RowIndex::~RowIndex() {
    if (rows_fd_ >= 0) ::close(rows_fd_);
    if (miners_fd_ >= 0) ::close(miners_fd_);
}

bool RowIndex::open(const std::string& directory, std::string& error) {
    dir_ = directory;
    const std::string rows = dir_ + "/rows.dat", miners = dir_ + "/miners.dat";
    const bool have_rows = fs::exists(rows), have_miners = fs::exists(miners);
    if (!have_rows && !have_miners) {
        if (!createEmpty(rows, error) || !createEmpty(miners, error) ||
            !appendSynced(rows, headerBytes(kRowsMagic, kRowsVersion, kRowsPerChunk), error) || !syncDirectory(dir_, error))
            return false;
    } else if (!have_rows || !have_miners) {
        error = "row index or its miner table is missing (not recreated)";
        return false;
    }
    rows_fd_ = ::open(rows.c_str(), O_RDWR | O_CLOEXEC);
    miners_fd_ = ::open(miners.c_str(), O_RDWR | O_CLOEXEC);
    if (rows_fd_ < 0 || miners_fd_ < 0) {
        error = "open row index: " + std::string(std::strerror(errno));
        return false;
    }
    // The miner table first: records refer to it.
    Bytes data;
    if (!readFile(miners, data, error)) return false;
    std::size_t pos = 0;
    while (pos < data.size()) {
        if (data.size() - pos < 4) break;
        const std::uint32_t n = static_cast<std::uint32_t>(data[pos]) | static_cast<std::uint32_t>(data[pos + 1]) << 8 |
                                static_cast<std::uint32_t>(data[pos + 2]) << 16 |
                                static_cast<std::uint32_t>(data[pos + 3]) << 24;
        if (data.size() - pos - 4 < static_cast<std::size_t>(n) + 8) break;  // interrupted append
        const Address a(data.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                        data.begin() + static_cast<std::ptrdiff_t>(pos + 4 + n));
        if (getU64(data.data() + pos + 4 + n) != minerCheck(a) || a.empty() || miner_ids_.count(a)) {
            error = "row index corrupt: bad miner entry " + std::to_string(miners_.size() + 1);
            return false;
        }
        miners_.push_back(a);
        miner_ids_.emplace(a, miners_.size());
        pos += 4 + n + 8;
    }
    miners_end_ = pos;
    if (pos != data.size() && ::ftruncate(miners_fd_, static_cast<off_t>(pos)) != 0) {
        error = "truncate miners.dat: " + std::string(std::strerror(errno));
        return false;
    }
    // Rows: the header, then whole chunks; every chunk header and every record is verified.
    if (!readFile(rows, data, error)) return false;
    if (data.size() < kHeaderBytes || std::memcmp(data.data(), kRowsMagic, 8) != 0 ||
        getU64(data.data() + 8) != kRowsVersion || getU64(data.data() + 16) != kRowsPerChunk ||
        getU64(data.data() + 24) != headerCheck(kRowsMagic, kRowsVersion, kRowsPerChunk)) {
        error = "row index corrupt: bad header";
        return false;
    }
    pos = kHeaderBytes;
    while (data.size() - pos >= kChunkBytes) {
        const std::uint8_t* c = data.data() + pos;
        const std::uint64_t ring = getU64(c + 8), chunk = getU64(c + 16);
        if (std::memcmp(c, kChunkMagic, 8) != 0 || getU64(c + 24) != headerCheck(kChunkMagic, ring, chunk) ||
            !chunks_.emplace(std::make_pair(ring, chunk), pos).second) {
            error = "row index corrupt: bad chunk header at offset " + std::to_string(pos);
            return false;
        }
        for (std::uint64_t k = 0; k < kRowsPerChunk; ++k) {
            const std::uint8_t* r = c + kHeaderBytes + k * kRecordBytes;
            if (isZero(r, kRecordBytes)) continue;
            const std::uint64_t slot = chunk * kRowsPerChunk + k + 1, height = getU64(r), miner = getU64(r + 8);
            if (r[16] == Empty || r[16] > Ramified || !isZero(r + 17, 7) || miner == 0 || miner > miners_.size() ||
                getU64(r + 24) != recordCheck(ring, slot, height, miner, r[16])) {
                error = "row index corrupt: bad record (ring " + std::to_string(ring) + ", slot " + std::to_string(slot) + ")";
                return false;
            }
        }
        pos += kChunkBytes;
    }
    rows_end_ = pos;
    if (pos != data.size() && ::ftruncate(rows_fd_, static_cast<off_t>(pos)) != 0) {  // interrupted chunk allocation
        error = "truncate rows.dat: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool RowIndex::readRecord(std::uint64_t offset, std::uint64_t ring, std::uint64_t slot, std::optional<Record>& out,
                          std::string& error) const {
    std::uint8_t r[kRecordBytes];
    out.reset();
    if (::pread(rows_fd_, r, kRecordBytes, static_cast<off_t>(offset)) != static_cast<ssize_t>(kRecordBytes)) {
        error = "row index: read failed";
        return false;
    }
    if (isZero(r, kRecordBytes)) return true;
    const std::uint64_t height = getU64(r), miner = getU64(r + 8);
    if (r[16] == Empty || r[16] > Ramified || !isZero(r + 17, 7) || miner == 0 || miner > miners_.size() ||
        getU64(r + 24) != recordCheck(ring, slot, height, miner, r[16])) {
        error = "row index corrupt: bad record (ring " + std::to_string(ring) + ", slot " + std::to_string(slot) + ")";
        return false;
    }
    out = Record{height, miners_[miner - 1], r[16]};
    return true;
}

std::optional<RowIndex::Record> RowIndex::get(std::uint64_t ring, std::uint64_t slot) const {
    if (slot == 0) return std::nullopt;
    const auto it = chunks_.find({ring, (slot - 1) / kRowsPerChunk});
    if (it == chunks_.end()) return std::nullopt;
    std::optional<Record> out;
    std::string error;
    if (!readRecord(it->second + kHeaderBytes + ((slot - 1) % kRowsPerChunk) * kRecordBytes, ring, slot, out, error))
        throw std::runtime_error(error);
    return out;
}

bool RowIndex::put(std::uint64_t ring, std::uint64_t slot, const Record& record, std::string& error) {
    if (slot == 0 || record.shape == Empty || record.shape > Ramified || record.miner.empty()) {
        error = "row index: bad record";
        return false;
    }
    std::uint64_t miner = 0;
    if (const auto it = miner_ids_.find(record.miner); it != miner_ids_.end()) {
        miner = it->second;
    } else {  // a new miner: into the table first, so no record refers to a missing entry
        Bytes frame;
        const auto n = static_cast<std::uint32_t>(record.miner.size());
        for (int i = 0; i < 4; ++i) frame.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
        frame.insert(frame.end(), record.miner.begin(), record.miner.end());
        putU64(frame, minerCheck(record.miner));
        if (!pwriteAll(miners_fd_, frame.data(), frame.size(), miners_end_, error) || ::fdatasync(miners_fd_) != 0) {
            if (error.empty()) error = "fsync miners.dat: " + std::string(std::strerror(errno));
            return false;
        }
        miners_end_ += frame.size();
        miners_.push_back(record.miner);
        miner = miners_.size();
        miner_ids_.emplace(record.miner, miner);
    }
    const std::pair<std::uint64_t, std::uint64_t> key{ring, (slot - 1) / kRowsPerChunk};
    auto it = chunks_.find(key);
    if (it == chunks_.end()) {  // allocate a chunk: its header and empty slots in one write
        Bytes chunk = headerBytes(kChunkMagic, ring, key.second);
        chunk.resize(kChunkBytes, 0);
        if (!pwriteAll(rows_fd_, chunk.data(), chunk.size(), rows_end_, error)) return false;
        it = chunks_.emplace(key, rows_end_).first;
        rows_end_ += kChunkBytes;
    }
    Bytes r;
    putU64(r, record.height);
    putU64(r, miner);
    r.push_back(record.shape);
    r.resize(24, 0);
    putU64(r, recordCheck(ring, slot, record.height, miner, record.shape));
    return pwriteAll(rows_fd_, r.data(), r.size(),
                     it->second + kHeaderBytes + ((slot - 1) % kRowsPerChunk) * kRecordBytes, error);
}

bool RowIndex::sync(std::string& error) {
    if (::fdatasync(rows_fd_) != 0) {
        error = "fsync rows.dat: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

void RowIndex::forEach(const std::function<void(std::uint64_t, std::uint64_t, const Record&)>& f) const {
    for (const auto& [key, offset] : chunks_)
        for (std::uint64_t k = 0; k < kRowsPerChunk; ++k) {
            const std::uint64_t slot = key.second * kRowsPerChunk + k + 1;
            std::optional<Record> r;
            std::string error;
            if (!readRecord(offset + kHeaderBytes + k * kRecordBytes, key.first, slot, r, error))
                throw std::runtime_error(error);
            if (r) f(key.first, slot, *r);
        }
}

std::uint64_t RowIndex::bytes() const { return rows_end_ + miners_end_; }

}  // namespace primechain::next
