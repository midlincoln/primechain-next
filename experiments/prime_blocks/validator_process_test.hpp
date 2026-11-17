#pragma once

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>

#include "chain_test_fixtures.hpp"
#include "loopback_test.hpp"
#include "peer_coordinator.hpp"

namespace primechain::experiments::process_test {
using namespace test_support;
constexpr std::array<std::uint8_t, 8> kStartupMagic{'P', 'C', 'N', 'X', 'P', 'W', '0', '2'};

inline void blob(protocol::Bytes& out, const protocol::Bytes& value) {
    appendWord(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

inline std::optional<ValidatorReply> peerExchange(const LocalPeer& peer, const protocol::Bytes& request) {
    try {
        test::Socket socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), 1);
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = htons(peer.port);
        if (connect(socket_fd.fd(), reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) return std::nullopt;
        test::sendFrame(socket_fd.fd(), request);
        ValidatorReply reply;
        std::string error;
        if (!decodeReply(test::receiveFrame(socket_fd.fd()), reply, error)) return std::nullopt;
        return reply;
    } catch (...) { return std::nullopt; }
}

// This entry point is reachable only through a private inherited control socket.
// Fault injection and shutdown are deliberately not validator TCP operations.
inline int workerMain(int control_fd) {
    test::Socket control(control_fd, 30);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() == 1)
        throw std::runtime_error("test worker parent unavailable");
    try {
        const auto startup = test::receiveFrame(control.fd());
        detail::Reader reader(startup);
        reader.magic(kStartupMagic);
        const auto path_bytes = reader.blob(4096);
        const std::string path(path_bytes.begin(), path_bytes.end());
        const bool create = reader.count(1) != 0;
        const bool autonomous = reader.count(1) != 0;
        RangePolicy policy{reader.integer()};
        crypto::SignatureKeyPair identity;
        identity.algorithm = crypto::kProtocolSignatureAlgorithm;
        identity.public_key = reader.blob(crypto::signaturePublicKeySize(identity.algorithm));
        identity.private_key = reader.blob(crypto::signaturePrivateKeySize(identity.algorithm));
        if (identity.public_key.size() != crypto::signaturePublicKeySize(identity.algorithm) ||
            identity.private_key.size() != crypto::signaturePrivateKeySize(identity.algorithm))
            throw std::runtime_error("invalid test worker key size");
        std::vector<Address> validators;
        for (std::size_t i = 0; i < 3; ++i) validators.push_back(reader.address());
        PrimeBlock genesis;
        std::string error;
        if (!decodeBlock(reader.blob(kExperimentMaxBytes), genesis, error)) throw std::runtime_error(error);
        reader.finish();
        ContinuousChain chain(path, genesis, validators, crypto::addressFromProtocolPublicKey(identity.public_key), policy);
        if (!chain.open(create, error)) throw std::runtime_error(error);
        std::unique_ptr<PeerCoordinator> coordinator;
        if (autonomous) {
            coordinator = std::make_unique<PeerCoordinator>((fs::path(path) / "peer-outbox.dat").string(), chain,
                                                          identity, peerExchange);
            if (!coordinator->open(create, error)) throw std::runtime_error(error);
        }
        test::Socket listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = 0;
        if (bind(listener.fd(), reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0 ||
            listen(listener.fd(), 16) != 0) throw std::runtime_error("test worker listen failed");
        socklen_t length = sizeof(endpoint);
        if (getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&endpoint), &length) != 0)
            throw std::runtime_error("test worker address failed");
        protocol::Bytes ready;
        appendWord(ready, ntohs(endpoint.sin_port));
        appendWord(ready, getpid());
        test::sendFrame(control.fd(), encodeReply({ReplyKind::Status, ready, {}}));
        bool drop_reply = false, exit_on_recovery = false;
        bool exit_on_peer_failure = false;
        bool targeted_drop = false;
        RequestKind drop_kind = RequestKind::ChainStatus;
        for (;;) {
            if (coordinator) {
                coordinator->tick();
                if (!coordinator->ready() && exit_on_peer_failure) _exit(71);
            }
            pollfd descriptors[2]{{control.fd(), POLLIN, 0}, {listener.fd(), POLLIN, 0}};
            const auto count = poll(descriptors, 2, coordinator ? 20 : 5000);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error("worker poll failed");
            if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) return 0;
            if (descriptors[0].revents & POLLIN) {
                const auto command = test::receiveFrame(control.fd());
                detail::Reader commands(command);
                const auto kind = commands.count(5);
                if (kind == 1) {
                    const auto point = commands.count(static_cast<std::size_t>(TransitionFailure::AfterActivation));
                    if (!point) throw std::runtime_error("invalid test failure point");
                    chain.failNextTransitionForTest(static_cast<TransitionFailure>(point));
                    exit_on_recovery = true;
                } else if (kind == 2) { drop_reply = true; targeted_drop = false; }
                else if (kind == 3) {
                    if (!coordinator) throw std::runtime_error("peer mode not enabled");
                    std::vector<LocalPeer> peers;
                    for (std::size_t i = 0; i < 2; ++i) {
                        const auto address = commands.address();
                        const auto port = commands.count(65535);
                        if (port == ntohs(endpoint.sin_port)) throw std::runtime_error("peer endpoint cannot be self");
                        peers.push_back({address, static_cast<std::uint16_t>(port)});
                    }
                    coordinator->setPeers(std::move(peers));
                } else if (kind == 4) {
                    if (!coordinator) throw std::runtime_error("peer mode not enabled");
                    const auto point = commands.count(static_cast<std::size_t>(PeerFailure::AfterLocalApply));
                    if (!point) throw std::runtime_error("invalid peer failure point");
                    coordinator->failNextCertificateForTest(static_cast<PeerFailure>(point));
                    exit_on_peer_failure = true;
                } else if (kind == 5) {
                    drop_reply = true;
                    targeted_drop = true;
                    drop_kind = static_cast<RequestKind>(commands.count(static_cast<std::size_t>(RequestKind::DeliverBlock)));
                }
                commands.finish();
                test::sendFrame(control.fd(), encodeReply({ReplyKind::Accepted, {}, {}}));
                if (kind == 0) return 0;
            }
            if (!(descriptors[1].revents & POLLIN)) continue;
            const int fd = accept4(listener.fd(), nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0 && errno == EINTR) continue;
            if (fd < 0) throw std::runtime_error("worker accept failed");
            try {
                test::Socket client(fd);
                const auto request = test::receiveFrame(client.fd());
                const auto reply = coordinator ? coordinator->handle(request) : handleValidatorRequest(request, chain, identity);
                if (exit_on_recovery && reply.kind == ReplyKind::RecoveryRequired) _exit(70);
                ValidatorRequest parsed;
                std::string parse_error;
                if (drop_reply && (!targeted_drop || (decodeRequest(request, parsed, parse_error) && parsed.kind == drop_kind))) {
                    drop_reply = false;
                    targeted_drop = false;
                    continue;
                }
                test::sendFrame(client.fd(), encodeReply(reply));
            } catch (const std::exception&) {
                // A malformed/truncated frame or disconnected client must not stop other clients.
            }
        }
    } catch (const std::exception& exception) {
        try { test::sendFrame(control.fd(), encodeReply({ReplyKind::Rejected, {}, std::string(exception.what()).substr(0, 512)})); }
        catch (...) {}
        return 1;
    }
}

