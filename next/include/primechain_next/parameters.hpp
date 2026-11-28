#pragma once
// D20 / B3.1: every number is a protocol parameter. One table, voted with notice, never retroactive.
// Values are unsigned integers; shares are basis points (10,000 = 100%); the ring weight exponent
// is a fraction num/den. Each parameter has hard bounds in code (placeholders until a model gives
// ranges); changing a bound is a protocol upgrade.
#include "primechain_next/codec.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace primechain::next {

// Stable identifiers: never renumber, never reuse. Numbers in brackets refer to the table in
// docs/protocol-parameters.md.
enum class Param : std::uint32_t {
    MainReward = 1,             // [1] micro-units per prime (D8)
    FinderShareBp = 2,          // [2] prime finder's share; composites get the rest (D8)
    MaxRangeComposites = 3,     // [3] composites per range (D4)
    MaxIntegerBytes = 4,        // [4] main-chain integer size (D14)
    MinFee = 5,                 // [5] minimum transfer fee (D16)
    MaxOutputs = 6,             // [6] outputs per transfer (D16)
    EpochLength = 7,            // [10] fee pool payout every N blocks (D17)
    GovernanceNotice = 8,       // [11] blocks between quorum and effect (A3-2)
    ProposerSkipAfter = 9,      // [12] missed proposer turns before a skip (A3-3)
    TimeoutBaseMs = 10,         // [13] round timeout base (D11)
    TimeoutCapMs = 11,          // [13] round timeout cap (D11)
    CheckpointInterval = 12,    // [14] permanent certificate every K blocks (A3-3)
    CertificateWindow = 13,     // [14] all certificates of the last W blocks (A3-3)
    RingPrimeValue = 14,        // [17] value of a new ring prime, e * v per prime (rule A4; was the A3 budget)
    RingWeightExpNum = 15,      // [18] weight 1/j^(num/den) of ring j
    RingWeightExpDen = 16,      // [18]
    RingBacklogShareBp = 17,    // [19] share paid for primes older than the ring
    RingUpstreamShareBp = 18,   // [21] owners of the primes below; ring miner gets the rest
    RingFounderShareBp = 19,    // [22] founder
    RingKeptDownBp = 20,        // [23] kept by holders at each level down
    RingGateOpen = 21,          // [24] new rings allowed (0 or 1)
    RingRevealWindow = 22,      // [26] blocks
    RingEligibilityExponent = 23,  // [28] abs(D) <= F^t
    CertificateBytes = 24,      // [29]
    CertificateIntegerBytes = 25,  // [30]
    SegmentPaidRows = 26,       // [31]
    SegmentInertRows = 27,      // [32]
    SegmentsPerSignature = 28,  // [33]
    MiningMessageBytes = 29,    // [34]
    RingBatchBytes = 30,        // [35]
    RingsPaused = 31,           // [36] all rings paused (0 or 1); per-ring pauses are ring state
    CashTotalLimit = 32,        // [37]
    CashKindLimit = 33,         // [38]
    RsaKeyBits = 34,            // [40]
    MembershipStockBp = 35,     // [43] share of a bank's capital
    MembershipPaidInBp = 36,    // [43] part of the subscription paid in
    RingsPerEpoch = 37,         // [25] kappa: rings opened per epoch, at most (B3.4; doc value to be set)
    RingDeposit = 38,           // [27] deposit with each RING_COMMIT, in micro-units of any main-chain prime (0 allowed)
    GeneratorBytes = 39,        // [65] bytes per coefficient of a recorded generator, at most (B3.5; a policy tool)
};

struct ParamSpec {
    Param id;
    const char* name;
    u128 min;
    u128 max;
    u128 start;  // default starting value; the genesis Policy may change it within the bounds
};

using ParameterValues = std::map<std::uint32_t, u128>;  // parameter id -> value

const std::vector<ParamSpec>& parameterSpecs();
const ParamSpec* findParam(std::uint32_t id);
ParameterValues defaultParameters();
// Empty if every value is known, within its bounds, and the combined rules hold
// (shares <= 100%, timeout cap >= base); otherwise the reason.
std::string checkParameters(const ParameterValues& values);
Hash256 parametersDigest(const ParameterValues& values);

// The table with its history: value(id, h) is the value in force for block h.
class ParameterTable {
public:
    ParameterTable() = default;
    explicit ParameterTable(const ParameterValues& genesis);

    u128 value(Param id, std::uint64_t height) const;
    ParameterValues valuesAt(std::uint64_t height) const;
    Hash256 digestAt(std::uint64_t height) const { return parametersDigest(valuesAt(height)); }
    // Record new values in force from `height` on (height must not be below the last change).
    void set(std::uint64_t height, const ParameterValues& changes);
    std::size_t changeCount() const;              // history entries after genesis
    Hash256 historyDigest() const;                // covers every entry (state root)
    void encode(Writer& w) const;
    bool decode(Reader& r, std::string& error);
    bool operator==(const ParameterTable& other) const { return history_ == other.history_; }

private:
    std::map<std::uint32_t, std::vector<std::pair<std::uint64_t, u128>>> history_;  // id -> (from height, value)
};

}  // namespace primechain::next
