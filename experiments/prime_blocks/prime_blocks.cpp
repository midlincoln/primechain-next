#include "prime_blocks.hpp"
#include "binary.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/node/sequential_node.hpp"

namespace primechain::experiments {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'P', 'C', 'N', 'X', 'P', 'B', '0', '1'};

void blob(protocol::Bytes& out, const protocol::Bytes& value) {
    appendWord(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void address(protocol::Bytes& out, const Address& value) {
    blob(out, protocol::Bytes(value.begin(), value.end()));
}

math::PrattProof mathProof(const protocol::PrattPrimeProofV0& proof) {
    math::PrattProof result;
    result.p = proof.p;
    result.witness = proof.witness;
    for (const auto& factor : proof.factors_of_p_minus_1)
        result.factors_of_p_minus_1.factors.push_back({factor.prime, factor.exponent});
    return result;
}

} // namespace

void appendWord(protocol::Bytes& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

protocol::Bytes encodeBlock(const PrimeBlock& block, bool with_votes) {
    protocol::Bytes out(kMagic.begin(), kMagic.end());
    appendWord(out, block.height);
    out.insert(out.end(), block.previous_hash.begin(), block.previous_hash.end());
    appendWord(out, block.previous_prime);
    appendWord(out, block.prime.p);
    appendWord(out, block.prime.witness);
    address(out, block.prime.provider_address);
    blob(out, block.prime.signature);
    appendWord(out, block.prime.factors_of_p_minus_1.size());
    for (const auto& factor : block.prime.factors_of_p_minus_1) {
        appendWord(out, factor.prime);
        appendWord(out, factor.exponent);
    }
    appendWord(out, block.composites.size());
    for (const auto& proof : block.composites) {
        if (!proof.signature.empty()) throw std::runtime_error("composite signatures are journal-only");
        appendWord(out, proof.m);
        appendWord(out, proof.d);
        appendWord(out, proof.e);
        address(out, proof.provider_address);
    }
    if (with_votes) {
        appendWord(out, block.votes.size());
        for (const auto& vote : block.votes) {
            address(out, vote.validator_address);
            blob(out, vote.public_key);
            out.insert(out.end(), vote.record_hash.begin(), vote.record_hash.end());
            appendWord(out, vote.round);
            blob(out, vote.signature);
        }
    }
    if (out.size() > kExperimentMaxBytes) throw std::runtime_error("block exceeds experiment byte limit");
    return out;
}

bool decodeBlock(const protocol::Bytes& bytes, PrimeBlock& block, std::string& error) {
    error.clear();
    try {
        if (bytes.size() > kExperimentMaxBytes) throw std::runtime_error("block exceeds experiment byte limit");
        detail::Reader reader(bytes);
        reader.magic(kMagic);
        PrimeBlock result;
        result.height = reader.integer();
        result.previous_hash = reader.hash();
        result.previous_prime = reader.integer();
        result.prime.p = reader.integer();
        result.prime.witness = reader.integer();
        result.prime.provider_address = reader.address();
        result.prime.signature = reader.blob(8192);
        const auto factors = reader.count(64);
        for (std::size_t i = 0; i < factors; ++i) {
            const auto prime = reader.integer();
            const auto exponent = reader.integer();
            result.prime.factors_of_p_minus_1.push_back({prime, exponent});
        }
        const auto composites = reader.count(kExperimentMaxInteger);
        for (std::size_t i = 0; i < composites; ++i) {
            CompositeProof proof;
            proof.m = reader.integer();
            proof.d = reader.integer();
            proof.e = reader.integer();
            proof.provider_address = reader.address();
            result.composites.push_back(std::move(proof));
        }
        const auto votes = reader.count(3);
        for (std::size_t i = 0; i < votes; ++i) {
            protocol::ValidatorVoteV0 vote;
            vote.validator_address = reader.address();
            vote.public_key = reader.blob(4096);
            vote.record_hash = reader.hash();
            vote.round = reader.integer();
            vote.signature = reader.blob(4096);
            result.votes.push_back(std::move(vote));
        }
        reader.finish();
        block = std::move(result);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

Hash256 blockSubjectHash(const PrimeBlock& block) {
    return crypto::sha3_256(encodeBlock(block, false));
}

PrimeBlockReplay::PrimeBlockReplay(std::vector<Address> validators)
    : validators_(std::move(validators)) {
    std::sort(validators_.begin(), validators_.end());
}

std::optional<CompositeProof> PrimeBlockReplay::findCompositeProof(PrimeValue n) const {
    const auto found = proofs_.find(n);
    if (found == proofs_.end()) return std::nullopt;
    return found->second;
}

std::optional<CompositeProof> OpenProofIndex::findCompositeProof(PrimeValue n) const {
    const auto found = std::find_if(pending_.begin(), pending_.end(),
        [n](const auto& proof) { return proof.m == n; });
    return found == pending_.end() ? history_.findCompositeProof(n) : std::optional<CompositeProof>(*found);
}

std::uint64_t PrimeBlockReplay::balance(const Address& address, PrimeValue prime) const {
    const auto found = balances_.find({address, prime});
    return found == balances_.end() ? 0 : found->second;
}

std::uint64_t PrimeBlockReplay::supply(PrimeValue prime) const {
    const auto found = supplies_.find(prime);
    return found == supplies_.end() ? 0 : found->second;
}

bool PrimeBlockReplay::validateProposal(const PrimeBlock& block, std::string& error) const {
    error.clear();
    auto reject = [&](const std::string& reason) { error = reason; return false; };
    if (validators_.size() != 3) return reject("experiment requires a fixed three-validator set");
    const auto p = block.prime.p;
    if (p < 2 || p > kExperimentMaxInteger || p <= frontier_ ||
        block.height != block_count_ || block.previous_prime != frontier_ || block.previous_hash != tip_)
        return reject("invalid block position or experiment frontier");
    const bool genesis = block_count_ == 0;
    if (genesis && (p != 2 || block.prime.provider_address != "pcdev1_genesis" || !block.prime.signature.empty()))
        return reject("invalid experimental genesis");
    const auto expected_count = genesis ? 0 : p - frontier_ - 1;
    if (block.composites.size() != expected_count) return reject("incomplete composite interval");
    for (std::size_t i = 0; i < block.composites.size(); ++i) {
        const auto& proof = block.composites[i];
        if (proof.m != frontier_ + 1 + i || !proof.signature.empty() ||
            !crypto::isProtocolSignatureAddress(proof.provider_address) || !math::verifyCompositeProof(proof))
            return reject("invalid composite evidence, order, or provider");
    }
    // Bound arithmetic before invoking the inherited trial-division verifier.
    if (block.prime.witness > kExperimentMaxInteger || block.prime.factors_of_p_minus_1.size() > 64)
        return reject("prime proof exceeds experiment limits");
    for (const auto& factor : block.prime.factors_of_p_minus_1)
        if (factor.prime > kExperimentMaxInteger || factor.exponent > 64)
            return reject("prime factor exceeds experiment limits");
    const auto proof = mathProof(block.prime);
    if (!math::verifyPrattProof(proof)) return reject("invalid closing prime proof");
    if (!genesis) {
        const OpenProofIndex index(*this, block.composites);
        const auto factors = math::factorizeFromProofIndex(p - 1, index);
        if (!factors || math::serializeFactorization(*factors) != math::serializeFactorization(proof.factors_of_p_minus_1))
            return reject("closing proof cannot be reconstructed from interval and history");
        std::vector<std::pair<PrimeValue, std::uint64_t>> pairs;
        for (const auto& factor : block.prime.factors_of_p_minus_1) pairs.emplace_back(factor.prime, factor.exponent);
        if (!crypto::verifyPackedPrimeProofAuthentication(block.previous_hash, p, block.prime.witness,
                pairs, block.prime.provider_address, block.prime.signature, error)) return false;
    }
    try { (void)blockSubjectHash(block); }
    catch (const std::exception& exception) { return reject(exception.what()); }
    return true;
}

bool PrimeBlockReplay::append(const PrimeBlock& block, std::string& error) {
    if (!validateProposal(block, error)) return false;
    const auto p = block.prime.p;
    const bool genesis = block_count_ == 0;
    Hash256 subject{};
    try { subject = blockSubjectHash(block); }
    catch (const std::exception& exception) { error = exception.what(); return false; }
    protocol::FinalizationProofV0 certificate;
    certificate.rule = "fixed-2-of-3-mldsa65-v2";
    certificate.votes = block.votes;
    if (!protocol::verifyRecordFinalization(certificate, subject, block.previous_hash, p, validators_, error)) return false;

    // All validation precedes state mutation. Rewards match the inherited fixed-validator ledger.
    auto credit = [&](const Address& address, std::uint64_t amount) { balances_[{address, p}] += amount; };
    if (genesis) {
        credit(block.prime.provider_address, node::kAssetMicroUnits);
    } else {
        credit(protocol::validatorRewardPoolAddress(0), node::kValidatorPrimeRewardMicroUnits);
        if (block.composites.empty()) {
            credit(block.prime.provider_address, node::kPrimeDiscoveryRewardMicroUnits + node::kCompositeDiscoveryRewardMicroUnits);
        } else {
            const auto count = block.composites.size();
            credit(block.prime.provider_address, node::kPrimeDiscoveryRewardMicroUnits + node::kCompositeDiscoveryRewardMicroUnits % count);
            for (const auto& composite : block.composites)
                credit(composite.provider_address, node::kCompositeDiscoveryRewardMicroUnits / count);
        }
    }
    for (const auto& composite : block.composites) proofs_.emplace(composite.m, composite);
    supplies_[p] = node::kAssetMicroUnits;
    frontier_ = p;
    ++block_count_;
    tip_ = subject;
    return true;
}

} // namespace primechain::experiments
