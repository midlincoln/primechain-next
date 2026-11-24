#include "primechain_next/store.hpp"

#include "primechain_next/merkle.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
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
        return writeIndex(error) && syncDirectory(dir_, error);
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
    return consistent || writeIndex(error);  // the index is derived; rebuild it when inconsistent
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

}  // namespace primechain::next
