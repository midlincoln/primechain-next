#pragma once

#include <cerrno>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "binary.hpp"
#include "validator_messages.hpp"

namespace primechain::experiments::test {

class Socket {
public:
    explicit Socket(int fd, long timeout_seconds = 3) : fd_(fd) {
        if (fd < 0) throw std::runtime_error("test socket creation failed");
        const timeval timeout{timeout_seconds, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            ::close(fd);
            throw std::runtime_error("test socket timeout setup failed");
        }
    }
    ~Socket() { ::close(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int fd() const { return fd_; }
private:
    int fd_;
};

inline void sendBytes(int fd, const protocol::Bytes& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("test socket write failed or timed out");
        offset += n;
    }
}

inline protocol::Bytes receiveBytes(int fd, std::size_t size) {
    if (size > kExperimentMaxBytes) throw std::runtime_error("test frame too large");
    protocol::Bytes bytes(size);
    std::size_t offset = 0;
    while (offset < size) {
        const auto n = recv(fd, bytes.data() + offset, size - offset, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("test socket read failed or timed out");
        offset += n;
    }
    return bytes;
}

inline void sendFrame(int fd, const protocol::Bytes& bytes) {
    if (bytes.size() > kExperimentMaxBytes) throw std::runtime_error("test frame too large");
    protocol::Bytes header;
    appendWord(header, bytes.size());
    sendBytes(fd, header);
    sendBytes(fd, bytes);
}

inline protocol::Bytes receiveFrame(int fd) {
    const auto header = receiveBytes(fd, 8);
    detail::Reader reader(header);
    return receiveBytes(fd, reader.count(kExperimentMaxBytes));
}

// One bounded TCP exchange, loopback only, ephemeral port, no persistent services.
inline ValidatorReply exchange(const protocol::Bytes& request, RangeJournal& journal,
                               const crypto::SignatureKeyPair& key) {
    Socket listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.sin_port = 0;
    if (bind(listener.fd(), reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0 ||
        listen(listener.fd(), 1) != 0) throw std::runtime_error("test loopback listen failed");
    socklen_t length = sizeof(endpoint);
    if (getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&endpoint), &length) != 0)
        throw std::runtime_error("test loopback address failed");
    Socket client(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (connect(client.fd(), reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0)
        throw std::runtime_error("test loopback connect failed");
    Socket server(accept(listener.fd(), nullptr, nullptr));
    auto worker = std::async(std::launch::async, [&] {
        const auto bytes = receiveFrame(server.fd());
        sendFrame(server.fd(), encodeReply(handleValidatorRequest(bytes, journal, key)));
    });
    sendFrame(client.fd(), request);
    const auto bytes = receiveFrame(client.fd());
    worker.get();
    ValidatorReply reply;
    std::string error;
    if (!decodeReply(bytes, reply, error)) throw std::runtime_error("test reply: " + error);
    return reply;
}

} // namespace primechain::experiments::test
