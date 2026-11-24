// Tests for PrimeChain Next A2.1: codec, number types, engine rules (D1-D14), replay.
// Usage: primechain-next-tests codec|engine|chain [limit]
#include "primechain/crypto/signature.hpp"
#include "primechain_next/engine.hpp"
#include "primechain_next/merkle.hpp"

#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <map>
#include <iostream>
#include <stdexcept>

using namespace primechain::next;
namespace crypto = primechain::crypto;

namespace {

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// ---- test helpers: arithmetic by trial division (test-only, independent of the engine) ----
std::uint64_t smallestFactor(std::uint64_t n) {
    for (std::uint64_t k = 2; k * k <= n; ++k) if (n % k == 0) return k;
    return n;
}
bool isPrime(std::uint64_t n) { return n >= 2 && smallestFactor(n) == n; }
std::uint64_t nextPrime(std::uint64_t n) { do ++n; while (!isPrime(n)); return n; }
std::uint64_t primitiveRoot(std::uint64_t p) {
    std::vector<std::uint64_t> qs;
    for (std::uint64_t m = p - 1; m > 1;) {
        const auto q = smallestFactor(m);
        if (qs.empty() || qs.back() != q) qs.push_back(q);
        m /= q;
    }
    for (std::uint64_t a = 2;; ++a) {
        bool ok = true;
        for (auto q : qs) {
            Integer r;
            mpz_powm_ui(r.get_mpz_t(), Integer(a).get_mpz_t(), (p - 1) / q, Integer(p).get_mpz_t());
            if (r == 1) { ok = false; break; }
        }
        if (ok) return a;
    }
}

struct Miner {
    crypto::SignatureKeyPair key;
    primechain::Address address;
};

Miner newMiner() {
    std::string error;
    auto key = crypto::generateProtocolSignatureKeyPair(error);
    require(key.has_value(), "key generation: " + error);
    return {*key, crypto::addressFromProtocolPublicKey(key->public_key)};
}

Bytes sign(const Miner& m, const Bytes& payload) {
    std::string error;
    auto signature = crypto::signProtocolMessage(m.key.private_key, payload, error);
    require(signature.has_value(), "signing: " + error);
    return *signature;
}

RangeSubmission makeRange(const Engine& e, const Miner& m, std::uint64_t start, std::uint64_t end) {
    RangeSubmission r;
    r.network_id = e.policy().network_id;
    r.anchor = e.tip();
    r.start = start;
    r.end = end;
    for (auto n = start; n <= end; ++n) r.divisors.emplace_back(smallestFactor(n));
    r.provider = m.address;
    if (!e.hasKey(m.address)) r.public_key = m.key.public_key;  // registered miners omit their key
    r.signature = sign(m, rangeSigningPayload(r));
    return r;
}

PrimeSubmission makePrime(const Engine& e, const Miner& m, std::uint64_t p) {
    PrimeSubmission s;
    s.network_id = e.policy().network_id;
    s.anchor = e.tip();
    s.prime = p;
    s.witness = primitiveRoot(p);
    s.provider = m.address;
    if (!e.hasKey(m.address)) s.public_key = m.key.public_key;
    s.signature = sign(m, primeSigningPayload(s));
    return s;
}

void submit(Engine& e, const RangeSubmission& r) {
    std::string error;
    require(e.submitRange(r, error), "range rejected: " + error);
}

void closeWith(Engine& e, const Miner& m) {
    std::string error;
    const auto p = e.acceptedFrontier().get_ui() + 1;
    require(e.closeBlock(makePrime(e, m, p), error).has_value(), "prime rejected: " + error);
}

// Mine every block whose closing prime is <= limit; ranges split across miners for variety.
void mineTo(Engine& e, std::uint64_t limit, const std::vector<Miner>& miners) {
    std::size_t turn = 0;
    for (std::uint64_t p = nextPrime(e.chainFrontier().get_ui()); p <= limit; p = nextPrime(p)) {
        const auto a = e.chainFrontier().get_ui() + 1, b = p - 1;
        if (b - a >= 3) {
            const auto mid = a + (b - a) / 2;
            submit(e, makeRange(e, miners[turn++ % miners.size()], a, mid));
            require(e.decideNextRange().has_value(), "first half decided");
            submit(e, makeRange(e, miners[turn++ % miners.size()], a, b));  // overlaps: only its tail is new
        } else {
            submit(e, makeRange(e, miners[turn++ % miners.size()], a, b));
        }
        while (e.decideNextRange()) {}
        require(e.acceptedFrontier() == p - 1, "gap covered before " + std::to_string(p));
        closeWith(e, miners[turn++ % miners.size()]);
    }
}

std::uint64_t peakRssKiB() {
    std::ifstream status("/proc/self/status");
    std::string key;
    std::uint64_t value = 0;
    while (status >> key) {
        if (key == "VmHWM:") { status >> value; return value; }
        status.ignore(1 << 10, '\n');
    }
    return 0;
}

// ---------------------------------------------------------------- codec & numbers
void codecTests() {
    // Amount: checked 128-bit arithmetic, never wraps.
    require(!Amount::max().add(Amount(1)).has_value(), "Amount add overflow detected");
    require(!Amount(0).sub(Amount(1)).has_value(), "Amount underflow detected");
    require(!Amount::max().mul(2).has_value(), "Amount mul overflow detected");
    require(Amount(1000000).mul(1000000000000ull)->str() == "1000000000000000000", "Amount beyond 2^64 ok");
    require(Amount::max().str() == "340282366920938463463374607431768211455", "Amount max decimal");

    // Integer and varint round trips at boundaries.
    const std::vector<Integer> values{0, 1, 127, 128, 255, 256, Integer("18446744073709551615"),
                                      Integer("18446744073709551616"), Integer("123456789012345678901234567890")};
    for (const auto& v : values) {
        Writer w;
        w.integer(v);
        w.amount(Amount(7));
        Reader r(w.data(), 64);
        Integer back;
        Amount a;
        require(r.integer(back) && back == v && r.amount(a) && a == Amount(7) && r.done(), "round trip " + v.get_str());
    }
    for (std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 18446744073709551615ull}) {
        Writer w;
        w.varint(v);
        Reader r(w.data(), 64);
        std::uint64_t back = 0;
        require(r.varint(back) && back == v && r.done(), "varint round trip");
    }
    // Strictness: non-minimal varint, varint overflow, leading zero in an integer, oversize, truncation.
    {
        const Bytes nonMinimal{0x80, 0x00};
        Reader r(nonMinimal, 64);
        std::uint64_t v = 0;
        require(!r.varint(v), "non-minimal varint rejected");
    }
    {
        const Bytes overflow{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
        Reader r(overflow, 64);
        std::uint64_t v = 0;
        require(!r.varint(v), "varint above 2^64 rejected");
    }
    {
        const Bytes leadingZero{0x02, 0x00, 0x05};
        Reader r(leadingZero, 64);
        Integer v;
        require(!r.integer(v), "integer with leading zero rejected");
    }
    {
        Writer w;
        w.integer(Integer(1) << 600);  // 76 bytes
        Reader r(w.data(), 64);
        Integer v;
        require(!r.integer(v), "oversize integer rejected");
    }
    bool threw = false;
    try { Writer w; w.integer(Integer(-1)); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "negative integers are not encodable");

    // Merkle: distinct shapes give distinct roots; empty root is fixed and differs from one empty leaf.
    require(merkleRoot({}) != merkleRoot({Bytes{}}), "empty tree differs from one empty leaf");
    require(merkleRoot({Bytes{1}, Bytes{2}, Bytes{3}}) != merkleRoot({Bytes{1}, Bytes{2}, Bytes{3}, Bytes{3}}),
            "no duplication ambiguity");
    std::cout << "codec: PASS (" << checks << " checks)\n";
}

// ---------------------------------------------------------------- engine rules
void engineTests() {
    const std::vector<Miner> miners{newMiner(), newMiner(), newMiner()};
    const Miner X = newMiner(), Y = newMiner(), Z = newMiner(), W = newMiner(), F = newMiner();
    Engine e;
    std::string error;

    // D7: genesis seeds 2 and 3; height = prime index.
    require(e.height() == 2 && e.chainFrontier() == 3 && e.blocks().size() == 2, "genesis {2,3}");
    require(e.blocks()[0].header.prime == 2 && e.blocks()[1].header.prime == 3, "genesis primes");
    mineTo(e, 13, miners);
    require(e.height() == 6 && e.chainFrontier() == 13, "blocks close at 5, 7, 11, 13 with height = prime index");
    for (std::size_t i = 2; i < e.blocks().size(); ++i)
        require(!e.blocks()[i].ranges.empty() && e.blocks()[i].prime, "every mined gap has a composite");

    // Key registry: each miner's key is stored exactly once, in the block where it is first accepted.
    std::map<Address, int> key_blocks;
    for (const auto& b : e.blocks())
        for (const auto& k : b.new_keys) ++key_blocks[k.address];
    require(key_blocks.size() == miners.size(), "one key record per miner");
    for (const auto& [address, n] : key_blocks) require(n == 1 && e.hasKey(address), "key stored once");
    {
        auto unknown = makeRange(e, W, e.acceptedFrontier().get_ui() + 1, e.acceptedFrontier().get_ui() + 1);
        unknown.public_key.clear();
        unknown.signature = sign(W, rangeSigningPayload(unknown));
        require(!e.validateRange(unknown, error), "unknown miner without key rejected");
    }
    // The block stores the accepted prime and the hash of the full signed submission (signature included).
    {
        const Engine before = e;
        Engine probe = before;
        const auto a = probe.chainFrontier().get_ui() + 1, p = nextPrime(probe.chainFrontier().get_ui());
        submit(probe, makeRange(probe, miners[0], a, p - 1));
        while (probe.decideNextRange()) {}
        const auto sub = makePrime(probe, miners[1], p);
        const auto block = probe.closeBlock(sub, error);
        require(block && block->prime && block->prime->submission_hash == submissionHash(sub) &&
                    block->prime->provider == miners[1].address,
                "accepted prime keeps the signed submission's hash");
        auto altered = sub;
        altered.signature[5] ^= 1;
        require(submissionHash(altered) != submissionHash(sub), "submission hash covers the signature bytes");
    }

    // Rejections.
    mineTo(e, 1009, miners);
    require(e.chainFrontier() == 1009, "frontier at 1009");
    {
        auto bad = makeRange(e, X, 1010, 1012);
        bad.divisors[1] = 3;  // 1011 = 3 * 337, so 3 is right; break 1012 instead
        bad.divisors[2] = 5;
        bad.signature = sign(X, rangeSigningPayload(bad));
        require(!e.validateRange(bad, error), "wrong divisor rejected");
        auto sig = makeRange(e, X, 1010, 1012);
        sig.signature[10] ^= 1;
        require(!e.validateRange(sig, error), "bad signature rejected");
        auto stranger = makeRange(e, X, 1010, 1012);
        stranger.provider = Y.address;
        require(!e.validateRange(stranger, error), "provider must match key");
        auto withPrime = makeRange(e, X, 1010, 1012);
        withPrime.end = 1013;
        withPrime.divisors.emplace_back(7);  // 1013 is prime: no valid divisor exists
        withPrime.signature = sign(X, rangeSigningPayload(withPrime));
        require(!e.validateRange(withPrime, error), "range containing a prime rejected");
        auto anchor = makeRange(e, X, 1010, 1012);
        anchor.anchor[0] ^= 1;
        anchor.signature = sign(X, rangeSigningPayload(anchor));
        require(!e.validateRange(anchor, error), "wrong anchor rejected");
    }

    // D4: X 1010-1011 vs Y 1010-1012 -> Y wins, X gets nothing.
    submit(e, makeRange(e, X, 1010, 1011));
    submit(e, makeRange(e, Y, 1010, 1012));
    {
        PrimeSubmission early = makePrime(e, F, 1013);
        require(!e.validatePrime(early, error), "prime before the gap is covered rejected");
    }
    auto won = e.decideNextRange();
    require(won && won->provider == Y.address && won->accepted_start == 1010 && won->end == 1012, "highest end wins");
    require(e.pendingCount() == 0 && !e.decideNextRange(), "X became stale");
    {
        auto badWitness = makePrime(e, F, 1013);
        badWitness.witness = 4;  // a square is never a primitive root
        badWitness.signature = sign(F, primeSigningPayload(badWitness));
        require(!e.validatePrime(badWitness, error), "bad Pratt witness rejected");
    }
    closeWith(e, F);
    require(e.balance(X.address, 1013) == Amount(0), "loser X gets nothing");
    require(e.balance(Y.address, 1013) == Amount(499998), "Y gets the whole composite share (3 x 166,666)");
    require(e.balance(F.address, 1013) == Amount(500002), "the finder gets its half plus the remainder 2");

    // D4 tail and D8 rewards: gap 1130-1150, prime 1151.
    mineTo(e, 1129, miners);
    require(e.chainFrontier() == 1129, "frontier at 1129");
    submit(e, makeRange(e, Y, 1130, 1140));
    require(e.decideNextRange()->end == 1140, "Y 1130-1140 accepted");
    submit(e, makeRange(e, W, 1130, 1145));
    submit(e, makeRange(e, Z, 1130, 1150));
    auto tail = e.decideNextRange();
    require(tail && tail->provider == Z.address && tail->accepted_start == 1141 && tail->end == 1150 &&
                tail->divisors.size() == 10, "Z's tail 1141-1150 accepted");
    require(e.pendingCount() == 0, "W (end 1145) became stale");
    closeWith(e, F);
    require(e.balance(Y.address, 1151) == Amount(261899), "D8: Y 11 composites -> 261,899");
    require(e.balance(Z.address, 1151) == Amount(238090), "D8: Z 10 composites -> 238,090");
    require(e.balance(F.address, 1151) == Amount(500011), "D8: prime finder 500,011");
    require(*e.balance(Y.address, 1151).add(e.balance(Z.address, 1151))->add(e.balance(F.address, 1151)) == Amount(1000000),
            "D8: the whole reward is paid, nothing else issued");
    require(e.balance(W.address, 1151) == Amount(0), "stale W gets nothing");

    // Tie: same end, earliest arrival wins (gap {1152}, prime 1153).
    submit(e, makeRange(e, X, 1152, 1152));
    submit(e, makeRange(e, Y, 1152, 1152));
    require(e.decideNextRange()->provider == X.address, "tie goes to the earliest arrival");
    closeWith(e, F);
    require(e.balance(Y.address, 1153) == Amount(0), "later tie loser gets nothing");

    // Replay reproduces the chain bit for bit; a tampered block is rejected.
    auto replayed = Engine::replay(e.policy(), e.blocks(), error);
    require(replayed && replayed->stateRoot() == e.stateRoot() && replayed->tip() == e.tip(), "replay identical: " + error);
    auto tampered = e.blocks();
    tampered.back().ranges.front().provider = Y.address;
    require(!Engine::replay(e.policy(), tampered, error), "tampered attribution detected");
    tampered = e.blocks();
    tampered[10].ranges.front().divisors.front() += 1;
    require(!Engine::replay(e.policy(), tampered, error), "tampered divisor detected");

    // Strict block decoding: round trip, trailing bytes, unknown version.
    const Bytes raw = encode(e.blocks().back());
    Block decoded;
    require(decode(raw, decoded, e.policy().limits, error) && encode(decoded) == raw, "block round trip");
    Bytes trailing = raw;
    trailing.push_back(0);
    require(!decode(trailing, decoded, e.policy().limits, error), "trailing bytes rejected");
    Bytes versioned = raw;
    versioned[1] = 2;  // block version byte
    require(!decode(versioned, decoded, e.policy().limits, error), "unknown version rejected");
    std::cout << "engine: PASS (" << checks << " checks)\n";
}


// ---------------------------------------------------------------- A2.2: transfers, validators, persistence
ValidatorRecord makeValidator(const std::string& network_id, Miner& payout_out) {
    const Miner consensus = newMiner();
    payout_out = newMiner();
    ValidatorRecord v;
    v.consensus_key = consensus.key.public_key;
    v.payout_address = payout_out.address;
    v.payout_key = payout_out.key.public_key;
    const Bytes payload = validatorBindingPayload(network_id, v);
    v.consensus_signature = sign(consensus, payload);
    v.payout_signature = sign(payout_out, payload);
    return v;
}

Transfer makeTransfer(const Engine& e, const Miner& from, std::vector<TransferInput> inputs,
                      std::vector<TransferOutput> outputs, const Integer& fee_asset, std::uint64_t fee, std::uint64_t nonce) {
    Transfer t;
    t.network_id = e.policy().network_id;
    t.sender = from.address;
    t.inputs = std::move(inputs);
    t.outputs = std::move(outputs);
    t.fee_asset = fee_asset;
    t.fee = Amount(fee);
    t.nonce = nonce;
    if (!e.hasKey(from.address)) t.public_key = from.key.public_key;
    t.signature = sign(from, transferSigningPayload(t));
    return t;
}

// A prime asset in which `who` holds at least `min` micro-units.
Integer assetWith(const Engine& e, const Miner& who, std::uint64_t min) {
    for (std::uint64_t p = 5; p <= e.chainFrontier().get_ui(); p = nextPrime(p))
        if (!(e.balance(who.address, p) < Amount(min))) return Integer(p);
    throw std::runtime_error("no asset with enough balance");
}

void mineOne(Engine& e, const std::vector<Miner>& miners) { mineTo(e, nextPrime(e.chainFrontier().get_ui()), miners); }

void transferTests() {
    Policy policy;
    policy.max_outputs = 8;
    policy.epoch_length = 1000;  // no distribution during these checks
    const std::vector<Miner> miners{newMiner(), newMiner(), newMiner()};
    Engine e(policy);
    mineTo(e, 200, miners);
    const Miner& A = miners[0];
    const Integer asset = assetWith(e, A, 10000);
    const Amount before = e.balance(A.address, asset);
    std::vector<Miner> receivers;
    std::vector<TransferOutput> outs;
    for (int i = 0; i < 5; ++i) {
        receivers.push_back(newMiner());
        outs.push_back({asset, Amount(10), receivers.back().address});
    }
    std::string error;
    // Bulk send: one transfer, five outputs, fee 1.
    const Transfer bulk = makeTransfer(e, A, {{asset, Amount(51)}}, outs, asset, 1, 1);
    require(e.submitTransfer(bulk, error), "bulk transfer accepted: " + error);
    const Amount fee_pool_before = e.feePoolBalance(asset);
    mineOne(e, miners);
    const Block& b = e.blocks().back();
    require(b.transfers.size() == 1 && encode(b.transfers[0]) == encode(bulk), "transfer included with the closing prime");
    require(b.header.transactions_root == merkleRoot({encode(bulk)}), "transactions_root over the transfer");
    for (const auto& r : receivers) require(e.balance(r.address, asset) == Amount(10), "each output credited");
    require(e.balance(A.address, asset) == *before.sub(Amount(51)), "sender debited inputs");
    require(e.feePoolBalance(asset) == *fee_pool_before.add(Amount(1)), "fee reaches the validator fee pool");
    require(e.nonce(A.address) == 1, "nonce advanced");

    // Rejections at submission.
    auto rejected = [&](Transfer t, const std::string& what, bool resign = true) {
        if (resign) t.signature = sign(A, transferSigningPayload(t));
        std::string reason;
        require(!e.submitTransfer(t, reason), what + " rejected");
    };
    Transfer t = makeTransfer(e, A, {{asset, Amount(11)}}, {{asset, Amount(10), receivers[0].address}}, asset, 1, 2);
    { auto bad = t; bad.signature[7] ^= 1; rejected(bad, "bad signature", false); }
    { auto bad = t; bad.nonce = 1; rejected(bad, "replayed nonce"); }
    { auto bad = t; bad.nonce = 0; rejected(bad, "zero nonce"); }
    { auto bad = t; bad.inputs[0].amount = Amount(12); rejected(bad, "conservation violation"); }
    { auto bad = t; bad.fee = Amount(0); bad.inputs[0].amount = Amount(10); rejected(bad, "fee below minimum"); }
    { auto bad = t; bad.outputs.assign(9, {asset, Amount(1), receivers[0].address}); bad.inputs[0].amount = Amount(10);
      rejected(bad, "too many outputs"); }
    { auto bad = t; bad.outputs[0].asset = 104729; rejected(bad, "unknown asset"); }
    { auto bad = t; bad.outputs[0].amount = Amount(0); bad.inputs[0].amount = Amount(1); rejected(bad, "zero output"); }
    { auto bad = t; bad.network_id = "other"; rejected(bad, "wrong network"); }
    {   // unknown sender without key
        const Miner X = newMiner();
        Transfer u = makeTransfer(e, X, {{asset, Amount(2)}}, {{asset, Amount(1), A.address}}, asset, 1, 1);
        u.public_key.clear();
        u.signature = sign(X, transferSigningPayload(u));
        require(!e.submitTransfer(u, error), "unknown sender without key rejected");
    }
    // Dropped at inclusion: a nonce gap and an overspend (both pass submission checks).
    const Transfer gap = makeTransfer(e, A, {{asset, Amount(2)}}, {{asset, Amount(1), receivers[1].address}}, asset, 1, 3);
    const Miner& B = miners[1];
    const Integer b_asset = assetWith(e, B, 1);
    const Transfer overspend = makeTransfer(e, B, {{b_asset, *e.balance(B.address, b_asset).add(Amount(1))}},
                                            {{b_asset, e.balance(B.address, b_asset), A.address}}, b_asset, 1, 1);
    require(e.submitTransfer(gap, error) && e.submitTransfer(overspend, error) && e.submitTransfer(t, error),
            "submission-time checks pass: " + error);
    mineOne(e, miners);
    require(e.blocks().back().transfers.size() == 1 && encode(e.blocks().back().transfers[0]) == encode(t),
            "only the in-order, funded transfer is included");
    require(e.nonce(A.address) == 2 && e.nonce(B.address) == 0, "dropped transfers change nothing");

    // Replay with transfers; a tampered transfer amount is detected.
    auto replayed = Engine::replay(policy, e.blocks(), error);
    require(replayed && replayed->stateRoot() == e.stateRoot() && replayed->tip() == e.tip(), "replay with transfers: " + error);
    auto tampered = e.blocks();
    for (auto& blk : tampered)
        if (!blk.transfers.empty()) { blk.transfers[0].outputs[0].amount = Amount(11); break; }
    require(!Engine::replay(policy, tampered, error), "tampered transfer detected (signature)");
    std::cout << "transfers: PASS (" << checks << " checks)\n";
}

void distributionTests() {
    Policy policy;
    policy.epoch_length = 10;
    std::vector<Miner> payouts(3);
    Genesis genesis;
    for (auto& p : payouts) genesis.validators.push_back(makeValidator(policy.network_id, p));
    {   // a broken binding is refused
        Genesis bad = genesis;
        bad.validators[0].payout_signature[3] ^= 1;
        bool threw = false;
        try { Engine x(policy, bad); } catch (const std::invalid_argument&) { threw = true; }
        require(threw, "invalid validator binding refused");
    }
    const std::vector<Miner> miners{newMiner(), newMiner()};
    Engine e(policy, genesis);
    require(e.blocks()[0].header.validator_epoch_digest == e.blocks()[2 - 1].header.validator_epoch_digest,
            "validator set digest in headers");
    std::vector<Address> order;
    for (const auto& v : e.validators()) order.push_back(v.payout_address);
    require(std::is_sorted(order.begin(), order.end()), "validators in payout-address order");
    // Mine through the first epoch (height 10) and check that validators get no share of issuance (D8).
    while (e.height() < 10) mineOne(e, miners);
    for (std::size_t i = 0; i < e.blocks().size(); ++i) {
        const auto& blk = e.blocks()[i];
        if (!blk.prime) continue;
        const Integer& p = blk.prime->prime;
        for (const auto& payout : order) require(e.balance(payout, p) == Amount{}, "no validator share of the reward");
    }
    // Fees are distributed too: a fee of 7 in one asset during epoch 2 -> 3, 2, 2.
    const Miner& A = miners[0];
    const Integer asset = assetWith(e, A, 100);
    std::vector<Amount> before;
    for (const auto& a : order) before.push_back(e.balance(a, asset));
    std::string error;
    require(e.submitTransfer(makeTransfer(e, A, {{asset, Amount(17)}}, {{asset, Amount(10), miners[1].address}}, asset, 7, 1), error),
            "fee transfer: " + error);
    mineOne(e, miners);
    require(e.feePoolBalance(asset) == Amount(7), "fee accumulates until the epoch ends");
    while (e.height() < 20) mineOne(e, miners);
    require(e.feePoolBalance(asset) == Amount{}, "fee pool emptied");
    for (std::size_t k = 0; k < order.size(); ++k)
        require(e.balance(order[k], asset) == *before[k].add(Amount(k == 0 ? 3 : 2)), "fee shares 3, 2, 2");
    auto replayed = Engine::replay(policy, genesis, e.blocks(), error);
    require(replayed && replayed->stateRoot() == e.stateRoot(), "replay with distributions: " + error);
    require(!Engine::replay(policy, Genesis{}, e.blocks(), error), "replay under a different validator set fails");
    std::cout << "distribution: PASS (" << checks << " checks)\n";
}

void flipByte(const std::string& path, std::uintmax_t offset) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(static_cast<char>(c ^ 0x5a));
}

