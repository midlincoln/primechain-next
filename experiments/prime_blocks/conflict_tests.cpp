#include "chain_test_fixtures.hpp"

#include <array>
#include <bitset>

namespace {
using namespace primechain::experiments::test_support;

std::unique_ptr<RangeJournal> journal(const fs::path& path, const PrimeBlockReplay& history,
                                    Fixture& f, std::size_t signer, bool create) {
    auto result = std::make_unique<RangeJournal>(path.string(), history, f.addresses,
                                                f.validators[signer].address, f.policy);
    const bool opened = result->open(create, f.error);
    require(opened, "open conflict fixture: " + f.error);
    return result;
}

protocol::ValidatorVoteV0 uncheckedVote(const Identity& signer, const Hash256& subject) {
    std::string error;
    auto vote = protocol::makeSignedValidatorVote(signer.address, signer.key.public_key,
                                                 signer.key.private_key, subject, 1, error);
    require(!vote.signature.empty(), "sign deliberate adversarial vote: " + error);
    return vote;
}

PrimeBlockReplay prefix(Fixture& f, PrimeValue limit) {
    PrimeBlockReplay history(f.addresses);
    require(history.append(f.genesis, f.error), "conflict fixture genesis");
    std::vector<CompositeProof> composites;
    for (PrimeValue n = 3; n <= limit; ++n) {
        if (!math::isPrime(n)) {
            const auto proof = math::makeCompositeProof(n, f.alice.address);
            require(proof.has_value(), "fixture composite proof");
            composites.push_back(*proof);
        } else {
            const auto block = f.block(history, composites, n, f.miner);
            require(history.append(block, f.error), "fixture prefix replay");
            composites.clear();
        }
    }
    require(composites.empty(), "fixture prefix ends at prime");
    return history;
}

void splitLocks(const fs::path& directory, Fixture& f) {
    const auto history = prefix(f, 7);
    std::array<CompositeRange, 3> ranges{f.range(history, 8, 8, f.alice), f.range(history, 8, 9, f.bob),
                                       f.range(history, 8, 9, f.miner)};
    for (auto& range : ranges) range.votes.clear();
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto path = directory / ("split-range-" + std::to_string(i));
        auto state = journal(path, history, f, i, true);
        require(state->voteFor(ranges[i], f.validators[i].key, f.error).has_value(), "first range vote locks");
        for (int restart = 0; restart < 2; ++restart) {
            if (restart) { state.reset(); state = journal(path, history, f, i, false); }
            for (std::size_t j = 0; j < ranges.size(); ++j)
                require(state->voteFor(ranges[j], f.validators[i].key, f.error).has_value() == (i == j),
                        "overlapping range/provider conflict remains locked across restart");
            require(state->acceptedFrontier() == 7 && state->rangeCount() == 0,
                    "three distinct singleton votes cannot advance range frontier");
        }
    }
    const auto genesis = prefix(f, 2);
    std::array<PrimeBlock, 3> blocks{f.block(genesis, {}, 3, f.alice), f.block(genesis, {}, 3, f.bob),
                                   f.block(genesis, {}, 3, f.miner)};
    for (auto& block : blocks) block.votes.clear();
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto path = directory / ("split-prime-" + std::to_string(i));
        auto state = journal(path, genesis, f, i, true);
        require(state->voteForBlock(blocks[i], f.validators[i].key, f.error).has_value(), "first closing vote locks");
        state.reset();
        state = journal(path, genesis, f, i, false);
        for (std::size_t j = 0; j < blocks.size(); ++j)
            require(state->voteForBlock(blocks[j], f.validators[i].key, f.error).has_value() == (i == j),
                    "prime-provider conflict remains locked across restart");
        require(!state->closed(), "split prime votes do not finalize");
    }
}

void minorityAdoptsCertificate(const fs::path& directory, Fixture& f) {
    const auto history = prefix(f, 7);
    auto minority = f.range(history, 8, 8, f.alice);
    auto winner = f.range(history, 8, 9, f.bob);
    minority.votes.clear();
    winner.votes.clear();
    std::array<std::unique_ptr<RangeJournal>, 3> nodes;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        nodes[i] = journal(directory / ("minority-" + std::to_string(i)), history, f, i, true);
    require(nodes[0]->voteFor(minority, f.validators[0].key, f.error).has_value(), "minority locks other range");
    for (std::size_t i : {1, 2}) {
        const auto vote = nodes[i]->voteFor(winner, f.validators[i].key, f.error);
        require(vote.has_value(), "majority signs winner");
        winner.votes.push_back(*vote);
    }
    sortVotes(winner.votes);
    require(!nodes[0]->voteFor(winner, f.validators[0].key, f.error), "minority does not sign twice");
    for (auto& state : nodes) {
        require(state->accept(winner, f.error) == RangeAcceptance::Accepted, "certified winner resolves minority vote");
        require(state->acceptedFrontier() == 9 && !state->pendingRange(), "range certificate advances and clears lock");
    }
    require(nodes[0]->accept(winner, f.error) == RangeAcceptance::AlreadyAccepted, "delivery retry is idempotent");
    require(nodes[0]->composites().front().provider_address == f.bob.address, "winner attribution preserved");
}

