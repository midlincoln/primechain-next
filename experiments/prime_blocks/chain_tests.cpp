#include "chain_test_fixtures.hpp"

#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace primechain::experiments::test_support;

int run(const fs::path& root) {
    fs::create_directories(root);
    auto name = (root / "continuous-XXXXXX").string();
    require(mkdtemp(name.data()) != nullptr, "create private fixture root");
    const fs::path directory(name);
    Fixture f;
    auto a = f.open(directory / "a", true, 0);
    auto b = f.open(directory / "b", true, 1);
    auto c = f.open(directory / "c", true, 2);
    {
        ContinuousChain second((directory / "a").string(), f.genesis, f.addresses, f.validators[0].address, f.policy);
        require(!second.open(false, f.error), "second chain writer rejected");
    }
    PrimeBlockReplay expected(f.addresses);
    require(expected.append(f.genesis, f.error), "reference genesis");
    std::vector<PrimeBlock> blocks{f.genesis};
    for (PrimeValue n = 3; n <= 101;) {
        if (!math::isPrime(n)) {
            auto last = n;
            while (last + 1 <= 101 && last - n + 1 < f.policy.max_members && !math::isPrime(last + 1)) ++last;
            auto range = f.range(*a, n, last, (n / 2) % 2 ? f.alice : f.bob);
            range.votes.clear();
            const auto va = a->voteForRange(range, f.validators[0].key, f.error);
            const auto vb = b->voteForRange(range, f.validators[1].key, f.error);
            require(va && vb, "multi-block range votes");
            range.votes = {*va, *vb};
            sortVotes(range.votes);
            for (auto* chain : {a.get(), b.get(), c.get()})
                require(chain->accept(range, f.error) == RangeAcceptance::Accepted, "accept next interval range");
            c.reset();
            c = f.open(directory / "c", false, 2);
            require(c->acceptedFrontier() == last && c->history().frontier() == expected.frontier(),
                    "restart preserves open work separately from finalized history");
            n = last + 1;
            continue;
        }
        auto block = f.block(a->history(), a->composites(), n, f.miner);
        block.votes.clear();
        const auto va = a->voteForBlock(block, f.validators[0].key, f.error);
        const auto vb = b->voteForBlock(block, f.validators[1].key, f.error);
        require(va && vb, "multi-block closing votes");
        b.reset();
        b = f.open(directory / "b", false, 1);
        const auto conflicting = f.block(b->history(), b->composites(), n, f.bob);
        require(!b->voteForBlock(conflicting, f.validators[1].key, f.error), "restart retains current closing lock");
        require(b->history().frontier() == expected.frontier(), "unsealed closing vote never installs block");
        block.votes = {*va, *vb};
        sortVotes(block.votes);
        for (auto* chain : {a.get(), b.get(), c.get()}) {
            require(chain->install(block, f.error) == BlockInstallation::Installed, "install and rotate interval: " + f.error);
            require(chain->ready() && chain->composites().empty() && chain->acceptedRangeTip() == Hash256{} &&
                    chain->acceptedFrontier() == n, "new interval starts empty at installed prime");
            require(chain->install(block, f.error) == BlockInstallation::AlreadyInstalled, "repeat installation idempotent");
        }
        require(expected.append(block, f.error), "reference replay next block");
        blocks.push_back(block);
        a.reset();
        a = f.open(directory / "a", false, 0);
        compare(a->history(), expected, f);
        ++n;
    }
    require(expected.blockCount() == 26 && expected.frontier() == 101, "26 prime blocks through 101");
    require(a->history().balance(f.miner.address, 3) == 900000, "empty interval 2 to 3 preserves reward rule");
    require(math::makePrattProof(101, a->history()).has_value(), "proof construction across installed intervals");
    require(fs::exists(directory / "a/interval-1.journal") && fs::exists(directory / "a/interval-25.journal.block"),
            "prior journals and sealed certificates retained");
    for (const auto& old : blocks)
        require(a->install(old, f.error) == BlockInstallation::AlreadyInstalled, "historical retry never rotates current journal");
    compare(a->history(), expected, f);
    auto forged = blocks.back();
    forged.votes.clear();
    require(a->install(forged, f.error) == BlockInstallation::Rejected, "retry still requires valid quorum");
    forged = blocks.back();
    forged.prime.witness = 0;
    require(a->install(forged, f.error) == BlockInstallation::Rejected, "conflicting historical retry rejected");

    // A fresh reader needs only compact blocks and trusted validators, not range journals.
    PrimeBlockReplay disk_replay(f.addresses);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        PrimeBlock block;
        require(decodeBlock(readFile(directory / "a" / ("block-" + std::to_string(i) + ".dat")), block, f.error),
                "decode installed compact block");
        require(disk_replay.append(block, f.error), "independent compact replay");
    }
    compare(disk_replay, expected, f);

    const std::vector<TransitionFailure> failures{
        TransitionFailure::AfterSeal, TransitionFailure::BeforeBlockPublish, TransitionFailure::AfterBlockPublish,
        TransitionFailure::BeforeCheckpoint, TransitionFailure::AfterCheckpoint, TransitionFailure::BeforeJournalOpen,
        TransitionFailure::AfterJournalOpen, TransitionFailure::BeforeActivation, TransitionFailure::AfterActivation};
    for (const bool abrupt_exit : {false, true}) {
        for (const auto point : failures) {
            const auto path = directory / ("failure-" + std::to_string(abrupt_exit) + "-" + std::to_string(static_cast<int>(point)));
            auto chain = f.open(path, true);
            const auto three = f.block(chain->history(), {}, 3, f.miner);
            require(chain->install(three, f.error) == BlockInstallation::Installed, "transition setup prime 3");
            auto four = f.range(*chain, 4, 4, f.alice);
            require(chain->accept(four, f.error) == RangeAcceptance::Accepted, "transition setup composite 4");
            const auto five = f.block(chain->history(), chain->composites(), 5, f.miner);
            require(chain->voteForBlock(five, f.validators[0].key, f.error).has_value(), "transition setup closing lock");
            if (abrupt_exit) {
                chain.reset();
                const auto pid = fork();
                require(pid >= 0, "fork crash fixture");
                if (pid == 0) {
                    try {
                        auto child = f.open(path, false);
                        child->failNextTransitionForTest(point);
                        const auto result = child->install(five, f.error);
                        // Simulate process death without running destructors after the interrupted operation.
                        _exit(result == BlockInstallation::RecoveryRequired && !child->ready() ? 0 : 1);
                    } catch (...) { _exit(2); }
                }
                int status = 0;
                require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "child terminated at injected transition without cleanup");
            } else {
                chain->failNextTransitionForTest(point);
                require(chain->install(five, f.error) == BlockInstallation::RecoveryRequired && !chain->ready(),
                        "interrupted transition requires recovery");
                require(!chain->voteForBlock(five, f.validators[0].key, f.error), "interrupted chain cannot sign");
                require(chain->accept(four, f.error) == RangeAcceptance::RecoveryRequired, "interrupted chain cannot accept");
                chain.reset();
            }
            chain = f.open(path, false);
            require(chain->history().frontier() == 5 && chain->history().blockCount() == 3 &&
                    chain->composites().empty(), "restart completes sealed block and next interval exactly once");
            require(chain->history().supply(5) == node::kAssetMicroUnits &&
                    chain->history().balance(f.alice.address, 5) == 450000 &&
                    chain->history().balance(f.miner.address, 5) == 450000, "recovery preserves composite reward");
            require(chain->install(five, f.error) == BlockInstallation::AlreadyInstalled, "retry resolves lost install acknowledgement");
            auto six = f.range(*chain, 6, 6, f.alice);
            const auto other_six = f.range(*chain, 6, 6, f.bob);
            require(chain->voteForRange(six, f.validators[0].key, f.error).has_value(), "new interval can vote");
            require(chain->install(five, f.error) == BlockInstallation::AlreadyInstalled, "old retry during new pending vote");
            require(!chain->voteForRange(other_six, f.validators[0].key, f.error), "old retry cannot erase new range lock");
            chain.reset();
            chain = f.open(path, false);
            require(!chain->voteForRange(other_six, f.validators[0].key, f.error), "new interval lock persists across restart");
            require(chain->accept(six, f.error) == RangeAcceptance::Accepted, "can continue mining after recovery");
            const auto seven = f.block(chain->history(), chain->composites(), 7, f.miner);
            require(chain->install(seven, f.error) == BlockInstallation::Installed, "next block works after interrupted transition");
        }
    }

    // Missing active state is never interpreted as permission to recreate a signing journal.
    for (const std::string target : {"checkpoint.dat", "block-0.dat", "interval-1.journal"}) {
        for (const bool missing : {false, true}) {
            const auto path = directory / ("damage-" + target + std::to_string(missing));
            auto chain = f.open(path, true);
            chain.reset();
            if (missing) require(fs::remove(path / target), "remove fixture state");
            else corrupt(path / target);
            ContinuousChain damaged(path.string(), f.genesis, f.addresses, f.validators[0].address, f.policy);
            require(!damaged.open(false, f.error) && !damaged.ready(), "missing/corrupt committed state fails closed");
            require(!damaged.voteForBlock(blocks[1], f.validators[0].key, f.error), "damaged chain cannot sign");
            if (missing) require(!fs::exists(path / target), "missing active file not silently recreated");
        }
    }
    const auto config_path = directory / "configuration";
    auto config = f.open(config_path, true);
    config.reset();
    {
        ContinuousChain changed(config_path.string(), f.genesis, f.addresses, f.validators[0].address, RangePolicy{1});
        require(!changed.open(false, f.error), "policy cannot change by reopening chain");
    }
    {
        ContinuousChain changed(config_path.string(), f.genesis, f.addresses, f.validators[1].address, f.policy);
        require(!changed.open(false, f.error), "chain directory bound to local validator");
    }
    {
        ContinuousChain recreate(config_path.string(), f.genesis, f.addresses, f.validators[0].address, f.policy);
        require(!recreate.open(true, f.error), "creation refuses existing directory");
    }
    const auto orphan_path = directory / "corrupt-uncommitted-block";
    auto orphan = f.open(orphan_path, true);
    const auto three = f.block(orphan->history(), {}, 3, f.miner);
    orphan->failNextTransitionForTest(TransitionFailure::AfterBlockPublish);
    require(orphan->install(three, f.error) == BlockInstallation::RecoveryRequired, "uncommitted block fixture");
    orphan.reset();
    corrupt(orphan_path / "block-1.dat");
    ContinuousChain conflict(orphan_path.string(), f.genesis, f.addresses, f.validators[0].address, f.policy);
    require(!conflict.open(false, f.error), "corrupted unpublished block not silently overwritten");
    std::cout << "continuous_chain_tests: PASS\nchecks: " << checks
              << "\nprime_blocks: 26\nfrontier: 101\ntransition_failure_points: " << failures.size()
              << "\nprocess_exit_cases: " << failures.size() << "\nartifacts: " << directory.string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: primechain-continuous-chain-tests <artifact-root>");
        return run(argv[1]);
    } catch (const std::exception& exception) {
        std::cerr << "continuous chain test failed: " << exception.what() << '\n';
        return 1;
    }
}