class Worker {
public:
    Worker(std::string executable, fs::path directory, Fixture& fixture, std::size_t index, bool autonomous = false)
        : executable_(std::move(executable)), directory_(std::move(directory)), fixture_(fixture), index_(index), autonomous_(autonomous) {}
    ~Worker() { terminate(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start(bool create) {
        require(pid_ < 0, "worker already started");
        protocol::Bytes startup(kStartupMagic.begin(), kStartupMagic.end());
        const auto path = directory_.string();
        blob(startup, protocol::Bytes(path.begin(), path.end()));
        appendWord(startup, create);
        appendWord(startup, autonomous_);
        appendWord(startup, fixture_.policy.max_members);
        const auto& key = fixture_.validators[index_].key;
        blob(startup, key.public_key);
        blob(startup, key.private_key);
        for (const auto& address : fixture_.addresses) blob(startup, protocol::Bytes(address.begin(), address.end()));
        blob(startup, encodeBlock(fixture_.genesis));
        int pair[2];
        require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "worker control socket pair");
        auto parent = std::make_unique<test::Socket>(pair[0], 30);
        test::Socket child(pair[1], 30);
        const auto parent_pid = getpid();
        pid_ = fork();
        require(pid_ >= 0, "fork validator process");
        if (pid_ == 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent_pid) _exit(126);
            if (child.fd() != 3 && dup2(child.fd(), 3) < 0) _exit(126);
            if (fcntl(3, F_SETFD, 0) < 0) _exit(126);
            execl(executable_.c_str(), executable_.c_str(), "--worker", "3", static_cast<char*>(nullptr));
            _exit(127);
        }
        control_ = std::move(parent);
        try {
            test::sendFrame(control_->fd(), startup);
            ValidatorReply reply;
            std::string error;
            require(decodeReply(test::receiveFrame(control_->fd()), reply, error), "decode worker startup");
            require(reply.kind == ReplyKind::Status, "worker startup: " + reply.error);
            detail::Reader reader(reply.payload);
            port_ = static_cast<std::uint16_t>(reader.count(65535));
            const auto reported_pid = reader.integer();
            reader.finish();
            require(port_ != 0 && reported_pid == static_cast<std::uint64_t>(pid_) && pid_ != getpid(),
                    "separate process reports assigned port and own PID");
        } catch (...) { terminate(); throw; }
    }