void oneEquivocator(const fs::path& directory, Fixture& f) {
    // A and C obey the journal. Only B bypasses it and signs both subjects.
    const auto history = prefix(f, 2);
    auto x = f.block(history, {}, 3, f.alice), y = f.block(history, {}, 3, f.bob);
    x.votes.clear();
    y.votes.clear();
    auto a = journal(directory / "equivocation-prime-a", history, f, 0, true);
    auto c = journal(directory / "equivocation-prime-c", history, f, 2, true);
    const auto ax = a->voteForBlock(x, f.validators[0].key, f.error);
    const auto cy = c->voteForBlock(y, f.validators[2].key, f.error);
    require(ax && cy, "honest validators sign only their own proposals");
    require(!a->voteForBlock(y, f.validators[0].key, f.error) && !c->voteForBlock(x, f.validators[2].key, f.error),
            "honest validators reject equivocation");
    x.votes = {*ax, uncheckedVote(f.validators[1], blockSubjectHash(x))};
    y.votes = {*cy, uncheckedVote(f.validators[1], blockSubjectHash(y))};
    sortVotes(x.votes);
    sortVotes(y.votes);
    require(a->seal(x, f.error) && c->seal(y, f.error), "counterexample: honest journals seal conflicting certificates");
    auto branch_x = history, branch_y = history;
    require(branch_x.append(x, f.error) && branch_y.append(y, f.error), "counterexample: both branches replay successfully");
    require(branch_x.frontier() == branch_y.frontier() && branch_x.tip() != branch_y.tip(), "same height has conflicting tips");
    require(branch_x.balance(f.alice.address, 3) != branch_y.balance(f.alice.address, 3), "fork changes prime rewards");

    const auto range_history = prefix(f, 7);
    auto rx = f.range(range_history, 8, 8, f.alice), ry = f.range(range_history, 8, 9, f.bob);
    rx.votes.clear();
    ry.votes.clear();
    a = journal(directory / "equivocation-range-a", range_history, f, 0, true);
    c = journal(directory / "equivocation-range-c", range_history, f, 2, true);
    const auto ar = a->voteFor(rx, f.validators[0].key, f.error);
    const auto cr = c->voteFor(ry, f.validators[2].key, f.error);
    require(ar && cr, "honest range votes");
    rx.votes = {*ar, uncheckedVote(f.validators[1], rangeSubjectHash(rx))};
    ry.votes = {*cr, uncheckedVote(f.validators[1], rangeSubjectHash(ry))};
    sortVotes(rx.votes);
    sortVotes(ry.votes);
    require(a->accept(rx, f.error) == RangeAcceptance::Accepted && c->accept(ry, f.error) == RangeAcceptance::Accepted,
            "counterexample: one equivocator certifies overlapping ranges");
    require(a->acceptedFrontier() == 8 && c->acceptedFrontier() == 9 &&
            a->composites().front().provider_address != c->composites().front().provider_address,
            "overlapping certificates disagree on frontier and attribution");

    auto duplicate = x;
    duplicate.votes = {*ax, uncheckedVote(f.validators[0], blockSubjectHash(x))};
    auto replay = history;
    require(!replay.append(duplicate, f.error), "two signatures from one identity are not a quorum");
    duplicate = x;
    duplicate.votes.front().signature.front() ^= 1;
    require(!replay.append(duplicate, f.error), "invalid signature is not counted");
}

void quorumBoundaries() {
    // Exact enumeration of signer sets, not a distributed-protocol proof.
    for (unsigned n = 1; n <= 8; ++n) {
        for (unsigned q = 1; q <= n; ++q) {
            unsigned minimum = n;
            for (unsigned a = 1; a < (1u << n); ++a) {
                if (std::bitset<8>(a).count() != q) continue;
                for (unsigned b = 1; b < (1u << n); ++b)
                    if (std::bitset<8>(b).count() == q)
                        minimum = std::min(minimum, static_cast<unsigned>(std::bitset<8>(a & b).count()));
            }
            require(minimum == (2 * q > n ? 2 * q - n : 0), "exhaustive quorum intersection bound");
            for (unsigned f = 0; f < n; ++f) {
                const bool honest_overlap = minimum > f;
                const bool progress_without_faulty = q <= n - f;
                if (n == 3 && f == 1) require(!(honest_overlap && progress_without_faulty), "no 3-node threshold tolerates one Byzantine withholding signer");
                if (n == 4 && q == 3 && f == 1) require(honest_overlap && progress_without_faulty, "3-of-4 meets necessary quorum bounds");
                if (n == 3 && q == 2 && f == 0) require(honest_overlap, "2-of-3 intersects with honest-only signers");
            }
        }
    }
    // An abstract unsafe timeout policy: B forgets its X vote; X may already be decided at A.
    std::array<int, 3> lock{1, 1, 0};
    const bool x_certified = lock[0] == 1 && lock[1] == 1;
    lock[1] = 0;
    lock[1] = lock[2] = 2;
    require(x_certified && lock[1] == 2 && lock[2] == 2,
            "counterexample: timeout-based lock erasure allows two decisions even without a malicious key");
}

int run(const fs::path& root) {
    fs::create_directories(root);
    auto name = (root / "conflicts-XXXXXX").string();
    require(mkdtemp(name.data()) != nullptr, "create private conflict-test directory");
    const auto directory = fs::absolute(name);
    Fixture f;
    splitLocks(directory, f);
    minorityAdoptsCertificate(directory, f);
    oneEquivocator(directory, f);
    quorumBoundaries();
    std::cout << "conflict_safety_boundaries: PASS\nchecks: " << checks
              << "\nexpected counterexamples: split locks, 2-of-3 equivocation, abstract timeout lock erasure\n"
              << "NOT a conflict-recovery or Byzantine-safety implementation\nartifacts: " << directory.string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: primechain-conflict-tests <artifact-root>");
        return run(argv[1]);
    } catch (const std::exception& exception) {
        std::cerr << "conflict test failed: " << exception.what() << '\n';
        return 1;
    }
}
