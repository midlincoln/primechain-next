#include "prime_blocks.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#include "primechain/crypto/signature.hpp"
#include "primechain/node/sequential_node.hpp"

namespace {
using namespace primechain;
using namespace primechain::experiments;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Identity {
    crypto::SignatureKeyPair keys;
    Address address;
};

Identity identity() {
    std::string error;
    const auto keys = crypto::generateProtocolSignatureKeyPair(error);
    check(keys.has_value(), "key generation: " + error);
    return {*keys, crypto::addressFromProtocolPublicKey(keys->public_key)};
}

std::vector<protocol::ValidatorVoteV0> votes(const Hash256& hash, const std::vector<Identity>& validators) {
    std::vector<protocol::ValidatorVoteV0> result;
    for (std::size_t i = 0; i < 2; ++i) {
        std::string error;
        const auto& validator = validators.at(i);
        auto vote = protocol::makeSignedValidatorVote(validator.address, validator.keys.public_key,
            validator.keys.private_key, hash, 1, error);
        check(!vote.signature.empty(), "vote signing: " + error);
        result.push_back(std::move(vote));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.validator_address < b.validator_address;
    });
    return result;
}

void authenticate(protocol::PrattPrimeProofV0& proof, const Hash256& previous, const Identity& miner) {
    std::vector<std::pair<PrimeValue, std::uint64_t>> factors;
    for (const auto& factor : proof.factors_of_p_minus_1) factors.emplace_back(factor.prime, factor.exponent);
    std::string error;
    const auto signature = crypto::signProtocolMessage(miner.keys.private_key,
        crypto::primeProofSigningPayload(previous, proof.p, proof.witness, factors, miner.address), error);
    check(signature.has_value(), "prime signing: " + error);
    proof.signature = crypto::packPrimeProofAuthentication(miner.keys.public_key, *signature);
}

template <typename Record>
void finalize(Record& record, const std::vector<Identity>& validators) {
    protocol::updateTransactionBatch(record);
    record.finalized_by.rule = "fixed-2-of-3-mldsa65-v2";
    record.finalized_by.votes = votes(protocol::candidateRecordHash(record), validators);
}

void writeFrame(std::ostream& out, const protocol::Bytes& bytes) {
    protocol::Bytes length;
    appendWord(length, bytes.size());
    out.write(reinterpret_cast<const char*>(length.data()), length.size());
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(out), "writing compact block failed");
}

PrimeBlockReplay replayFile(const fs::path& file, const std::vector<Address>& validators) {
    PrimeBlockReplay replay(validators);
    std::ifstream in(file, std::ios::binary);
    check(bool(in), "cannot open compact chain");
    while (true) {
        std::array<unsigned char, 8> header{};
        in.read(reinterpret_cast<char*>(header.data()), header.size());
        if (in.gcount() == 0 && in.eof()) break;
        check(in.gcount() == 8, "truncated compact frame header");
        std::uint64_t size = 0;
        for (unsigned i = 0; i < 8; ++i) size |= std::uint64_t(header[i]) << (8 * i);
        check(size > 0 && size <= kExperimentMaxBytes, "invalid compact frame length");
        protocol::Bytes bytes(size);
        in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        check(bool(in), "truncated compact frame payload");
        PrimeBlock block;
        std::string error;
        const auto decoded = decodeBlock(bytes, block, error);
        check(decoded, "decode: " + error);
        const auto appended = replay.append(block, error);
        check(appended, "replay: " + error);
    }
    check(replay.blockCount() > 0, "empty compact chain");
    return replay;
}

