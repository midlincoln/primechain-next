#include "atomic_ranges.hpp"
#include "loopback_test.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "primechain/crypto/hash.hpp"
#include "primechain/node/sequential_node.hpp"

namespace {
using namespace primechain;
using namespace primechain::experiments;
namespace fs = std::filesystem;

std::size_t checks = 0;
void require(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

struct Identity {
    crypto::SignatureKeyPair key;
    Address address;
};

Identity identity() {
    std::string error;
    auto key = crypto::generateProtocolSignatureKeyPair(error);
    require(key.has_value(), "key generation: " + error);
    return {*key, crypto::addressFromProtocolPublicKey(key->public_key)};
}

std::vector<protocol::ValidatorVoteV0> certificate(const Hash256& hash, const std::vector<Identity>& keys) {
    std::vector<protocol::ValidatorVoteV0> votes;
    for (std::size_t i = 0; i < 2; ++i) {
        std::string error;
        auto vote = protocol::makeSignedValidatorVote(keys[i].address, keys[i].key.public_key,
            keys[i].key.private_key, hash, 1, error);
        require(!vote.signature.empty(), "certificate signing: " + error);
        votes.push_back(std::move(vote));
    }
    std::sort(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
        return a.validator_address < b.validator_address;
    });
    return votes;
}

void signRange(CompositeRange& range, const Identity& miner) {
    range.provider = miner.address;
    range.public_key = miner.key.public_key;
    std::string error;
    const auto signature = crypto::signProtocolMessage(miner.key.private_key, rangeSigningPayload(range), error);
    require(signature.has_value(), "range miner signing: " + error);
    range.signature = *signature;
}

CompositeRange proposal(const PrimeBlockReplay& history, PrimeValue first, PrimeValue last,
                        const Identity& miner, Hash256 previous = {}) {
    CompositeRange range;
    range.previous_block = history.tip();
    range.previous_range = previous;
    range.start = first;
    range.end = last;
    for (auto n = first; n <= last; ++n) {
        auto proof = math::makeCompositeProof(n, miner.address);
        require(proof.has_value(), "fixture range contains a prime");
        range.factors.push_back({proof->d, proof->e});
    }
    signRange(range, miner);
    return range;
}

PrimeBlock closingBlock(const PrimeBlockReplay& history, const std::vector<CompositeProof>& composites,
                        PrimeValue prime, const Identity& miner, const std::vector<Identity>& validators) {
    const OpenProofIndex index(history, composites);
    const auto proof = math::makePrattProof(prime, index);
    require(proof.has_value(), "closing Pratt proof");
    PrimeBlock block;
    block.height = history.blockCount();
    block.previous_hash = history.tip();
    block.previous_prime = history.frontier();
    block.prime.p = prime;
    block.prime.witness = proof->witness;
    block.prime.provider_address = prime == 2 ? "pcdev1_genesis" : miner.address;
    block.composites = composites;
    std::vector<std::pair<PrimeValue, std::uint64_t>> factors;
    for (const auto& factor : proof->factors_of_p_minus_1.factors) {
        block.prime.factors_of_p_minus_1.push_back({factor.prime, factor.exponent});
        factors.emplace_back(factor.prime, factor.exponent);
    }
    if (prime != 2) {
        std::string error;
        const auto signature = crypto::signProtocolMessage(miner.key.private_key,
            crypto::primeProofSigningPayload(block.previous_hash, prime, proof->witness, factors, miner.address), error);
        require(signature.has_value(), "closing miner authentication");
        block.prime.signature = crypto::packPrimeProofAuthentication(miner.key.public_key, *signature);
    }
    block.votes = certificate(blockSubjectHash(block), validators);
    return block;
}

protocol::Bytes readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "read fixture file");
    return protocol::Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const protocol::Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    require(bool(out), "write fixture file");
}

