#pragma once

#include "continuous_chain.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "primechain/node/sequential_node.hpp"

namespace primechain::experiments::test_support {
using namespace primechain;
using namespace primechain::experiments;
namespace fs = std::filesystem;
inline std::size_t checks = 0;

inline void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct Identity {
    crypto::SignatureKeyPair key;
    Address address;
    Identity() {
        std::string error;
        const auto generated = crypto::generateProtocolSignatureKeyPair(error);
        require(generated.has_value(), "generate fixture identity: " + error);
        key = *generated;
        address = crypto::addressFromProtocolPublicKey(key.public_key);
    }
};

inline void sortVotes(std::vector<protocol::ValidatorVoteV0>& votes) {
    std::sort(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
        return a.validator_address < b.validator_address;
    });
}

struct Fixture {
    std::vector<Identity> validators{3};
    Identity alice, bob, miner;
    std::vector<Address> addresses;
    RangePolicy policy{2};
    PrimeBlock genesis;
    std::string error;

    Fixture() {
        for (const auto& validator : validators) addresses.push_back(validator.address);
        genesis = block(PrimeBlockReplay(addresses), {}, 2, miner);
    }

    std::vector<protocol::ValidatorVoteV0> certificate(const Hash256& subject) {
        std::vector<protocol::ValidatorVoteV0> result;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& signer = validators[i];
            auto vote = protocol::makeSignedValidatorVote(signer.address, signer.key.public_key,
                signer.key.private_key, subject, 1, error);
            require(!vote.signature.empty(), "fixture certificate");
            result.push_back(std::move(vote));
        }
        sortVotes(result);
        return result;
    }

    PrimeBlock block(const PrimeBlockReplay& history, const std::vector<CompositeProof>& composites,
                     PrimeValue prime, const Identity& provider) {
        const OpenProofIndex index(history, composites);
        const auto proof = math::makePrattProof(prime, index);
        require(proof.has_value(), "construct fixture prime proof");
        PrimeBlock result;
        result.height = history.blockCount();
        result.previous_hash = history.tip();
        result.previous_prime = history.frontier();
        result.composites = composites;
        result.prime.p = prime;
        result.prime.witness = proof->witness;
        result.prime.provider_address = prime == 2 ? "pcdev1_genesis" : provider.address;
        std::vector<std::pair<PrimeValue, std::uint64_t>> factors;
        for (const auto& factor : proof->factors_of_p_minus_1.factors) {
            result.prime.factors_of_p_minus_1.push_back({factor.prime, factor.exponent});
            factors.emplace_back(factor.prime, factor.exponent);
        }
        if (prime != 2) {
            const auto signature = crypto::signProtocolMessage(provider.key.private_key,
                crypto::primeProofSigningPayload(result.previous_hash, prime, proof->witness, factors,
                                                 provider.address), error);
            require(signature.has_value(), "authenticate fixture prime");
            result.prime.signature = crypto::packPrimeProofAuthentication(provider.key.public_key, *signature);
        }
        result.votes = certificate(blockSubjectHash(result));
        return result;
    }

    CompositeRange range(const ContinuousChain& chain, PrimeValue first, PrimeValue last, const Identity& provider) {
        return range(chain.history(), first, last, provider, chain.acceptedRangeTip());
    }

    CompositeRange range(const PrimeBlockReplay& history, PrimeValue first, PrimeValue last,
                         const Identity& provider, const Hash256& previous_range = {}) {
        CompositeRange result;
        result.previous_block = history.tip();
        result.previous_range = previous_range;
        result.max_range_members = policy.max_members;
        result.start = first;
        result.end = last;
        result.provider = provider.address;
        result.public_key = provider.key.public_key;
        for (auto n = first; n <= last; ++n) {
            const auto proof = math::makeCompositeProof(n, provider.address);
            require(proof.has_value(), "fixture range cannot contain prime");
            result.factors.push_back({proof->d, proof->e});
        }
        const auto signature = crypto::signProtocolMessage(provider.key.private_key, rangeSigningPayload(result), error);
        require(signature.has_value(), "authenticate fixture range");
        result.signature = *signature;
        result.votes = certificate(rangeSubjectHash(result));
        return result;
    }

    std::unique_ptr<ContinuousChain> open(const fs::path& path, bool create, std::size_t validator = 0) {
        auto chain = std::make_unique<ContinuousChain>(path.string(), genesis, addresses, validators[validator].address, policy);
        const bool ok = chain->open(create, error);
        require(ok, "open chain: " + error);
        return chain;
    }
};

inline protocol::Bytes readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "read test artifact");
    return protocol::Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void corrupt(const fs::path& path) {
    auto bytes = readFile(path);
    require(!bytes.empty(), "corruption fixture nonempty");
    bytes.back() ^= 1;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    require(bool(out), "write test corruption");
}

inline void compare(const PrimeBlockReplay& actual, const PrimeBlockReplay& expected, const Fixture& f) {
    require(actual.tip() == expected.tip() && actual.frontier() == expected.frontier() &&
            actual.blockCount() == expected.blockCount(), "replay frontier/hash/count agree");
    for (PrimeValue n = 2; n <= expected.frontier(); ++n) {
        if (math::isPrime(n)) {
            require(actual.supply(n) == expected.supply(n), "supply replayed exactly once");
            for (const auto& address : {f.alice.address, f.bob.address, f.miner.address,
                                       Address("pcdev1_genesis"), protocol::validatorRewardPoolAddress(0)})
                require(actual.balance(address, n) == expected.balance(address, n), "reward replayed exactly once");
        } else {
            const auto a = actual.findCompositeProof(n), b = expected.findCompositeProof(n);
            require(a && b && a->d == b->d && a->e == b->e && a->provider_address == b->provider_address,
                    "proof index and composite attribution rebuilt");
        }
    }
}

} // namespace primechain::experiments::test_support
