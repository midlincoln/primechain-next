#include "validator_process_test.hpp"

namespace {
using namespace primechain::experiments::process_test;

struct Interval {
    std::vector<CompositeRange> ranges;
    PrimeBlock block;
};

void expectKind(const ValidatorReply& reply, ReplyKind expected, const std::string& label) {
    require(reply.kind == expected, label + ": " + reply.error);
}

void verifyVotes(const std::vector<protocol::ValidatorVoteV0>& votes, const Hash256& subject,
                 const Hash256& previous, PrimeValue integer, const Fixture& fixture) {
    protocol::FinalizationProofV0 proof;
    proof.rule = "fixed-2-of-3-mldsa65-v2";
    proof.votes = votes;
    std::string error;
    auto validators = fixture.addresses;
    std::sort(validators.begin(), validators.end());
    const bool valid = protocol::verifyRecordFinalization(proof, subject, previous, integer, validators, error);
    require(valid, "verify actual process quorum: " + error);
}

protocol::ValidatorVoteV0 rangeVote(const ValidatorReply& reply, const CompositeRange& proposal,
                                   const Address& signer) {
    CompositeRange voted;
    std::string error;
    require(reply.kind == ReplyKind::Vote && decodeRange(reply.payload, voted, error) && voted.votes.size() == 1 &&
            rangeSubjectHash(voted) == rangeSubjectHash(proposal) && voted.votes[0].validator_address == signer,
            "worker returned vote for exact range and own identity: " + reply.error);
    return voted.votes[0];
}

protocol::ValidatorVoteV0 blockVote(const ValidatorReply& reply, const PrimeBlock& proposal,
                                   const Address& signer) {
    PrimeBlock voted;
    std::string error;
    require(reply.kind == ReplyKind::Vote && decodeBlock(reply.payload, voted, error) && voted.votes.size() == 1 &&
            blockSubjectHash(voted) == blockSubjectHash(proposal) && voted.votes[0].validator_address == signer,
            "worker returned vote for exact block and own identity: " + reply.error);
    return voted.votes[0];
}

void expectLostReply(Worker& worker, RequestKind kind, const protocol::Bytes& payload) {
    bool disconnected = false;
    try { (void)worker.call(kind, payload); }
    catch (const std::exception&) { disconnected = true; }
    require(disconnected, "injected interruption must lose TCP reply");
}

void checkStatus(const Worker& worker, const PrimeBlockReplay& history, PrimeValue accepted,
                 const Hash256& range_tip, const Fixture& f) {
    const auto status = worker.status();
    require(status.ready && status.max_range_members == f.policy.max_members &&
            status.finalized_frontier == history.frontier() && status.accepted_frontier == accepted &&
            status.block_count == history.blockCount() && status.block_tip == history.tip() && status.range_tip == range_tip,
            "worker status agrees with finalized and open state");
}

int run(const fs::path& root, const std::string& executable) {
    fs::create_directories(root);
    auto name = (root / "processes-XXXXXX").string();
    require(mkdtemp(name.data()) != nullptr, "create private process fixture root");
    const fs::path directory = fs::absolute(name);
    Fixture f;
    Worker a(executable, directory / "a", f, 0), b(executable, directory / "b", f, 1),
           c(executable, directory / "c", f, 2);
    a.start(true);
    b.start(true);
    c.start(true);
    require(a.pid() != b.pid() && b.pid() != c.pid() && a.pid() != c.pid(), "three distinct validator processes");
    require(a.port() != b.port() && b.port() != c.port() && a.port() != c.port(), "three distinct localhost ports");
    std::cout << "workers: " << a.pid() << ' ' << b.pid() << ' ' << c.pid()
              << "\nloopback_ports: " << a.port() << ' ' << b.port() << ' ' << c.port() << std::endl;
    PrimeBlockReplay expected(f.addresses);
    require(expected.append(f.genesis, f.error), "reference genesis");
    for (const auto* worker : {&a, &b, &c}) checkStatus(*worker, expected, 2, {}, f);
    expectKind(a.call(RequestKind::Status), ReplyKind::Rejected, "chain rejects ambiguous interval status");
    expectKind(a.call(RequestKind::SealBlock, encodeBlock(f.genesis)), ReplyKind::Rejected,
               "chain requires install rather than seal operation");
    a.malformedFrame(true);
    a.malformedFrame(false);
    auto malformed = encodeRequest(RequestKind::ChainStatus);
    malformed.push_back(0);
    expectKind(a.raw(malformed), ReplyKind::Rejected, "reject trailing request bytes");
    malformed = encodeRequest(RequestKind::ChainStatus);
    malformed[8] = 255;
    expectKind(a.raw(malformed), ReplyKind::Rejected, "reject unknown request kind");
    checkStatus(a, expected, 2, {}, f);
    auto encoded_status = a.call(RequestKind::ChainStatus).payload;
    ChainStatus decoded_status;
    encoded_status.back() = 2;
    require(!decodeChainStatus(encoded_status, decoded_status, f.error), "status readiness must be a boolean");
    encoded_status = a.call(RequestKind::ChainStatus).payload;
    encoded_status.push_back(0);
    require(!decodeChainStatus(encoded_status, decoded_status, f.error), "reject trailing status payload");

    bool c_online = true;
    std::vector<Interval> intervals;
    std::vector<CompositeRange> pending_ranges;
    std::vector<CompositeProof> pending;
    Hash256 range_tip{};
    for (PrimeValue n = 3; n <= 101;) {
        if (n == 8) { c.crash(); c_online = false; }
        if (!math::isPrime(n)) {
            auto last = n;
            while (last + 1 <= 101 && last - n + 1 < f.policy.max_members && !math::isPrime(last + 1)) ++last;
            const auto& provider = (n / 2) % 2 ? f.alice : f.bob;
            auto range = f.range(expected, n, last, provider, range_tip);
            range.votes.clear();
            if (n == 8) {
                auto oversized = f.range(expected, 8, 10, f.alice, range_tip);
                oversized.votes.clear();
                expectKind(a.call(RequestKind::VoteRange, encodeRange(oversized)), ReplyKind::Rejected,
                           "worker enforces configured range limit");
                auto invalid = range;
                invalid.factors.back() = {1, last};
                const auto signature = crypto::signProtocolMessage(provider.key.private_key, rangeSigningPayload(invalid), f.error);
                require(signature.has_value(), "sign invalid math fixture");
                invalid.signature = *signature;
                expectKind(a.call(RequestKind::VoteRange, encodeRange(invalid)), ReplyKind::Rejected,
                           "one invalid member rejects atomic range");
                checkStatus(a, expected, n - 1, range_tip, f);
            }
            auto va = rangeVote(a.call(RequestKind::VoteRange, encodeRange(range)), range, f.validators[0].address);
            if (n == 8) {
                a.crash();
                a.start(false);
                auto conflicting = f.range(expected, n, last, range.provider == f.alice.address ? f.bob : f.alice, range_tip);
                conflicting.votes.clear();
                expectKind(a.call(RequestKind::VoteRange, encodeRange(conflicting)), ReplyKind::Rejected,
                           "range lock survives SIGKILL and exec restart");
                va = rangeVote(a.call(RequestKind::VoteRange, encodeRange(range)), range, f.validators[0].address);
            }
            const auto vb = n == 30
                ? rangeVote(c.call(RequestKind::VoteRange, encodeRange(range)), range, f.validators[2].address)
                : rangeVote(b.call(RequestKind::VoteRange, encodeRange(range)), range, f.validators[1].address);
            range.votes = {va};
            if (n == 4) expectKind(a.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::Rejected,
                                  "single range vote is not a quorum");
            range.votes.push_back(vb);
            sortVotes(range.votes);
            verifyVotes(range.votes, rangeSubjectHash(range), range.previous_block, range.start, f);
            expectKind(a.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::Accepted, "A accepts range");
            if (n == 20) {
                b.dropNextReply();
                expectLostReply(b, RequestKind::AcceptRange, encodeRange(range));
                expectKind(b.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::AlreadyAccepted,
                           "lost acceptance reply resolves without duplicate work");
            } else expectKind(b.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::Accepted, "B accepts range");
            if (c_online) expectKind(c.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::Accepted, "C accepts range");
            expectKind(a.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::AlreadyAccepted, "duplicate range idempotent");
            range_tip = rangeSubjectHash(range);
            for (std::size_t i = 0; i < range.factors.size(); ++i) {
                CompositeProof proof;
                proof.m = range.start + i;
                proof.d = range.factors[i].divisor;
                proof.e = range.factors[i].cofactor;
                proof.provider_address = range.provider;
                pending.push_back(std::move(proof));
            }
            pending_ranges.push_back(range);
            checkStatus(a, expected, last, range_tip, f);
            checkStatus(b, expected, last, range_tip, f);
            n = last + 1;
            continue;
        }
        auto block = f.block(expected, pending, n, f.miner);
        block.votes.clear();
        const auto va = blockVote(a.call(RequestKind::VoteBlock, encodeBlock(block)), block, f.validators[0].address);
        auto vb = n == 29
            ? blockVote(c.call(RequestKind::VoteBlock, encodeBlock(block)), block, f.validators[2].address)
            : blockVote(b.call(RequestKind::VoteBlock, encodeBlock(block)), block, f.validators[1].address);
        if (n == 11) {
            b.crash();
            b.start(false);
            auto competing = f.block(expected, pending, n, f.bob);
            competing.votes.clear();
            expectKind(b.call(RequestKind::VoteBlock, encodeBlock(competing)), ReplyKind::Rejected,
                       "closing lock survives process restart");
            vb = blockVote(b.call(RequestKind::VoteBlock, encodeBlock(block)), block, f.validators[1].address);
        }
        block.votes = {va};
        if (n == 3) expectKind(a.call(RequestKind::InstallBlock, encodeBlock(block)), ReplyKind::Rejected,
                              "single closing vote cannot install block");
        block.votes.push_back(vb);
        sortVotes(block.votes);
        verifyVotes(block.votes, blockSubjectHash(block), block.previous_hash, block.prime.p, f);
        if (n == 19) {
            a.armFailure(TransitionFailure::AfterCheckpoint);
            expectLostReply(a, RequestKind::InstallBlock, encodeBlock(block));
            a.expectExit(70);
            a.start(false);
            expectKind(a.call(RequestKind::InstallBlock, encodeBlock(block)), ReplyKind::AlreadyInstalled,
                       "restart completes interrupted installation before serving requests");
        } else expectKind(a.call(RequestKind::InstallBlock, encodeBlock(block)), ReplyKind::Installed, "A installs block");
        if (n == 23) {
            b.dropNextReply();
            expectLostReply(b, RequestKind::InstallBlock, encodeBlock(block));
            expectKind(b.call(RequestKind::InstallBlock, encodeBlock(block)), ReplyKind::AlreadyInstalled,
                       "lost installation reply resolves idempotently");
        } else expectKind(b.call(RequestKind::InstallBlock, encodeBlock(block)), ReplyKind::Installed, "B installs block");
        if (c_online) expectKind(c.call(RequestKind::InstallBlock, encodeBlock(block)), ReplyKind::Installed, "C installs block");
        require(expected.append(block, f.error), "reference block replay");
        intervals.push_back({pending_ranges, block});
        pending.clear();
        pending_ranges.clear();
        range_tip = {};
        checkStatus(a, expected, n, {}, f);
        checkStatus(b, expected, n, {}, f);
        if (n == 17) {
            c.start(false);
            require(c.status().finalized_frontier == 7, "offline validator retains its prior chain");
            const auto first_missing = c.status().block_count - 1;
            for (std::size_t i = first_missing; i < intervals.size(); ++i) {
                const auto& interval = intervals[i];
                if (!interval.ranges.empty())
                    expectKind(c.call(RequestKind::InstallBlock, encodeBlock(interval.block)), ReplyKind::Rejected,
                               "lagging validator cannot close without accepted range evidence");
                for (const auto& range : interval.ranges)
                    expectKind(c.call(RequestKind::AcceptRange, encodeRange(range)), ReplyKind::Accepted,
                               "coordinator replays retained certified ranges to returning validator");
                expectKind(c.call(RequestKind::InstallBlock, encodeBlock(interval.block)), ReplyKind::Installed,
                           "returning validator installs caught-up block");
            }
            c_online = true;
        }
        if (c_online) checkStatus(c, expected, n, {}, f);
        ++n;
    }
    require(expected.blockCount() == 26, "26 prime blocks agreed through 101");
    for (auto* worker : {&a, &b, &c}) {
        expectKind(worker->call(RequestKind::InstallBlock, encodeBlock(intervals.front().block)), ReplyKind::AlreadyInstalled,
                   "historical network retry does not reset current interval");
        worker->shutdown();
        worker->start(false);
        checkStatus(*worker, expected, 101, {}, f);
        expectKind(worker->call(RequestKind::AcceptRange, encodeRange(intervals[1].ranges.front())), ReplyKind::Rejected,
                   "stale range from old interval rejected");
        worker->shutdown();
        PrimeBlockReplay disk(f.addresses);
        for (std::size_t height = 0; height < expected.blockCount(); ++height) {
            PrimeBlock block;
            require(decodeBlock(readFile(worker->directory() / ("block-" + std::to_string(height) + ".dat")), block, f.error),
                    "decode process-persisted block");
            require(disk.append(block, f.error), "independent process disk replay");
        }
        compare(disk, expected, f);
        require(math::makePrattProof(101, disk).has_value(), "process-written proof index reconstructs Pratt evidence");
    }
    require(a.pid() < 0 && b.pid() < 0 && c.pid() < 0, "all validator children reaped");
    std::cout << "local_validator_process_tests: PASS\nchecks: " << checks
              << "\nvalidator_processes: 3\nprime_blocks: 26\nfrontier: 101\n"
              << "recovery: range lock, closing lock, offline catch-up, interrupted installation, lost replies\n"
              << "artifacts: " << directory.string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--worker" && std::string(argv[2]) == "3")
            return workerMain(3);
        if (argc != 2) throw std::runtime_error("usage: primechain-local-validator-tests <artifact-root>");
        return run(argv[1], fs::canonical("/proc/self/exe").string());
    } catch (const std::exception& exception) {
        std::cerr << "local validator test failed: " << exception.what() << '\n';
        return 1;
    }
}