int run(const fs::path& root) {
    fs::create_directories(root);
    auto name = (root / "ranges-XXXXXX").string();
    require(mkdtemp(name.data()) != nullptr, "create private fixture directory");
    const fs::path directory(name);
    const std::vector<Identity> validators{identity(), identity(), identity()};
    const Identity alice = identity(), bob = identity(), prime_miner = identity();
    std::vector<Address> addresses;
    for (const auto& validator : validators) addresses.push_back(validator.address);
    PrimeBlockReplay history(addresses);
    std::vector<CompositeProof> pending;
    std::string error;
    for (PrimeValue n = 2; n <= 7; ++n) {
        if (math::isPrime(n)) {
            const auto block = closingBlock(history, pending, n, prime_miner, validators);
            const auto ok = history.append(block, error);
            require(ok, "history: " + error);
            pending.clear();
        } else pending.push_back(*math::makeCompositeProof(n, alice.address));
    }
    auto open = [&](const std::string& file, std::size_t index, bool create) {
        auto journal = std::make_unique<RangeJournal>((directory / file).string(), history, addresses, validators[index].address);
        const auto ok = journal->open(create, error);
        require(ok, "open " + file + ": " + error);
        return journal;
    };

    auto a = open("a.journal", 0, true);
    auto b = open("b.journal", 1, true);
    auto c = open("c.journal", 2, true);
    {
        RangeJournal second((directory / "a.journal").string(), history, addresses, validators[0].address);
        require(!second.open(false, error), "reject concurrent writer");
    }
    auto range = proposal(history, 8, 9, alice);
    auto invalid = range;
    invalid.factors[1] = {1, 9};
    signRange(invalid, alice);
    invalid.votes = certificate(rangeSubjectHash(invalid), validators);
    const auto initial = readFile((directory / "a.journal").string());
    require(a->accept(invalid, error) == RangeAcceptance::Rejected, "reject whole range with bad final member");
    require(!a->voteFor(invalid, validators[0].key, error), "do not vote for invalid range");
    require(a->acceptedFrontier() == 7 && a->composites().empty(), "no partial acceptance of 8");
    require(readFile((directory / "a.journal").string()) == initial, "invalid proposal does not touch journal");
    invalid = range;
    invalid.factors.pop_back();
    signRange(invalid, alice);
    require(!a->voteFor(invalid, validators[0].key, error), "missing member");
    invalid = range;
    invalid.start = 10;
    invalid.end = 9;
    signRange(invalid, alice);
    require(!a->voteFor(invalid, validators[0].key, error), "reversed range");
    invalid = range;
    invalid.end = invalid.start + kMaxRangeMembers;
    require(!a->voteFor(invalid, validators[0].key, error), "oversized range rejected");
    invalid = range;
    invalid.signature[0] ^= 1;
    require(!a->voteFor(invalid, validators[0].key, error), "bad miner signature");
    invalid = range;
    invalid.provider = bob.address;
    require(!a->voteFor(invalid, validators[0].key, error), "provider substitution");
    invalid = proposal(history, 8, 10, alice);
    invalid.end = 11;
    invalid.factors.push_back({1, 11});
    signRange(invalid, alice);
    require(!a->voteFor(invalid, validators[0].key, error), "range cannot cross prime 11");

    const auto vote_a = a->voteFor(range, validators[0].key, error);
    require(vote_a.has_value(), "first whole-range vote: " + error);
    require(a->acceptedFrontier() == 7, "one vote is not accepted work");
    auto competing = proposal(history, 8, 10, bob);
    require(!a->voteFor(competing, validators[0].key, error), "conflicting range length locked out");
    a.reset();
    a = open("a.journal", 0, false);
    require(!a->voteFor(competing, validators[0].key, error), "vote lock survives restart");
    require(a->voteFor(range, validators[0].key, error).has_value(), "same proposal can be retried after restart");
    range.votes = {*vote_a};
    require(a->accept(range, error) == RangeAcceptance::Rejected, "one vote cannot accept range");
    const auto vote_b = b->voteFor(range, validators[1].key, error);
    require(vote_b.has_value(), "second whole-range vote: " + error);
    range.votes.push_back(*vote_b);
    std::sort(range.votes.begin(), range.votes.end(), [](const auto& x, const auto& y) { return x.validator_address < y.validator_address; });
    auto duplicate = range;
    duplicate.votes[1] = duplicate.votes[0];
    require(a->accept(duplicate, error) == RangeAcceptance::Rejected, "duplicate vote does not form quorum");
    auto forged = range;
    forged.votes[0].signature[0] ^= 1;
    require(a->accept(forged, error) == RangeAcceptance::Rejected, "bad quorum signature rejected");
    forged = range;
    forged.votes = certificate(rangeSubjectHash(forged), {alice, bob});
    require(a->accept(forged, error) == RangeAcceptance::Rejected, "unauthorized signers rejected");
    require(c->voteFor(competing, validators[2].key, error).has_value(), "minority votes for competing proposal");
    for (auto* journal : {a.get(), b.get(), c.get()}) {
        const auto result = journal->accept(range, error);
        require(result == RangeAcceptance::Accepted, "accept quorum-certified range: " + error);
        require(journal->acceptedFrontier() == 9 && journal->composites().size() == 2, "atomic frontier advances to 9");
        require(journal->accept(range, error) == RangeAcceptance::AlreadyAccepted, "retry idempotent");
    }
    const auto persisted = readFile((directory / "a.journal").string());
    require(a->accept(range, error) == RangeAcceptance::AlreadyAccepted, "repeat retry");
    require(readFile((directory / "a.journal").string()) == persisted, "retry does not rewrite state");
    auto resigned = range;
    signRange(resigned, alice);
    require(rangeSubjectHash(resigned) == rangeSubjectHash(range), "receipt identity excludes authentication randomness");
    require(a->accept(resigned, error) == RangeAcceptance::AlreadyAccepted, "resigned identical work has no new reward");
    competing.votes = certificate(rangeSubjectHash(competing), validators);
    require(a->accept(competing, error) == RangeAcceptance::Rejected, "accepted ranges cannot overlap");

    a.reset();
    a = open("a.journal", 0, false);
    require(a->rangeCount() == 1 && a->acceptedFrontier() == 9, "accepted receipt survives restart");
    require(a->accept(range, error) == RangeAcceptance::AlreadyAccepted, "receipt query after restart");
    auto last = proposal(history, 10, 10, bob, a->acceptedRangeTip());
    auto wrong_parent = last;
    wrong_parent.previous_range = {};
    signRange(wrong_parent, bob);
    require(!a->voteFor(wrong_parent, validators[0].key, error), "range receipt linkage enforced");
    auto wrong_block = last;
    wrong_block.previous_block[0] ^= 1;
    signRange(wrong_block, bob);
    require(!a->voteFor(wrong_block, validators[0].key, error), "block anchor enforced");
    const auto last_a = a->voteFor(last, validators[0].key, error);
    const auto last_b = b->voteFor(last, validators[1].key, error);
    require(last_a && last_b, "singleton is also a range");
    last.votes = {*last_a, *last_b};
    std::sort(last.votes.begin(), last.votes.end(), [](const auto& x, const auto& y) { return x.validator_address < y.validator_address; });
    for (auto* journal : {a.get(), b.get(), c.get()})
        require(journal->accept(last, error) == RangeAcceptance::Accepted, "accept second range");

    auto block = closingBlock(history, a->composites(), 11, prime_miner, validators);
    block.votes.clear();
    const auto before_closing = readFile((directory / "a.journal").string());
    auto bad_closing = block;
    bad_closing.prime.witness = 0;
    require(!a->voteForBlock(bad_closing, validators[0].key, error), "no vote for invalid prime proof");
    bad_closing = block;
    bad_closing.prime.signature.back() ^= 1;
    require(!a->voteForBlock(bad_closing, validators[0].key, error), "no vote for unauthenticated prime");
    bad_closing = block;
    bad_closing.composites[0].provider_address = bob.address;
    require(!a->voteForBlock(bad_closing, validators[0].key, error), "no vote for reassigned composite work");
    require(!a->voteForBlock(block, validators[1].key, error), "closing vote uses local identity only");
    require(readFile((directory / "a.journal").string()) == before_closing, "bad closing proposals do not lock journal");
    require(history.validateProposal(block, error), "uncertified block is a valid proposal");
    auto uncertified_replay = history;
    require(!uncertified_replay.append(block, error) && uncertified_replay.frontier() == 7,
            "proposal validation cannot finalize or award rewards");
    const auto closing_a = a->voteForBlock(block, validators[0].key, error);
    require(closing_a.has_value(), "persist and sign closing proposal");
    auto other_closing = closingBlock(history, a->composites(), 11, bob, validators);
    other_closing.votes.clear();
    require(!a->voteForBlock(other_closing, validators[0].key, error), "refuse conflicting closing miner");
    require(!a->voteFor(last, validators[0].key, error), "closing lock blocks further range votes");
    require(!a->closed() && a->acceptedFrontier() == 10, "closing vote is not finalization");
    a.reset();
    a = open("a.journal", 0, false);
    require(!a->voteForBlock(other_closing, validators[0].key, error), "closing lock survives restart");
    require(a->voteForBlock(block, validators[0].key, error).has_value(), "same closing proposal retry allowed");
    require(c->voteForBlock(other_closing, validators[2].key, error).has_value(), "minority closing proposal fixture");
    block.votes = {*closing_a};
    require(!a->seal(block, error), "one closing vote cannot seal block");
    const auto closing_b = b->voteForBlock(block, validators[1].key, error);
    require(closing_b.has_value(), "second validator signs closing block");
    block.votes.push_back(*closing_b);
    std::sort(block.votes.begin(), block.votes.end(), [](const auto& x, const auto& y) { return x.validator_address < y.validator_address; });
    auto altered = block;
    altered.composites[0].provider_address = bob.address;
    altered.votes = certificate(blockSubjectHash(altered), validators);
    require(!a->seal(altered, error), "even a signed closing block cannot reassign accepted work locally");
    altered = block;
    altered.composites.pop_back();
    altered.votes = certificate(blockSubjectHash(altered), validators);
    require(!a->seal(altered, error), "closing block must contain all accepted work");
    require(a->seal(block, error), "seal prime block: " + error);
    require(c->seal(block, error), "certified closing block resolves minority closing lock");
    require(a->closed() && a->seal(block, error), "idempotent closure");
    require(!a->voteForBlock(block, validators[0].key, error), "closed journal cannot emit more closing votes");
    require(!a->voteFor(last, validators[0].key, error), "no votes after closure");
    a.reset();
    a = open("a.journal", 0, false);
    require(a->closed(), "closure survives restart");
    PrimeBlock restored;
    require(decodeBlock(readFile((directory / "a.journal.block").string()), restored, error), "decode standalone prime block");
    auto replay = history;
    require(replay.append(restored, error), "replay block without any range signatures");
    require(replay.balance(alice.address, 11) == 300000 && replay.balance(bob.address, 11) == 150000,
            "rewards are per composite, not per range");
    require(replay.balance(prime_miner.address, 11) == 450000 && replay.supply(11) == node::kAssetMicroUnits,
            "prime reward and total supply unchanged");
    require(math::makePrattProof(11, replay).has_value(), "Pratt reconstruction survives range signature omission");
    require(std::all_of(restored.composites.begin(), restored.composites.end(),
            [](const auto& proof) { return proof.signature.empty(); }), "no per-composite signatures in block");

    // A single three-member range also requires just one quorum certificate.
    auto entire = open("entire.journal", 0, true);
    auto whole_range = proposal(history, 8, 10, alice);
    whole_range.votes = certificate(rangeSubjectHash(whole_range), validators);
    require(entire->accept(whole_range, error) == RangeAcceptance::Accepted, "accept all three composites atomically");
    require(entire->rangeCount() == 1 && entire->composites().size() == 3, "one receipt for three composites");
    const auto whole_block = closingBlock(history, entire->composites(), 11, prime_miner, validators);
    require(entire->seal(whole_block, error), "close single-range block");
    auto whole_replay = history;
    require(whole_replay.append(whole_block, error) && whole_replay.balance(alice.address, 11) == 450000,
            "all three shares go to the accepted range provider");

    // Interrupted writes must expose either the whole old state or the whole new state.
    for (const auto fault : {TestWriteFailure::BeforePublish, TestWriteFailure::AfterPublish}) {
        const auto file = fault == TestWriteFailure::BeforePublish ? "before.journal" : "after.journal";
        auto journal = open(file, 0, true);
        journal->failNextWriteForTest(fault);
        require(journal->accept(range, error) == RangeAcceptance::RecoveryRequired, "write failure is not a rejection acknowledgement");
        require(!journal->ready() && !journal->voteFor(range, validators[0].key, error), "uncertain persistence blocks further signing");
        journal.reset();
        journal = open(file, 0, false);
        const bool committed = fault == TestWriteFailure::AfterPublish;
        require(journal->acceptedFrontier() == (committed ? 9 : 7), "restart recovers entire range or none");
        require(journal->accept(range, error) == (committed ? RangeAcceptance::AlreadyAccepted : RangeAcceptance::Accepted),
                "retry resolves lost acknowledgement");
    }
    for (const auto fault : {TestWriteFailure::BeforePublish, TestWriteFailure::AfterPublish}) {
        const auto file = fault == TestWriteFailure::BeforePublish ? "lock-before.journal" : "lock-after.journal";
        auto journal = open(file, 0, true);
        journal->failNextWriteForTest(fault);
        require(!journal->voteFor(range, validators[0].key, error), "no vote returned on lock persistence failure");
        journal.reset();
        journal = open(file, 0, false);
        const bool may_vote_other = fault == TestWriteFailure::BeforePublish;
        require(journal->voteFor(competing, validators[0].key, error).has_value() == may_vote_other,
                "persisted vote lock honored after acknowledgement loss");
    }
    for (const auto fault : {TestWriteFailure::BeforePublish, TestWriteFailure::AfterPublish}) {
        const auto file = fault == TestWriteFailure::BeforePublish ? "closing-lock-before.journal" : "closing-lock-after.journal";
        auto journal = open(file, 0, true);
        require(journal->accept(range, error) == RangeAcceptance::Accepted, "closing lock setup range one");
        require(journal->accept(last, error) == RangeAcceptance::Accepted, "closing lock setup range two");
        journal->failNextWriteForTest(fault);
        require(!journal->voteForBlock(block, validators[0].key, error) && !journal->ready(),
                "no closing vote on persistence failure");
        require(!journal->voteForBlock(other_closing, validators[0].key, error), "failed closing write blocks all signing");
        journal.reset();
        journal = open(file, 0, false);
        require(journal->voteForBlock(other_closing, validators[0].key, error).has_value() ==
                (fault == TestWriteFailure::BeforePublish), "closing lock recovered after acknowledgement loss");
        require(journal->seal(block, error), "certified closure resolves recovered vote lock");
    }
    for (const auto fault : {TestWriteFailure::BeforePublish, TestWriteFailure::AfterPublish}) {
        const auto file = fault == TestWriteFailure::BeforePublish ? "seal-before.journal" : "seal-after.journal";
        auto journal = open(file, 0, true);
        require(journal->accept(range, error) == RangeAcceptance::Accepted, "closure failure setup range one");
        require(journal->accept(last, error) == RangeAcceptance::Accepted, "closure failure setup range two");
        journal->failNextWriteForTest(fault);
        require(!journal->seal(block, error) && !journal->ready(), "closure failure requires reload");
        journal.reset();
        journal = open(file, 0, false);
        require(journal->closed() == (fault == TestWriteFailure::AfterPublish), "recover published checkpoint");
        require(journal->seal(block, error), "retry closure after interruption");
    }

    // Policy is fixed for an open interval and bound into signed range subjects.
    const RangePolicy small_policy{2};
    const auto policy_path = (directory / "policy.journal").string();
    auto small = std::make_unique<RangeJournal>(policy_path, history, addresses, validators[0].address, small_policy);
    require(small->open(true, error) && small->policy().max_members == 2, "configure range size policy");
    require(!small->voteFor(range, validators[0].key, error), "mismatched signed policy rejected even below limit");
    require(small->accept(range, error) == RangeAcceptance::Rejected, "certificate cannot override policy mismatch");
    auto oversized = proposal(history, 8, 10, alice);
    oversized.max_range_members = 2;
    signRange(oversized, alice);
    oversized.votes = certificate(rangeSubjectHash(oversized), validators);
    require(!small->voteFor(oversized, validators[0].key, error), "policy rejects oversized voting proposal");
    require(small->accept(oversized, error) == RangeAcceptance::Rejected && small->acceptedFrontier() == 7,
            "certified oversized range still rejected atomically");
    auto limited = range;
    limited.max_range_members = 2;
    require(rangeSubjectHash(limited) != rangeSubjectHash(range), "range signature subject binds policy");
    require(!small->voteFor(limited, validators[0].key, error), "changing limit invalidates miner signature");
    signRange(limited, alice);
    require(small->accept(limited, error) == RangeAcceptance::Rejected, "old-policy votes cannot be reused");
    limited.votes = certificate(rangeSubjectHash(limited), validators);
    require(small->voteFor(limited, validators[0].key, error).has_value(), "exact policy boundary allowed");
    require(small->accept(limited, error) == RangeAcceptance::Accepted, "exact limit range accepted");
    small.reset();
    {
        RangeJournal changed(policy_path, history, addresses, validators[0].address, RangePolicy{1});
        require(!changed.open(false, error), "restart cannot change active interval policy");
    }
    small = std::make_unique<RangeJournal>(policy_path, history, addresses, validators[0].address, small_policy);
    require(small->open(false, error) && small->acceptedFrontier() == 9, "same policy recovers receipts");
    auto limited_last = last;
    limited_last.previous_range = small->acceptedRangeTip();
    limited_last.max_range_members = 2;
    signRange(limited_last, bob);
    limited_last.votes = certificate(rangeSubjectHash(limited_last), validators);
    require(small->accept(limited_last, error) == RangeAcceptance::Accepted, "range cap is not a block composite cap");
    require(small->seal(block, error), "policy-sized ranges retain original attribution and rewards");
    for (const auto limit : {std::uint64_t(0), std::uint64_t(kMaxRangeMembers + 1)}) {
        RangeJournal invalid_policy((directory / "bad-policy.journal").string(), history, addresses,
            validators[0].address, RangePolicy{limit});
        require(!invalid_policy.open(true, error), "reject zero or above safety ceiling policy");
    }
    RangeJournal singleton((directory / "singleton-policy.journal").string(), history, addresses,
        validators[0].address, RangePolicy{1});
    require(singleton.open(true, error), "one-member policy valid");
    auto one = proposal(history, 8, 8, alice);
    one.max_range_members = 1;
    signRange(one, alice);
    one.votes = certificate(rangeSubjectHash(one), validators);
    require(singleton.accept(one, error) == RangeAcceptance::Accepted, "singleton policy allows single composite");

    // Corruption is never treated as an empty journal, which could lose voting locks.
    auto corrupt = open("corrupt.journal", 0, true);
    require(corrupt->voteFor(range, validators[0].key, error).has_value(), "corruption fixture lock");
    corrupt.reset();
    auto bytes = readFile((directory / "corrupt.journal").string());
    bytes.back() ^= 1;
    writeFile((directory / "corrupt.journal").string(), bytes);
    RangeJournal damaged((directory / "corrupt.journal").string(), history, addresses, validators[0].address);
    require(!damaged.open(false, error), "corrupt journal fails closed");
    require(!damaged.voteFor(competing, validators[0].key, error), "no signing after corrupt recovery");
    RangeJournal missing((directory / "missing.journal").string(), history, addresses, validators[0].address);
    require(!missing.open(false, error), "missing journal is not silently initialized");
    b.reset();
    RangeJournal wrong_identity((directory / "b.journal").string(), history, addresses, validators[0].address);
    require(!wrong_identity.open(false, error), "journal cannot be reused with another identity");

    // Exercise the isolated validator message boundary over real loopback TCP.
    auto net_a = open("net-a.journal", 0, true);
    auto net_b = open("net-b.journal", 1, true);
    auto net_c = open("net-c.journal", 2, true);
    auto call = [&](RangeJournal& journal, std::size_t validator, RequestKind kind,
                    const protocol::Bytes& payload = protocol::Bytes{}) {
        return test::exchange(encodeRequest(kind, payload), journal, validators[validator].key);
    };
    auto status = call(*net_a, 0, RequestKind::Status);
    require(status.kind == ReplyKind::Status, "network status response");
    require(call(*net_a, 0, RequestKind::ChainStatus).kind == ReplyKind::Rejected,
            "interval handler rejects chain status operation");
    require(call(*net_a, 0, RequestKind::InstallBlock, encodeBlock(block)).kind == ReplyKind::Rejected,
            "interval handler does not pretend to install a chain block");
    detail::Reader status_reader(status.payload);
    require(status_reader.integer() == RangePolicy{}.max_members && status_reader.integer() == 7 &&
            status_reader.integer() == 0 && status_reader.integer() == 0 && status_reader.integer() == 1,
            "network advertises policy and recovered frontier");
    status_reader.finish();
    auto wire_range = proposal(history, 8, 10, alice);
    auto wire_bad = wire_range;
    wire_bad.factors.back() = {1, 10};
    signRange(wire_bad, alice);
    require(call(*net_a, 0, RequestKind::VoteRange, encodeRange(wire_bad)).kind == ReplyKind::Rejected,
            "network invalid final member rejects whole range");
    auto malformed = encodeRequest(RequestKind::VoteRange, encodeRange(wire_range));
    malformed.push_back(0);
    require(test::exchange(malformed, *net_a, validators[0].key).kind == ReplyKind::Rejected,
            "network trailing request bytes rejected before voting");
    malformed = encodeRequest(RequestKind::Status);
    malformed[8] = 255;
    require(test::exchange(malformed, *net_a, validators[0].key).kind == ReplyKind::Rejected,
            "unknown request rejected");
    require(test::exchange(protocol::Bytes{1, 2}, *net_a, validators[0].key).kind == ReplyKind::Rejected,
            "truncated request rejected");
    require(call(*net_a, 0, RequestKind::Status, {1}).kind == ReplyKind::Rejected,
            "status request must have no payload");
    const auto wire_a = call(*net_a, 0, RequestKind::VoteRange, encodeRange(wire_range));
    const auto wire_b = call(*net_b, 1, RequestKind::VoteRange, encodeRange(wire_range));
    CompositeRange voted_a, voted_b;
    require(wire_a.kind == ReplyKind::Vote && decodeRange(wire_a.payload, voted_a, error) && voted_a.votes.size() == 1,
            "first validator votes through TCP");
    require(wire_b.kind == ReplyKind::Vote && decodeRange(wire_b.payload, voted_b, error) && voted_b.votes.size() == 1,
            "second validator votes through TCP");
    require(call(*net_a, 0, RequestKind::AcceptRange, encodeRange(voted_a)).kind == ReplyKind::Rejected,
            "network one vote cannot accept range");
    wire_range.votes = {voted_a.votes.front(), voted_b.votes.front()};
    std::sort(wire_range.votes.begin(), wire_range.votes.end(), [](const auto& x, const auto& y) {
        return x.validator_address < y.validator_address;
    });
    require(call(*net_a, 0, RequestKind::AcceptRange, encodeRange(wire_range)).kind == ReplyKind::Accepted,
            "network certificate accepts whole range");
    require(call(*net_b, 1, RequestKind::AcceptRange, encodeRange(wire_range)).kind == ReplyKind::Accepted,
            "second validator accepts network certificate");
    require(call(*net_a, 0, RequestKind::VoteRange, encodeRange(wire_range)).kind == ReplyKind::Rejected,
            "voting request must not contain votes");
    net_c->failNextWriteForTest(TestWriteFailure::AfterPublish);
    require(call(*net_c, 2, RequestKind::AcceptRange, encodeRange(wire_range)).kind == ReplyKind::RecoveryRequired,
            "wire reports uncertain persistence distinctly from rejection");
    net_c.reset();
    net_c = open("net-c.journal", 2, false);
    require(call(*net_c, 2, RequestKind::AcceptRange, encodeRange(wire_range)).kind == ReplyKind::AlreadyAccepted,
            "network retry after restart recovers published receipt");
    auto wire_block = closingBlock(history, net_a->composites(), 11, prime_miner, validators);
    wire_block.votes.clear();
    const auto close_a = call(*net_a, 0, RequestKind::VoteBlock, encodeBlock(wire_block));
    PrimeBlock voted_block_a, voted_block_b;
    require(close_a.kind == ReplyKind::Vote && decodeBlock(close_a.payload, voted_block_a, error) &&
            voted_block_a.votes.size() == 1, "network closing vote returned only after lock persistence");
    net_a.reset();
    net_a = open("net-a.journal", 0, false);
    auto wire_other = closingBlock(history, net_a->composites(), 11, bob, validators);
    wire_other.votes.clear();
    require(call(*net_a, 0, RequestKind::VoteBlock, encodeBlock(wire_other)).kind == ReplyKind::Rejected,
            "network conflicting closure rejected after restart");
    require(call(*net_a, 0, RequestKind::SealBlock, encodeBlock(voted_block_a)).kind == ReplyKind::Rejected,
            "network one closing vote cannot finalize");
    const auto close_b = call(*net_b, 1, RequestKind::VoteBlock, encodeBlock(wire_block));
    require(close_b.kind == ReplyKind::Vote && decodeBlock(close_b.payload, voted_block_b, error) &&
            voted_block_b.votes.size() == 1, "network second closing vote");
    wire_block.votes = {voted_block_a.votes.front(), voted_block_b.votes.front()};
    std::sort(wire_block.votes.begin(), wire_block.votes.end(), [](const auto& x, const auto& y) {
        return x.validator_address < y.validator_address;
    });
    for (auto* journal : {net_a.get(), net_b.get(), net_c.get()})
        require(call(*journal, 0, RequestKind::SealBlock, encodeBlock(wire_block)).kind == ReplyKind::Sealed,
                "all validators seal identical network block");
    require(call(*net_a, 0, RequestKind::SealBlock, encodeBlock(wire_block)).kind == ReplyKind::Sealed,
            "network closing retry idempotent");
    auto network_replay = history;
    require(network_replay.append(wire_block, error) && network_replay.balance(alice.address, 11) == 450000,
            "network block independently replays with unchanged rewards");
    auto malformed_reply = encodeReply(status);
    malformed_reply.pop_back();
    ValidatorReply parsed_reply;
    require(!decodeReply(malformed_reply, parsed_reply, error), "reject truncated wire reply");

    auto encoded = encodeRange(range);
    CompositeRange decoded;
    require(decodeRange(encoded, decoded, error) && encodeRange(decoded) == encoded, "canonical range round trip");
    for (const auto size : {std::size_t(0), std::size_t(7), encoded.size() - 1})
        require(!decodeRange(protocol::Bytes(encoded.begin(), encoded.begin() + size), decoded, error), "reject truncated range");
    encoded.push_back(0);
    require(!decodeRange(encoded, decoded, error), "reject trailing range data");
    encoded = encodeRange(range);
    const auto count_offset = rangeSigningPayload(range).size() - range.factors.size() * 16 - 8;
    encoded[count_offset] = 1;
    encoded[count_offset + 1] = 1;
    require(!decodeRange(encoded, decoded, error), "reject excessive decoded member count before allocation");
    std::cout << "atomic_range_tests: PASS\nchecks: " << checks
              << "\naccepted_ranges: 8-9 (Alice), 10 (Bob)\nclosing_prime: 11\n"
              << "range_certificates: 2 (two validator votes each)\n"
              << "per_composite_vote_certificates: 0\n"
              << "artifacts: " << fs::absolute(directory).string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: primechain-atomic-range-tests <artifact-root>");
        return run(argv[1]);
    } catch (const std::exception& exception) {
        std::cerr << "atomic range test failed: " << exception.what() << '\n';
        return 1;
    }
}
