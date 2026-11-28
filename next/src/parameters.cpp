#include "primechain_next/parameters.hpp"

#include "primechain_next/merkle.hpp"

#include <stdexcept>

namespace primechain::next {
namespace {

constexpr u128 kBp = 10000;
constexpr u128 kMaxMoney = 1000000000000000ull;  // 10^15 micro-units: placeholder bound

// Placeholder hard bounds, marked as such: generous, to be replaced by the ranges a later model suggests.
const std::vector<ParamSpec> kSpecs = {
    {Param::MainReward, "main reward per prime", 0, kMaxMoney, 1000000},
    {Param::FinderShareBp, "prime finder share (bp)", 0, kBp, 5000},
    {Param::MaxRangeComposites, "composites per range", 1, 1024, 1024},    // 1,024 is also the decoder's bound
    {Param::MaxIntegerBytes, "main-chain integer bytes", 16, 64, 64},      // 64 is also the decoder's bound
    {Param::MinFee, "minimum fee", 0, kMaxMoney, 1},
    {Param::MaxOutputs, "outputs per transfer", 1, 100000, 1000},
    {Param::EpochLength, "epoch length (blocks)", 1, 1000000000, 100},
    {Param::GovernanceNotice, "governance notice (blocks)", 1, 1000000000, 100},
    {Param::ProposerSkipAfter, "proposer skip after (missed turns)", 1, 1000, 3},
    {Param::TimeoutBaseMs, "round timeout base (ms)", 1, 3600000, 1000},
    {Param::TimeoutCapMs, "round timeout cap (ms)", 1, 3600000, 60000},
    {Param::CheckpointInterval, "certificate checkpoint interval (blocks)", 1, 1000000000, 1000},
    {Param::CertificateWindow, "certificate window (blocks)", 1, 1000000000, 1000},
    {Param::RingPrimeValue, "value of a new ring prime (A4)", 0, kMaxMoney, 100000},
    {Param::RingWeightExpNum, "ring weight exponent numerator", 0, 16, 1},
    {Param::RingWeightExpDen, "ring weight exponent denominator", 1, 16, 2},
    {Param::RingBacklogShareBp, "ring backlog share (bp)", 0, kBp, 1000},
    {Param::RingUpstreamShareBp, "ring upstream share (bp)", 0, kBp, 3500},
    {Param::RingFounderShareBp, "ring founder share (bp)", 0, kBp, 1500},
    {Param::RingKeptDownBp, "kept on the way down (bp)", 0, kBp, 5000},
    {Param::RingGateOpen, "new rings allowed", 0, 1, 1},
    {Param::RingRevealWindow, "reveal window (blocks)", 1, 1000000, 10},
    {Param::RingEligibilityExponent, "eligibility exponent t", 1, 8, 2},
    {Param::CertificateBytes, "certificate bytes", 1024, 64u << 20, 256u << 10},
    {Param::CertificateIntegerBytes, "certificate integer bytes", 64, 1u << 20, 4096},
    {Param::SegmentPaidRows, "paid rows per segment", 1, 65536, 32},
    {Param::SegmentInertRows, "inert rows before a paid one", 1, 1u << 20, 1024},
    {Param::SegmentsPerSignature, "segments per signature", 1, 65536, 64},
    {Param::MiningMessageBytes, "mining message bytes", 1024, 64u << 20, 256u << 10},
    {Param::RingBatchBytes, "ring batch bytes", 1024, 256u << 20, 256u << 10},
    {Param::RingsPaused, "all rings paused", 0, 1, 0},
    {Param::CashTotalLimit, "cash total limit", 0, kMaxMoney * 1000, 0},
    {Param::CashKindLimit, "cash limit per kind", 0, kMaxMoney * 1000, 0},
    {Param::RsaKeyBits, "RSA key bits", 3072, 16384, 3072},
    {Param::MembershipStockBp, "membership stock (bp of capital)", 0, kBp, 600},
    {Param::MembershipPaidInBp, "membership paid in (bp)", 0, kBp, 5000},
    // Proposed starting value: the doc leaves kappa to be set; 100 per epoch is permissive (one per
    // block on average with 100-block epochs). Gate 1 is the way to stop new rings.
    {Param::RingsPerEpoch, "new rings per epoch (kappa)", 1, 1000000, 100},
    {Param::RingDeposit, "ring deposit (micro-units)", 0, kMaxMoney, 100000},  // a policy tool; 0 allowed
};

u128 at(const ParameterValues& v, Param id) { return v.at(static_cast<std::uint32_t>(id)); }

}  // namespace

const std::vector<ParamSpec>& parameterSpecs() { return kSpecs; }

const ParamSpec* findParam(std::uint32_t id) {
    for (const auto& s : kSpecs)
        if (static_cast<std::uint32_t>(s.id) == id) return &s;
    return nullptr;
}

ParameterValues defaultParameters() {
    ParameterValues v;
    for (const auto& s : kSpecs) v[static_cast<std::uint32_t>(s.id)] = s.start;
    return v;
}

std::string checkParameters(const ParameterValues& values) {
    if (values.size() != kSpecs.size()) return "parameter table incomplete";
    for (const auto& [id, value] : values) {
        const ParamSpec* s = findParam(id);
        if (!s) return "unknown parameter " + std::to_string(id);
        if (value < s->min || value > s->max) return std::string("parameter out of bounds: ") + s->name;
    }
    if (at(values, Param::RingUpstreamShareBp) + at(values, Param::RingFounderShareBp) > kBp)
        return "ring shares exceed 100%";
    if (at(values, Param::TimeoutCapMs) < at(values, Param::TimeoutBaseMs)) return "timeout cap below base";
    return {};
}

Hash256 parametersDigest(const ParameterValues& values) {
    Writer w;
    w.text("primechain-next/parameters");
    w.varint(1);
    w.varint(values.size());
    for (const auto& [id, value] : values) {
        w.varint(id);
        w.amount(Amount::fromRaw(value));
    }
    return sha3(w.data());
}

ParameterTable::ParameterTable(const ParameterValues& genesis) {
    for (const auto& [id, value] : genesis) history_[id].push_back({0, value});
}

u128 ParameterTable::value(Param id, std::uint64_t height) const {
    const auto it = history_.find(static_cast<std::uint32_t>(id));
    if (it == history_.end() || it->second.empty()) throw std::logic_error("parameter missing from table");
    u128 v = it->second.front().second;
    for (const auto& [from, value] : it->second) {
        if (from > height) break;
        v = value;
    }
    return v;
}

ParameterValues ParameterTable::valuesAt(std::uint64_t height) const {
    ParameterValues out;
    for (const auto& [id, entries] : history_) out[id] = value(static_cast<Param>(id), height);
    return out;
}

void ParameterTable::set(std::uint64_t height, const ParameterValues& changes) {
    for (const auto& [id, value] : changes) {
        auto& entries = history_.at(id);
        if (!entries.empty() && entries.back().first > height) throw std::logic_error("parameter history out of order");
        if (!entries.empty() && entries.back().first == height) entries.back().second = value;
        else entries.push_back({height, value});
    }
}

std::size_t ParameterTable::changeCount() const {
    std::size_t n = 0;
    for (const auto& [id, entries] : history_) n += entries.size() - 1;
    return n;
}

Hash256 ParameterTable::historyDigest() const {
    Writer w;
    w.text("primechain-next/parameter-history");
    encode(w);
    return sha3(w.data());
}

void ParameterTable::encode(Writer& w) const {
    w.varint(history_.size());
    for (const auto& [id, entries] : history_) {
        w.varint(id);
        w.varint(entries.size());
        for (const auto& [from, value] : entries) {
            w.varint(from);
            w.amount(Amount::fromRaw(value));
        }
    }
}

bool ParameterTable::decode(Reader& r, std::string& error) {
    history_.clear();
    std::uint64_t count = 0;
    if (!r.varint(count) || count != kSpecs.size()) { error = "bad parameter table"; return false; }
    std::uint64_t previous = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t id = 0, entries = 0;
        if (!r.varint(id) || (i > 0 && id <= previous) || !findParam(static_cast<std::uint32_t>(id)) || !r.varint(entries) ||
            entries == 0 || entries > (1u << 24)) {
            error = "bad parameter entry";
            return false;
        }
        previous = id;
        auto& list = history_[static_cast<std::uint32_t>(id)];
        for (std::uint64_t j = 0; j < entries; ++j) {
            std::uint64_t from = 0;
            Amount value;
            if (!r.varint(from) || !r.amount(value) || (j == 0 && from != 0) || (j > 0 && from <= list.back().first)) {
                error = "bad parameter history";
                return false;
            }
            list.push_back({from, value.raw()});
        }
    }
    for (std::uint64_t h : {std::uint64_t(0)})
        if (!checkParameters(valuesAt(h)).empty()) { error = "parameter table out of bounds"; return false; }
    return true;
}

}  // namespace primechain::next