    ValidatorReply raw(const protocol::Bytes& request) const {
        require(pid_ > 0, "worker must be started");
        test::Socket client(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), 15);
        connect(client.fd());
        test::sendFrame(client.fd(), request);
        ValidatorReply reply;
        std::string error;
        require(decodeReply(test::receiveFrame(client.fd()), reply, error), "decode worker reply: " + error);
        return reply;
    }

    ValidatorReply call(RequestKind kind, const protocol::Bytes& payload = {}) const {
        return raw(encodeRequest(kind, payload));
    }

    ChainStatus status() const {
        const auto reply = call(RequestKind::ChainStatus);
        ChainStatus result;
        std::string error;
        require(reply.kind == ReplyKind::Status && decodeChainStatus(reply.payload, result, error), "decode chain status");
        return result;
    }

    void armFailure(TransitionFailure point) { command(1, static_cast<std::uint64_t>(point)); }
    void dropNextReply() { command(2); }
    void dropReplyFor(RequestKind kind) { command(5, static_cast<std::uint64_t>(kind)); }
    void armPeerFailure(PeerFailure point) { command(4, static_cast<std::uint64_t>(point)); }
    void configurePeers(const std::vector<LocalPeer>& peers) {
        require(peers.size() == 2, "configure exactly two peers");
        protocol::Bytes bytes;
        appendWord(bytes, 3);
        for (const auto& peer : peers) {
            blob(bytes, {peer.address.begin(), peer.address.end()});
            appendWord(bytes, peer.port);
        }
        controlCommand(bytes);
    }
    void crash() { require(pid_ > 0, "crash running worker"); terminate(); }
    void shutdown() { command(0); expectExit(0); }

    void expectExit(int code) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int status = 0;
        for (;;) {
            const auto result = waitpid(pid_, &status, WNOHANG);
            if (result < 0 && errno == EINTR) continue;
            if (result == pid_) break;
            require(result == 0, "wait for worker exit");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("worker exit timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        pid_ = -1;
        control_.reset();
        require(WIFEXITED(status) && WEXITSTATUS(status) == code, "worker exit status");
    }

    void malformedFrame(bool oversized) const {
        test::Socket client(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        connect(client.fd());
        protocol::Bytes header;
        appendWord(header, oversized ? kExperimentMaxBytes + 1 : 100);
        test::sendBytes(client.fd(), header);
        if (!oversized) require(::shutdown(client.fd(), SHUT_WR) == 0, "truncate test frame");
        char byte{};
        require(recv(client.fd(), &byte, 1, 0) == 0, "malformed frame connection closed");
    }

    pid_t pid() const { return pid_; }
    std::uint16_t port() const { return port_; }
    const fs::path& directory() const { return directory_; }

private:
    void connect(int fd) const {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = htons(port_);
        require(::connect(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0, "connect local validator");
    }

    void command(std::uint64_t kind, std::uint64_t argument = 0) {
        require(pid_ > 0 && control_, "worker control available");
        protocol::Bytes bytes;
        appendWord(bytes, kind);
        if (kind == 1 || kind == 4 || kind == 5) appendWord(bytes, argument);
        controlCommand(bytes);
    }

    void controlCommand(const protocol::Bytes& bytes) {
        require(pid_ > 0 && control_, "worker control available");
        test::sendFrame(control_->fd(), bytes);
        ValidatorReply reply;
        std::string error;
        require(decodeReply(test::receiveFrame(control_->fd()), reply, error) && reply.kind == ReplyKind::Accepted,
                "private worker control acknowledgement");
    }

    void terminate() noexcept {
        if (pid_ > 0) {
            int status = 0;
            pid_t result;
            do { result = waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
            if (result == 0) {
                kill(pid_, SIGKILL);
                do { result = waitpid(pid_, &status, 0); } while (result < 0 && errno == EINTR);
            }
            pid_ = -1;
        }
        control_.reset();
    }

    std::string executable_;
    fs::path directory_;
    Fixture& fixture_;
    std::size_t index_;
    bool autonomous_;
    pid_t pid_{-1};
    std::uint16_t port_{0};
    std::unique_ptr<test::Socket> control_;
};

} // namespace primechain::experiments::process_test