void persistenceTests() {
    namespace fs = std::filesystem;
    std::string tmpl = (fs::temp_directory_path() / "pcnext-store-XXXXXX").string();
    require(mkdtemp(tmpl.data()) != nullptr, "temp dir");
    const fs::path root(tmpl);
    Policy policy;
    policy.snapshot_interval = 7;
    policy.epoch_length = 5;
    std::vector<Miner> payouts(3);
    Genesis genesis;
    for (auto& p : payouts) genesis.validators.push_back(makeValidator(policy.network_id, p));
    const std::vector<Miner> miners{newMiner(), newMiner()};
    const std::string dir = (root / "chain").string();
    std::string error;
    Hash256 tip{}, state{};
    std::uint64_t height = 0;
    {
        auto e = Engine::open(dir, policy, genesis, error);
        require(e.has_value(), "create store: " + error);
        mineTo(*e, 300, miners);
        const Miner& A = miners[0];
        const Integer asset = assetWith(*e, A, 100);
        require(e->submitTransfer(makeTransfer(*e, A, {{asset, Amount(30)}}, {{asset, Amount(29), miners[1].address}}, asset, 1, 1), error),
                "transfer: " + error);
        mineOne(*e, miners);
        tip = e->tip(); state = e->stateRoot(); height = e->height();
    }
    require(fs::exists(dir + "/snapshot.dat"), "snapshot written");
    {
        auto e = Engine::open(dir, policy, genesis, error);
        require(e && e->tip() == tip && e->stateRoot() == state && e->height() == height, "reopen identical: " + error);
        require(e->blocks().size() < height, "reopen loads the snapshot and replays only the tail");
        mineOne(*e, miners);  // appending continues after reopen
        tip = e->tip(); state = e->stateRoot(); height = e->height();
    }
    {
        auto e = Engine::open(dir, policy, genesis, error);
        require(e && e->tip() == tip && e->stateRoot() == state, "reopen after continuing: " + error);
    }
    // Interrupted append: an incomplete final frame is dropped, the chain is unchanged.
    const auto size = fs::file_size(dir + "/blocks.dat");
    { std::ofstream f(dir + "/blocks.dat", std::ios::app | std::ios::binary); f.write("\x64\0\0\0\0\0\0\0partial", 15); }
    {
        auto e = Engine::open(dir, policy, genesis, error);
        require(e && e->tip() == tip && fs::file_size(dir + "/blocks.dat") == size, "incomplete final frame dropped: " + error);
    }
    // A damaged complete frame, a damaged snapshot, a different genesis: refused.
    const std::string copy1 = (root / "c1").string(), copy2 = (root / "c2").string();
    fs::copy(dir, copy1);
    fs::copy(dir, copy2);
    flipByte(copy1 + "/blocks.dat", size - 40);
    require(!Engine::open(copy1, policy, genesis, error) && error.find("corrupt") != std::string::npos,
            "damaged block frame refused: " + error);
    flipByte(copy2 + "/snapshot.dat", 60);
    require(!Engine::open(copy2, policy, genesis, error) && error.find("snapshot") != std::string::npos,
            "damaged snapshot refused: " + error);
    require(!Engine::open(dir, policy, Genesis{}, error), "different validator set refused");
    Policy other = policy;
    other.reward_micro_units = 2000000;
    require(!Engine::open(dir, other, genesis, error), "different policy refused");
    fs::remove_all(root);
    std::cout << "persistence: PASS (" << checks << " checks)\n";
}