void mutationTests(const PrimeBlockReplay& before, const PrimeBlock& block,
                   const std::vector<Identity>& validators, std::size_t& checks) {
    auto reject = [&](PrimeBlock bad, const std::string& name, bool resign = false) {
        if (resign) bad.votes = votes(blockSubjectHash(bad), validators);
        auto state = before;
        const auto tip = state.tip();
        std::string error;
        check(!state.append(bad, error), "accepted invalid block: " + name);
        check(state.tip() == tip && state.frontier() == before.frontier() &&
              state.supply(block.prime.p) == 0, "rejection mutated state: " + name);
        ++checks;
    };
    auto bad = block;
    ++bad.height;
    reject(bad, "height", true);
    bad = block;
    bad.previous_hash[0] ^= 1;
    reject(bad, "parent", true);
    bad = block;
    ++bad.previous_prime;
    reject(bad, "previous prime", true);
    bad = block;
    bad.votes.pop_back();
    reject(bad, "insufficient votes");
    bad = block;
    bad.votes[1] = bad.votes[0];
    reject(bad, "duplicate validator");
    bad = block;
    bad.votes[0].signature[0] ^= 1;
    reject(bad, "invalid signature");
    bad = block;
    bad.votes[0].record_hash[0] ^= 1;
    reject(bad, "wrong signature target");
    bad = block;
    bad.votes = votes(blockSubjectHash(block), {identity(), identity()});
    reject(bad, "unauthorized validators");
    if (!block.composites.empty()) {
        bad = block;
        bad.composites.pop_back();
        reject(bad, "missing composite", true);
        bad = block;
        bad.composites.push_back(block.composites.front());
        reject(bad, "extra composite", true);
        bad = block;
        bad.composites.front().d = 1;
        reject(bad, "invalid divisor", true);
        bad = block;
        ++bad.composites.front().m;
        reject(bad, "wrong integer", true);
        bad = block;
        bad.composites.front().provider_address = validators.front().address;
        reject(bad, "changed attribution under old certificate");
        bad = block;
        bad.composites.front().signature = {1};
        reject(bad, "intermediate signature in finalized block");
        bad = block;
        bad.prime.witness = 1;
        reject(bad, "invalid Pratt witness", true);
        bad = block;
        bad.prime.factors_of_p_minus_1.front().prime = std::numeric_limits<PrimeValue>::max();
        reject(bad, "oversized factor", true);
        bad = block;
        bad.prime.signature[0] ^= 1;
        reject(bad, "invalid prime miner authentication", true);
    }
    if (block.composites.size() >= 2) {
        bad = block;
        std::swap(bad.composites[0], bad.composites[1]);
        reject(bad, "reordered composites", true);
        bad = block;
        bad.composites[1] = bad.composites[0];
        reject(bad, "duplicate composite", true);
    }
    auto bytes = encodeBlock(block);
    PrimeBlock decoded;
    std::string error;
    check(decodeBlock(bytes, decoded, error) && encodeBlock(decoded) == bytes, "canonical round trip");
    bytes.push_back(0);
    check(!decodeBlock(bytes, decoded, error), "accepted trailing bytes");
    ++checks;
    bytes = encodeBlock(block);
    bytes[0] ^= 1;
    check(!decodeBlock(bytes, decoded, error), "accepted wrong format");
    ++checks;
    bytes = encodeBlock(block);
    for (const auto size : {std::size_t(0), std::size_t(7), std::size_t(8), bytes.size() - 1}) {
        check(!decodeBlock(protocol::Bytes(bytes.begin(), bytes.begin() + size), decoded, error), "accepted truncation");
        ++checks;
    }
    bad = block;
    bad.prime.factors_of_p_minus_1.assign(65, {2, 1});
    check(!decodeBlock(encodeBlock(bad), decoded, error), "accepted excessive factor count");
    ++checks;
    // Genesis has no factors/composites: overwrite its vote count with an excessive count.
    if (block.height == 0) {
        const auto offset = encodeBlock(block, false).size();
        bytes[offset] = 255;
        check(!decodeBlock(bytes, decoded, error), "accepted excessive vote count");
        ++checks;
    }
}

void frameTests(const fs::path& directory, const std::vector<Address>& validators, std::size_t& checks) {
    auto reject = [&](const protocol::Bytes& bytes) {
        const auto path = directory / "invalid-frame.dat";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out.close();
        check(bool(out), "cannot write malformed frame fixture");
        bool failed = false;
        try { (void)replayFile(path, validators); }
        catch (const std::runtime_error&) { failed = true; }
        check(failed, "accepted malformed frame file");
        ++checks;
    };
    reject(protocol::Bytes(7, 0));
    reject(protocol::Bytes(8, 0));
    protocol::Bytes oversized;
    appendWord(oversized, kExperimentMaxBytes + 1);
    reject(oversized);
    protocol::Bytes truncated;
    appendWord(truncated, 100);
    truncated.push_back(0);
    reject(truncated);
}

