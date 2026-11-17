#include "validator_process_test.hpp"

namespace {
using namespace primechain::experiments::process_test;

void expectKind(const ValidatorReply& reply, ReplyKind expected, const std::string& label) {
    require(reply.kind == expected, label + ": " + reply.error);
}

template<class Predicate> void eventually(Predicate predicate, const std::string& label) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "timed out: " + label);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    require(true, label);
}

void verifyVotes(const std::vector<protocol::ValidatorVoteV0>& votes, const Hash256& subject,
                 const Hash256& previous, PrimeValue integer, const Fixture& f) {
    protocol::FinalizationProofV0 proof;
    proof.rule = "fixed-2-of-3-mldsa65-v2";
    proof.votes = votes;
    auto validators = f.addresses;
    std::sort(validators.begin(), validators.end());
    std::string error;
    const bool valid = protocol::verifyRecordFinalization(proof, subject, previous, integer, validators, error);
    require(valid, "worker-assembled quorum: " + error);
}

void lostReply(Worker& worker, RequestKind kind, const protocol::Bytes& payload) {
    bool lost = false;
    try { (void)worker.call(kind, payload); } catch (const std::exception&) { lost = true; }
    require(lost, "injected lost miner acknowledgement");
}

int run(const fs::path& root, const std::string& executable) {
    fs::create_directories(root);
    auto name = (root / "peers-XXXXXX").string();
    require(mkdtemp(name.data()) != nullptr, "create private peer fixture");
    const auto directory = fs::absolute(name);
    Fixture f;
    Worker a(executable, directory / "a", f, 0, true), b(executable, directory / "b", f, 1, true),
           c(executable, directory / "c", f, 2, true);
    std::array<Worker*, 3> workers{&a, &b, &c};
    for (auto* worker : workers) worker->start(true);
    const auto configure = [&] {
        for (std::size_t i = 0; i < workers.size(); ++i) {
            if (workers[i]->pid() < 0) continue;
            std::vector<LocalPeer> peers;
            for (std::size_t j = 0; j < workers.size(); ++j)
                if (i != j) peers.push_back({f.validators[j].address, workers[j]->port()});
            workers[i]->configurePeers(peers);
        }
    };
    configure();
    std::cout << "autonomous workers: " << a.pid() << ' ' << b.pid() << ' ' << c.pid() << std::endl;
    PrimeBlockReplay expected(f.addresses);
    require(expected.append(f.genesis, f.error), "reference genesis");
    const auto synchronized = [&](PrimeValue accepted, const Hash256& range_tip) {
        eventually([&] {
            for (const auto* worker : workers) {
                if (worker->pid() < 0) continue;
                const auto status = worker->status();
                require(status.ready && status.max_range_members == f.policy.max_members, "worker remains ready");
                if (status.block_count != expected.blockCount() || status.block_tip != expected.tip() ||
                    status.finalized_frontier != expected.frontier() || status.accepted_frontier != accepted ||
                    status.range_tip != range_tip) return false;
            }
            return true;
        }, "peer-driven synchronization at " + std::to_string(accepted));
    };
    for (const auto kind : {RequestKind::SubmitRange, RequestKind::SubmitBlock,
                            RequestKind::DeliverRange, RequestKind::DeliverBlock})
        expectKind(a.call(kind, {0xff}), ReplyKind::Rejected, "malformed payload does not poison coordinator");
    synchronized(2, {});
    std::vector<CompositeProof> pending;
    std::optional<CompositeRange> first_range;
    Hash256 range_tip{};
    for (PrimeValue n = 3; n <= 101;) {
        Worker& ingress = n > 23 && expected.blockCount() % 2 ? b : a;
        if (n == 8) c.crash();
        if (!math::isPrime(n)) {
            auto last = n;
            while (last + 1 <= 101 && last - n + 1 < f.policy.max_members && !math::isPrime(last + 1)) ++last;
            const auto& provider = (n / 2) % 2 ? f.alice : f.bob;
            auto proposal = f.range(expected, n, last, provider, range_tip);
            proposal.votes.clear();
            const auto bytes = encodeRange(proposal);
            if (n == 8) {
                auto oversized = f.range(expected, 8, 10, provider, range_tip);
                oversized.votes.clear();
                expectKind(a.call(RequestKind::SubmitRange, encodeRange(oversized)), ReplyKind::Rejected, "range policy limit");
                auto invalid = proposal;
                invalid.factors.back() = {1, last};
                const auto signature = crypto::signProtocolMessage(provider.key.private_key, rangeSigningPayload(invalid), f.error);
                require(signature.has_value(), "sign invalid math fixture");
                invalid.signature = *signature;
                expectKind(a.call(RequestKind::SubmitRange, encodeRange(invalid)), ReplyKind::Rejected,
                           "invalid member rejects entire miner range");
                synchronized(n - 1, range_tip);
            }
            if (n == 4) {
                b.crash();
                c.crash();
                expectKind(a.call(RequestKind::SubmitRange, bytes), ReplyKind::Pending, "durable intent without quorum");
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                require(a.status().accepted_frontier == 3, "one validator cannot finalize range");
                a.crash();
                a.start(false);
                b.start(false);
                c.start(false);
                configure();
                // Only status polling follows: recovery must resume without miner resubmission.
            } else if (n == 20) {
                ingress.dropReplyFor(RequestKind::SubmitRange);
                b.dropReplyFor(RequestKind::DeliverRange);
                lostReply(ingress, RequestKind::SubmitRange, bytes);
            } else expectKind(ingress.call(RequestKind::SubmitRange, bytes), ReplyKind::Pending, "miner submits one range");
            const auto hash = rangeSubjectHash(proposal);
            synchronized(last, hash);
            const auto receipt = ingress.call(RequestKind::SubmitRange, bytes);
            expectKind(receipt, ReplyKind::AlreadyAccepted, "miner observes completed range");
            CompositeRange certified;
            require(decodeRange(receipt.payload, certified, f.error) && rangeSubjectHash(certified) == hash,
                    "exact range certificate returned");
            verifyVotes(certified.votes, hash, proposal.previous_block, n, f);
            if (!first_range) first_range = certified;
            for (std::size_t i = 0; i < certified.factors.size(); ++i) {
                CompositeProof proof;
                proof.m = certified.start + i;
                proof.d = certified.factors[i].divisor;
                proof.e = certified.factors[i].cofactor;
                proof.provider_address = certified.provider;
                pending.push_back(std::move(proof));
            }
            range_tip = hash;
            n = last + 1;
            continue;
        }
        auto proposal = f.block(expected, pending, n, f.miner);
        proposal.votes.clear();
        const auto bytes = encodeBlock(proposal);
        if (n == 3) {
            // Valid signatures from the wrong configured identity must not count.
            a.configurePeers({{f.validators[1].address, c.port()}, {f.validators[2].address, b.port()}});
            expectKind(a.call(RequestKind::SubmitBlock, bytes), ReplyKind::Pending, "first miner prime submission");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            require(a.status().finalized_frontier == 2, "wrong endpoint identity cannot provide quorum");
            configure();
        } else {
            if (n == 11) a.armPeerFailure(PeerFailure::BeforeCertificatePublish);
            if (n == 19) a.armPeerFailure(PeerFailure::AfterCertificatePublish);
            if (n == 23) {
                a.armPeerFailure(PeerFailure::AfterLocalApply);
                b.dropReplyFor(RequestKind::DeliverBlock);
            }
            expectKind(ingress.call(RequestKind::SubmitBlock, bytes), ReplyKind::Pending, "miner submits one prime");
            if (n == 11 || n == 19 || n == 23) {
                a.expectExit(71);
                a.start(false);
                configure();
            }
        }
        eventually([&] { return ingress.status().finalized_frontier == n; }, "autonomous prime finalization");
        const auto receipt = ingress.call(RequestKind::SubmitBlock, bytes);
        expectKind(receipt, ReplyKind::AlreadyInstalled, "miner observes completed prime");
        PrimeBlock block;
        require(decodeBlock(receipt.payload, block, f.error) && blockSubjectHash(block) == blockSubjectHash(proposal),
                "exact block certificate returned");
        verifyVotes(block.votes, blockSubjectHash(block), block.previous_hash, n, f);
        require(expected.append(block, f.error), "replay worker-certified prime block");
        pending.clear();
        range_tip = {};
        if (n == 17) {
            c.start(false);
            require(c.status().finalized_frontier == 7, "offline peer retains old frontier");
            configure();
        }
        synchronized(n, {});
        ++n;
    }
    require(expected.blockCount() == 26, "26 prime blocks through 101");
    // Historical duplicate delivery must acknowledge without changing balances or the open interval.
    for (auto* worker : workers) {
        expectKind(worker->call(RequestKind::DeliverRange, encodeRange(*first_range)), ReplyKind::AlreadyAccepted,
                   "retained receipt acknowledges historical range");
        auto unsigned_range = *first_range;
        unsigned_range.votes.clear();
        expectKind(worker->call(RequestKind::DeliverRange, encodeRange(unsigned_range)), ReplyKind::Rejected,
                   "historical duplicate delivery still requires quorum");
    }
    synchronized(101, {});
    for (auto* worker : workers) worker->shutdown();
    for (auto* worker : workers) worker->start(false);
    configure();
    synchronized(101, {});
    for (auto* worker : workers) {
        worker->shutdown();
        PrimeBlockReplay disk(f.addresses);
        for (std::size_t height = 0; height < expected.blockCount(); ++height) {
            PrimeBlock block;
            require(decodeBlock(readFile(worker->directory() / ("block-" + std::to_string(height) + ".dat")), block, f.error),
                    "decode persisted prime block");
            require(disk.append(block, f.error), "independent disk replay");
        }
        compare(disk, expected, f);
        require(math::makePrattProof(101, disk).has_value(), "Pratt evidence reconstructed from compact blocks");
    }
    corrupt(a.directory() / "peer-outbox.dat");
    bool refused = false;
    try { a.start(false); } catch (const std::exception&) { refused = true; }
    require(refused && a.pid() < 0, "corrupt outbox fails closed and child reaped");
    fs::rename(b.directory() / "peer-outbox.dat", b.directory() / "peer-outbox.saved");
    refused = false;
    try { b.start(false); } catch (const std::exception&) { refused = true; }
    require(refused && b.pid() < 0, "missing outbox fails closed and child reaped");
    // Three different prime providers lock three different subjects before peers connect.
    Worker d(executable, directory / "conflict-a", f, 0, true),
           e(executable, directory / "conflict-b", f, 1, true),
           g(executable, directory / "conflict-c", f, 2, true);
    std::array<Worker*, 3> conflicting{&d, &e, &g};
    PrimeBlockReplay genesis(f.addresses);
    require(genesis.append(f.genesis, f.error), "conflict fixture genesis");
    std::array<PrimeBlock, 3> proposals{f.block(genesis, {}, 3, f.alice), f.block(genesis, {}, 3, f.bob),
                                       f.block(genesis, {}, 3, f.miner)};
    for (std::size_t i = 0; i < conflicting.size(); ++i) {
        proposals[i].votes.clear();
        conflicting[i]->start(true);
        expectKind(conflicting[i]->call(RequestKind::SubmitBlock, encodeBlock(proposals[i])), ReplyKind::Pending,
                   "conflicting local intent persisted");
    }
    for (std::size_t i = 0; i < conflicting.size(); ++i) {
        std::vector<LocalPeer> peers;
        for (std::size_t j = 0; j < conflicting.size(); ++j)
            if (i != j) peers.push_back({f.validators[j].address, conflicting[j]->port()});
        conflicting[i]->configurePeers(peers);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    for (std::size_t i = 0; i < conflicting.size(); ++i) {
        require(conflicting[i]->status().finalized_frontier == 2, "conflicting proposals do not invent quorum");
        expectKind(conflicting[i]->call(RequestKind::SubmitBlock, encodeBlock(proposals[(i + 1) % 3])),
                   ReplyKind::Rejected, "retry does not replace existing lock");
        conflicting[i]->crash();
        conflicting[i]->start(false);
        expectKind(conflicting[i]->call(RequestKind::SubmitBlock, encodeBlock(proposals[(i + 1) % 3])),
                   ReplyKind::Rejected, "restart does not clear conflicting lock");
        require(conflicting[i]->status().finalized_frontier == 2, "restart remains at agreed frontier");
        conflicting[i]->shutdown();
    }
    std::cout << "autonomous_peer_coordination: PASS\nchecks: " << checks
              << "\nvalidator_processes: 3\nprime_blocks: 26\nfrontier: 101\n"
              << "recovery: durable intent, offline peer, lost replies, three publication failures\n"
              << "artifacts: " << directory.string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--worker" && std::string(argv[2]) == "3") return workerMain(3);
        if (argc != 2) throw std::runtime_error("usage: primechain-peer-coordination-tests <artifact-root>");
        return run(argv[1], fs::canonical("/proc/self/exe").string());
    } catch (const std::exception& exception) {
        std::cerr << "peer coordination test failed: " << exception.what() << '\n';
        return 1;
    }
}