// ---------------------------------------------------------------- generated chain + replay
void chainTest(std::uint64_t limit) {
    const std::vector<Miner> miners{newMiner(), newMiner(), newMiner(), newMiner()};
    Engine e;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t transfers = 0;
    std::vector<Miner> receivers;
    for (int i = 0; i < 10; ++i) receivers.push_back(newMiner());
    while (nextPrime(e.chainFrontier().get_ui()) <= limit) {
        if (e.height() > 20 && e.height() % 50 == 0) {  // a 10-output bulk send every 50 blocks
            const Miner& A = miners[0];
            const Integer asset = assetWith(e, A, 1000);
            std::vector<TransferOutput> outs;
            for (const auto& r : receivers) outs.push_back({asset, Amount(7), r.address});
            std::string error;
            require(e.submitTransfer(makeTransfer(e, A, {{asset, Amount(71)}}, outs, asset, 1, e.nonce(A.address) + 1), error),
                    "bulk transfer: " + error);
            ++transfers;
        }
        mineOne(e, miners);
    }
    const auto t1 = std::chrono::steady_clock::now();
    // Round-trip every block through the canonical encoding, then replay from the decoded copies.
    std::vector<Block> decoded(e.blocks().size());
    std::string error;
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const Bytes raw = encode(e.blocks()[i]);
        bytes += raw.size();
        require(decode(raw, decoded[i], e.policy().limits, error), "decode block: " + error);
    }
    auto replayed = Engine::replay(e.policy(), decoded, error);
    const auto t2 = std::chrono::steady_clock::now();
    require(replayed && replayed->stateRoot() == e.stateRoot() && replayed->tip() == e.tip(), "replay identical: " + error);
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    std::size_t included = 0;
    for (const auto& b : e.blocks()) included += b.transfers.size();
    require(included == transfers, "every bulk transfer included");
    std::cout << "chain: PASS\nblocks: " << e.height() << " (last prime " << e.chainFrontier().get_str() << ")\n"
              << "bulk transfers (10 outputs each): " << included << "\n"
              << "encoded chain bytes: " << bytes << " (" << bytes / e.blocks().size() << " per block)\n"
              << "mine+sign+verify seconds: " << secs(t0, t1) << "\nreplay seconds: " << secs(t1, t2) << "\n"
              << "peak RSS KiB: " << peakRssKiB() << "\nchecks: " << checks << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "engine";
        if (mode == "codec") codecTests();
        else if (mode == "engine") engineTests();
        else if (mode == "transfers") { transferTests(); distributionTests(); }
        else if (mode == "persistence") persistenceTests();
        else if (mode == "chain") chainTest(argc > 2 ? std::stoull(argv[2]) : 20000);
        else throw std::runtime_error("usage: primechain-next-tests codec|engine|transfers|persistence|chain [limit]");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "next test failed: " << e.what() << '\n';
        return 1;
    }
}