int experiment(PrimeValue limit, const fs::path& root) {
    fs::create_directories(root);
    auto pattern = (root / "run-XXXXXX").string();
    check(mkdtemp(pattern.data()) != nullptr, "cannot create private experiment directory");
    const fs::path directory(pattern);
    std::cout << "artifacts: " << fs::absolute(directory).string() << "\n";
    std::vector<Identity> validators{identity(), identity(), identity()};
    std::vector<Address> validator_addresses;
    for (const auto& validator : validators) validator_addresses.push_back(validator.address);
    std::sort(validator_addresses.begin(), validator_addresses.end());
    std::ofstream config(directory / "validators.txt");
    for (const auto& address : validator_addresses) config << address << '\n';
    config.close();
    check(bool(config), "cannot write fixture validator configuration");
    const std::vector<Identity> miners{identity(), identity(), identity()};
    const auto legacy_file = (directory / "legacy.dat").string();
    node::SequentialNode legacy(legacy_file);
    std::string error;
    const auto empty_loaded = legacy.load(error);
    check(empty_loaded, "load fixture: " + error);
    const auto initialized = legacy.initializeGenesis(validator_addresses, error);
    check(initialized, "fixture genesis: " + error);
    PrimeBlockReplay compact(validator_addresses);
    std::vector<CompositeProof> pending;
    std::vector<PrimeValue> primes;
    std::size_t negative_checks = 0;
    std::uint64_t legacy_closed_bytes = 0;
    std::ofstream out(directory / "prime-blocks.dat", std::ios::binary);
    check(bool(out), "cannot open compact output");
    const auto started = std::chrono::steady_clock::now();
    for (PrimeValue n = 2; n <= limit; ++n) {
        const auto& miner = miners[n % miners.size()];
        if (!math::isPrime(n)) {
            const auto proof = math::makeCompositeProof(n, miner.address);
            check(proof.has_value(), "cannot construct composite proof");
            protocol::CompositeRecordV0 record;
            record.version = node::kTransactionMerkleRecordVersion;
            record.height = legacy.status().height + 1;
            record.previous_record_hash = legacy.status().latest_record_hash;
            record.integer = n;
            record.proof.g = n;
            record.proof.d = proof->d;
            record.proof.e = proof->e;
            record.proof.provider_address = miner.address;
            const auto signature = crypto::signProtocolMessage(miner.keys.private_key,
                crypto::compositeRevealSigningPayload(n, proof->d, proof->e, 0, miner.address), error);
            check(signature.has_value(), "composite signing: " + error);
            record.proof.signature = crypto::packCompositeRevealProof(miner.keys.public_key, 0, *signature);
            finalize(record, validators);
            const auto accepted = legacy.appendComposite(record, error);
            check(accepted, "legacy composite: " + error);
            pending.push_back(*proof);
            continue;
        }
        const OpenProofIndex index(compact, pending);
        const auto proof = math::makePrattProof(n, index);
        check(proof.has_value(), "cannot construct Pratt proof from open interval and history");
        PrimeBlock block;
        block.height = compact.blockCount();
        block.previous_hash = compact.tip();
        block.previous_prime = compact.frontier();
        block.prime.p = n;
        block.prime.witness = proof->witness;
        for (const auto& factor : proof->factors_of_p_minus_1.factors)
            block.prime.factors_of_p_minus_1.push_back({factor.prime, factor.exponent});
        block.prime.provider_address = n == 2 ? "pcdev1_genesis" : miner.address;
        block.composites = pending;
        if (n != 2) {
            protocol::PrimeRecordV0 record;
            record.version = node::kTransactionMerkleRecordVersion;
            record.height = legacy.status().height + 1;
            record.previous_record_hash = legacy.status().latest_record_hash;
            record.integer = n;
            record.proof = block.prime;
            authenticate(record.proof, record.previous_record_hash, miner);
            finalize(record, validators);
            const auto accepted = legacy.appendPrime(record, error);
            check(accepted, "legacy prime: " + error);
            authenticate(block.prime, block.previous_hash, miner);
        }
        block.votes = votes(blockSubjectHash(block), validators);
        if (n == 2 || n == 11 || n == 13) mutationTests(compact, block, validators, negative_checks);
        const auto accepted = compact.append(block, error);
        check(accepted, "compact prime: " + error);
        auto replay_duplicate = compact;
        check(!replay_duplicate.append(block, error), "accepted duplicate prime block");
        error.clear();
        ++negative_checks;
        writeFrame(out, encodeBlock(block));
        pending.clear();
        primes.push_back(n);
        legacy_closed_bytes = fs::file_size(legacy_file);
    }
    out.close();
    check(bool(out), "closing compact output failed");
    frameTests(directory, validator_addresses, negative_checks);
    const auto replay = replayFile(directory / "prime-blocks.dat", validator_addresses);
    check(replay.tip() == compact.tip(), "disk replay tip mismatch");
    // Force full inherited replay, not its snapshot fast path, for the comparison.
    fs::remove(legacy_file + ".snapshot");
    node::SequentialNode reloaded_legacy(legacy_file);
    error.clear();
    const auto loaded = reloaded_legacy.load(error);
    check(loaded, "legacy disk replay: " + error);
    std::vector<Address> accounts{"pcdev1_genesis", protocol::validatorRewardPoolAddress(0)};
    for (const auto& miner : miners) accounts.push_back(miner.address);
    for (const auto p : primes) {
        std::uint64_t sum = 0;
        for (const auto& account : accounts) {
            check(replay.balance(account, p) == reloaded_legacy.balanceMicroUnits(account, p), "reward mismatch");
            sum += replay.balance(account, p);
        }
        check(sum == node::kAssetMicroUnits && replay.supply(p) == reloaded_legacy.totalSupplyMicroUnits(p), "supply mismatch");
        const auto proof = math::makePrattProof(p, replay);
        check(proof && math::verifyPrattProof(*proof), "Pratt reconstruction after disk replay failed");
    }
    for (PrimeValue n = 4; n <= replay.frontier(); ++n) {
        if (math::isPrime(n)) continue;
        const auto proof = replay.findCompositeProof(n);
        check(proof && math::verifyCompositeProof(*proof), "composite lookup after replay failed");
    }
    for (const auto& proof : pending)
        check(!replay.findCompositeProof(proof.m), "unfinished interval was finalized");
    const auto bytes = fs::file_size(directory / "prime-blocks.dat");
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "requested_integer: " << limit << "\n"
              << "finalized_prime: " << replay.frontier() << "\n"
              << "prime_blocks: " << replay.blockCount() << "\n"
              << "embedded_composites: " << replay.frontier() - 1 - replay.blockCount() << "\n"
              << "pending_composites_not_finalized: " << pending.size() << "\n"
              << "legacy_finalized_prefix_bytes: " << legacy_closed_bytes << "\n"
              << "compact_finalized_bytes: " << bytes << "\n"
              << "fixture_validator_config_bytes: " << fs::file_size(directory / "validators.txt") << "\n"
              << "legacy_total_bytes_including_pending: " << fs::file_size(legacy_file) << "\n"
              << std::fixed << std::setprecision(2)
              << "finalized_storage_reduction_percent: " << 100.0 * (1.0 - double(bytes) / legacy_closed_bytes) << "\n"
              << "negative_checks: " << negative_checks << "\n"
              << "experiment_elapsed_ms: " << milliseconds << "\n"
              << "reward_comparison: PASS\nproof_reconstruction: PASS\ndisk_replay: PASS\n"
              << "scope: offline fixed-validator uint64 fixture; no transactions, governance, journal, or networking\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--replay") {
            const fs::path directory(argv[2]);
            std::ifstream config(directory / "validators.txt");
            std::vector<Address> validators;
            Address address;
            while (config >> address) validators.push_back(address);
            check(config.eof() && validators.size() == 3, "invalid fixture validator configuration");
            const auto replay = replayFile(directory / "prime-blocks.dat", validators);
            std::cout << "disk_replay: PASS\nprime_blocks: " << replay.blockCount()
                      << "\nfinalized_prime: " << replay.frontier() << "\n";
            return 0;
        }
        if (argc != 3) {
            std::cerr << "usage: primechain-prime-block-experiment <integer-limit> <artifact-root>\n"
                      << "       primechain-prime-block-experiment --replay <run-directory>\n";
            return 1;
        }
        PrimeValue limit = 0;
        const std::string text(argv[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
        check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
              limit >= 2 && limit <= kExperimentMaxInteger, "integer limit must be between 2 and 100000");
        return experiment(limit, argv[2]);
    } catch (const std::exception& exception) {
        std::cerr << "experiment failed: " << exception.what() << "\n";
        return 1;
    }
}
