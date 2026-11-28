#include "primechain_next/rings.hpp"

#include "primechain_next/merkle.hpp"

#include <stdexcept>

namespace primechain::next {
namespace {

// pi^2 enclosed in [kPi2Low, kPi2High] / 10^30.
const Integer kScale("1000000000000000000000000000000");
const Integer kPi2Low("9869604401089358618834490999876");
const Integer kPi2High("9869604401089358618834490999877");

Integer absValue(const Integer& x) { return x < 0 ? Integer(-x) : x; }
Integer mod(const Integer& a, const Integer& m) { Integer r = a % m; if (r < 0) r += m; return r; }
bool oneMod4(const Integer& d) { return mod(d, 4) == 1; }

void signedInteger(Writer& w, const Integer& v) {
    w.u8(v < 0 ? 1 : 0);
    w.integer(absValue(v));
}
bool signedInteger(Reader& r, Integer& v) {
    std::uint8_t sign = 0;
    if (!r.u8(sign) || sign > 1 || !r.integer(v)) return false;
    if (sign == 1 && v == 0) return false;  // negative zero is not canonical
    if (sign == 1) v = -v;
    return true;
}
void element(Writer& w, const QuadElement& a) { signedInteger(w, a.c0); signedInteger(w, a.c1); }
bool element(Reader& r, QuadElement& a) { return signedInteger(r, a.c0) && signedInteger(r, a.c1); }

std::size_t magnitudeBytes(const Integer& v) { return integerMagnitude(absValue(v)).size(); }

// A generator of the prime above q: norm q (D < 0) or +-q (D > 0), and its derived root is the
// smaller root (split q) or the double root (ramified q). Returns the root.
std::optional<Integer> checkGenerator(const Integer& d, const Integer& q, const QuadElement& g,
                                      const std::optional<QuadElement>& unit, std::string& error) {
    const Integer n = quadNorm(d, g);
    if (d < 0 ? n != q : (n != q && n != -q)) { error = "generator norm is not +-" + q.get_str(); return std::nullopt; }
    const auto r = derivedRoot(q, g);
    if (!r || !isRoot(d, q, *r)) { error = "generator does not lie over a root of f mod " + q.get_str(); return std::nullopt; }
    const Integer other = otherRoot(d, q, *r);
    if (rowType(d, q) == RowType::Ramified ? other != *r : other <= *r) {
        error = "generator is not for the smaller root at " + q.get_str();
        return std::nullopt;
    }
    if (!isReduced(d, g, unit)) { error = "generator is padded by a unit at " + q.get_str(); return std::nullopt; }
    return r;
}

}  // namespace

Integer quadNorm(const Integer& d, const QuadElement& a) {
    if (oneMod4(d)) return a.c0 * a.c0 + a.c0 * a.c1 + a.c1 * a.c1 * ((1 - d) / 4);
    return a.c0 * a.c0 - (d / 4) * a.c1 * a.c1;
}

QuadElement quadMul(const Integer& d, const QuadElement& a, const QuadElement& b) {
    // omega^2 = omega + (D - 1)/4 (D = 1 mod 4), or D/4.
    const Integer cc = a.c1 * b.c1;
    if (oneMod4(d)) return {a.c0 * b.c0 + cc * ((d - 1) / 4), a.c0 * b.c1 + a.c1 * b.c0 + cc};
    return {a.c0 * b.c0 + cc * (d / 4), a.c0 * b.c1 + a.c1 * b.c0};
}

QuadElement quadConjugate(const Integer& d, const QuadElement& a) {
    if (oneMod4(d)) return {a.c0 + a.c1, -a.c1};  // conj(omega) = 1 - omega
    return {a.c0, -a.c1};
}

Integer quadHeight(const QuadElement& a) { return a.c0 * a.c0 + a.c1 * a.c1; }

RowType rowType(const Integer& d, const Integer& q) {
    const int k = mpz_kronecker(d.get_mpz_t(), q.get_mpz_t());
    return k == 0 ? RowType::Ramified : (k > 0 ? RowType::Split : RowType::Inert);
}

bool isRoot(const Integer& d, const Integer& q, const Integer& r) {
    if (r < 0 || r >= q) return false;
    const Integer f = oneMod4(d) ? Integer(r * r - r + (1 - d) / 4) : Integer(r * r - d / 4);
    return mod(f, q) == 0;
}

Integer otherRoot(const Integer& d, const Integer& q, const Integer& r) {
    return mod((oneMod4(d) ? Integer(1) : Integer(0)) - r, q);  // the roots sum to the trace of omega
}

std::optional<Integer> derivedRoot(const Integer& q, const QuadElement& g) {
    Integer c1 = mod(g.c1, q), inverse;
    if (c1 == 0 || mpz_invert(inverse.get_mpz_t(), c1.get_mpz_t(), q.get_mpz_t()) == 0) return std::nullopt;
    return mod(-g.c0 * inverse, q);
}

bool isReduced(const Integer& d, const QuadElement& g, const std::optional<QuadElement>& unit) {
    if (!unit) return true;
    // u^-1 = N(u) * conj(u), since N(u) = +-1. The height along g*u^k is a strictly convex
    // function of k (a sum of two exponentials plus a constant), so a local minimum is global.
    const Integer n = quadNorm(d, *unit);
    QuadElement inv = quadConjugate(d, *unit);
    inv.c0 *= n; inv.c1 *= n;
    const Integer h = quadHeight(g);
    return quadHeight(quadMul(d, g, *unit)) >= h && quadHeight(quadMul(d, g, inv)) >= h;
}

bool isFundamental(const Integer& d, const std::vector<std::pair<Integer, std::uint64_t>>& factors,
                   const PrimeOracle& is_prime, std::string& error) {
    if (d == 0 || d == 1) { error = "D must not be 0 or 1"; return false; }
    Integer product = 1, previous = 0;
    std::uint64_t two_exponent = 0;
    bool odd_squarefree = true;
    for (const auto& [p, e] : factors) {
        if (p <= previous) { error = "factorization not in increasing order"; return false; }
        if (e == 0 || e > 3 || !is_prime(p)) { error = "factorization contains a non-prime or a bad exponent"; return false; }
        previous = p;
        Integer power;
        mpz_pow_ui(power.get_mpz_t(), p.get_mpz_t(), static_cast<unsigned long>(e));
        product *= power;
        if (p == 2) two_exponent = e;
        else if (e > 1) odd_squarefree = false;
    }
    if (product != absValue(d)) { error = "factorization does not multiply to |D|"; return false; }
    if (!odd_squarefree) { error = "D has an odd square factor"; return false; }
    if (two_exponent == 0) {
        if (mod(d, 4) != 1) { error = "odd D must be 1 mod 4"; return false; }
        return true;
    }
    if (two_exponent == 1) { error = "invalid power of 2 in D"; return false; }
    const Integer m4 = mod(d / 4, 4);
    if (two_exponent == 2 && m4 != 3) { error = "D = 4m with odd m requires m = 3 mod 4"; return false; }
    if (two_exponent == 3 && m4 != 2) { error = "D = 4m with even m requires m = 2 mod 4"; return false; }
    return true;
}

bool eligible(const Integer& d, const Integer& frontier, std::uint64_t exponent) {
    Integer bound;
    mpz_pow_ui(bound.get_mpz_t(), frontier.get_mpz_t(), static_cast<unsigned long>(exponent));
    return absValue(d) <= bound;
}

bool minkowskiBelow(const Integer& d, const Integer& x) {
    // Real: M = sqrt(D)/2 < x <=> D < 4x^2 (D = 4x^2 is never fundamental, R3 section 5).
    if (d > 0) return d < 4 * x * x;
    // Imaginary: M = (2/pi) sqrt|D| < x <=> 4|D| < pi^2 x^2 (never equal: pi^2 is irrational).
    const Integer lhs = 4 * absValue(d) * kScale;
    if (lhs < kPi2Low * x * x) return true;
    if (lhs > kPi2High * x * x) return false;
    throw std::runtime_error("pi^2 enclosure too coarse for this comparison");
}

Hash256 quadraticRingId(const Integer& d) {
    Writer w;
    w.text("primechain-next/ring-id");
    w.varint(kQuadraticCertificateType);
    w.varint(2);  // degree
    signedInteger(w, d);
    return sha3(w.data());
}

// ---------------------------------------------------------------- encodings

Bytes encode(const QuadCertificate& cert) {
    Writer w;
    w.varint(cert.version);
    w.varint(kQuadraticCertificateType);
    signedInteger(w, cert.d);
    w.varint(cert.factors.size());
    for (const auto& [p, e] : cert.factors) { w.integer(p); w.varint(e); }
    w.varint(cert.generators.size());
    for (const auto& g : cert.generators) { w.integer(g.q); element(w, g.g); }
    w.u8(cert.unit ? 1 : 0);
    if (cert.unit) element(w, *cert.unit);
    return w.data();
}

bool decode(const Bytes& data, QuadCertificate& out, const CertificateLimits& limits, std::string& error) {
    out = {};
    if (data.size() > limits.certificate_bytes) { error = "certificate exceeds the size limit"; return false; }
    Reader r(data, static_cast<std::size_t>(limits.integer_bytes));
    std::uint64_t type = 0, count = 0;
    auto fail = [&](const std::string& why) { error = r.error().empty() ? why : why + ": " + r.error(); return false; };
    if (!r.varint(out.version) || out.version != kVersion1) return fail("unsupported certificate version");
    if (!r.varint(type) || type != kQuadraticCertificateType) return fail("not a quadratic certificate");
    if (!signedInteger(r, out.d)) return fail("bad discriminant");
    if (!r.varint(count) || count > data.size()) return fail("bad factor count");
    for (std::uint64_t i = 0; i < count; ++i) {
        Integer p; std::uint64_t e = 0;
        if (!r.integer(p) || !r.varint(e)) return fail("bad factor");
        out.factors.emplace_back(p, e);
    }
    if (!r.varint(count) || count > data.size()) return fail("bad generator count");
    for (std::uint64_t i = 0; i < count; ++i) {
        QuadGenerator g;
        if (!r.integer(g.q) || !element(r, g.g)) return fail("bad generator");
        out.generators.push_back(g);
    }
    std::uint8_t has_unit = 0;
    if (!r.u8(has_unit) || has_unit > 1) return fail("bad unit flag");
    if (has_unit) {
        QuadElement u;
        if (!element(r, u)) return fail("bad unit");
        out.unit = u;
    }
    if (!r.done()) return fail("trailing bytes after certificate");
    return true;
}

void encodeSegmentBody(Writer& w, const Segment& s) {
    w.varint(s.rows.size());
    for (std::size_t i = 0; i < s.rows.size(); i += 4) {
        std::uint8_t byte = 0;
        for (std::size_t k = 0; k < 4 && i + k < s.rows.size(); ++k)
            byte |= static_cast<std::uint8_t>(static_cast<std::uint8_t>(s.rows[i + k]) << (2 * k));
        w.u8(byte);
    }
    w.varint(s.generators.size());
    for (const auto& g : s.generators) element(w, g);
}

bool decodeSegmentBody(Reader& r, Segment& out, const SegmentLimits& limits, std::string& error) {
    auto fail = [&](const std::string& why) { error = r.error().empty() ? why : why + ": " + r.error(); return false; };
    std::uint64_t count = 0;
    out.rows.clear();
    out.generators.clear();
    // A segment has at most inert_rows rows before its first paid row, at most paid_rows paid
    // rows, and one closing row (R7).
    if (!r.varint(count) || count == 0 || count > limits.inert_rows + limits.paid_rows) return fail("bad row count");
    for (std::uint64_t i = 0; i < count; i += 4) {
        std::uint8_t byte = 0;
        if (!r.u8(byte)) return fail("truncated row types");
        for (std::uint64_t k = 0; k < 4; ++k) {
            const std::uint8_t code = (byte >> (2 * k)) & 3;
            if (i + k >= count) {
                if (code != 0) return fail("non-zero padding bits in row types");
                continue;
            }
            if (code == 3) return fail("invalid row type");
            out.rows.push_back(static_cast<RowType>(code));
        }
    }
    if (!r.varint(count) || count > limits.paid_rows) return fail("bad generator count");
    for (std::uint64_t i = 0; i < count; ++i) {
        QuadElement g;
        if (!element(r, g)) return fail("bad generator");
        out.generators.push_back(g);
    }
    return true;
}

Bytes encode(const Segment& s) {
    Writer w;
    w.varint(s.version);
    w.hash(s.ring_id);
    w.integer(s.start);
    encodeSegmentBody(w, s);
    return w.data();
}

bool decode(const Bytes& data, Segment& out, const SegmentLimits& limits, std::size_t max_integer_bytes,
            std::string& error) {
    out = {};
    Reader r(data, max_integer_bytes);
    auto fail = [&](const std::string& why) { error = r.error().empty() ? why : why + ": " + r.error(); return false; };
    if (!r.varint(out.version) || out.version != kVersion1) return fail("unsupported segment version");
    if (!r.hash(out.ring_id) || !r.integer(out.start)) return fail("bad segment header");
    if (!decodeSegmentBody(r, out, limits, error)) return false;
    if (!r.done()) return fail("trailing bytes after segment");
    return true;
}

Integer segmentLast(const Segment& s, const std::vector<Integer>& primes) {
    if (s.rows.empty() || primes.size() < s.rows.size()) return Integer(0);
    return primes[s.rows.size() - 1];
}

// ---------------------------------------------------------------- checks

bool verifyCertificate(const QuadCertificate& cert, const Integer& frontier, const std::vector<Integer>& primes,
                       const PrimeOracle& is_prime, const CertificateLimits& limits, std::string& error) {
    if (cert.version != kVersion1) { error = "unsupported certificate version"; return false; }
    if (encode(cert).size() > limits.certificate_bytes) { error = "certificate exceeds the size limit"; return false; }
    auto too_long = [&](const Integer& v) { return magnitudeBytes(v) > limits.integer_bytes; };
    if (too_long(cert.d)) { error = "integer exceeds the size limit"; return false; }
    for (const auto& g : cert.generators)
        if (too_long(g.q) || too_long(g.g.c0) || too_long(g.g.c1)) { error = "integer exceeds the size limit"; return false; }
    if (cert.unit && (too_long(cert.unit->c0) || too_long(cert.unit->c1))) { error = "integer exceeds the size limit"; return false; }
    if (!eligible(cert.d, frontier, limits.eligibility_exponent)) { error = "ring not eligible at this frontier"; return false; }
    if (!isFundamental(cert.d, cert.factors, is_prime, error)) return false;
    // R10: a unit eps != +-1 for D > 0, none for D < 0.
    if (cert.d > 0) {
        if (!cert.unit) { error = "real quadratic certificate needs a unit"; return false; }
        const Integer n = quadNorm(cert.d, *cert.unit);
        if ((n != 1 && n != -1) || cert.unit->c1 == 0) { error = "unit must have norm +-1 and differ from +-1"; return false; }
    } else if (cert.unit) {
        error = "imaginary quadratic certificate takes no unit";
        return false;
    }
    std::size_t next = 0;
    bool covered = false;
    for (const auto& q : primes) {
        if (minkowskiBelow(cert.d, q)) { covered = true; break; }  // q > M_K
        if (rowType(cert.d, q) == RowType::Inert) continue;
        if (next >= cert.generators.size() || cert.generators[next].q != q) {
            error = "missing generator for prime " + q.get_str();
            return false;
        }
        if (!checkGenerator(cert.d, q, cert.generators[next++].g, cert.unit, error)) return false;
    }
    if (!covered) { error = "prime list does not reach the Minkowski bound"; return false; }
    if (next != cert.generators.size()) { error = "certificate lists generators that are not required"; return false; }
    return true;
}

bool checkSegment(const Integer& d, const std::optional<QuadElement>& unit, std::uint64_t ring_number,
                  const Segment& s, const std::vector<Integer>& primes, const SegmentLimits& limits,
                  std::vector<RingPrime>& out, std::string& error) {
    out.clear();
    if (s.version != kVersion1) { error = "unsupported segment version"; return false; }
    if (s.ring_id != quadraticRingId(d)) { error = "segment is for another ring"; return false; }
    if (primes.empty() || primes.front() != s.start) { error = "segment does not start at the ring's next prime"; return false; }
    if (limits.paid_rows == 0 || limits.inert_rows == 0) { error = "segment limits must be positive"; return false; }
    // R7: inert rows, then one or more paid rows, then the first inert row after them. It also
    // ends at the paid_rows-th paid row, or, empty, after inert_rows inert rows.
    std::uint64_t paid = 0;
    std::size_t used = 0, end = 0;
    for (std::size_t k = 0;; ++k) {
        if (k >= primes.size()) { error = "segment would run past the frontier"; return false; }
        if (k >= s.rows.size()) { error = "segment ends before its end (R7)"; return false; }
        const Integer& q = primes[k];
        const RowType t = rowType(d, q);
        if (s.rows[k] != t) { error = "wrong row type at " + q.get_str(); return false; }
        if (t != RowType::Inert) {
            if (used >= s.generators.size()) { error = "missing generator at " + q.get_str(); return false; }
            // [65] generator size, a policy tool (R10): each coefficient at most generator_bytes.
            const QuadElement& g = s.generators[used];
            if (magnitudeBytes(g.c0) > limits.generator_bytes || magnitudeBytes(g.c1) > limits.generator_bytes) {
                error = "generator exceeds the size limit (parameter 65) at " + q.get_str();
                return false;
            }
            const auto r = checkGenerator(d, q, s.generators[used++], unit, error);
            if (!r) return false;
            if (t == RowType::Ramified) out.push_back({ring_number, q, 0, 2, *r});
            else {
                out.push_back({ring_number, q, 0, 1, *r});
                out.push_back({ring_number, q, 1, 1, otherRoot(d, q, *r)});
            }
            if (++paid == limits.paid_rows) { end = k; break; }
        } else if (paid > 0 || k + 1 == limits.inert_rows) {
            end = k;
            break;
        }
    }
    if (s.rows.size() != end + 1) { error = "segment continues past its end (R7)"; return false; }
    if (used != s.generators.size()) { error = "segment carries extra generators"; return false; }
    return true;
}

// ---------------------------------------------------------------- B3.5: mining records

namespace {

constexpr std::uint64_t kMaxSegmentsDecoded = 65536;  // the bound of parameter SegmentsPerSignature
const SegmentLimits kDecodeLimits{65536, 1u << 20, kMaxGeneratorBytes};  // hard decoder bounds

void writeWorkFields(Writer& w, const RingWork& x) {
    w.varint(x.version);
    w.text(x.network_id);
    w.hash(x.ring_id);
    w.text(x.miner);
    w.integer(x.start);
    w.varint(x.segments.size());
    for (const auto& s : x.segments) {  // each segment: its start, then rows and generators
        w.integer(s.start);
        encodeSegmentBody(w, s);
    }
}

void writeRingBlock(Writer& w, const RingBlock& b) {
    w.varint(b.version);
    w.varint(b.ring);
    w.hash(b.previous);
    w.integer(b.start);
    w.varint(b.groups.size());
    for (const auto& g : b.groups) {
        w.text(g.miner);
        w.hash(g.work_hash);
        w.varint(g.segments.size());
        for (const auto& s : g.segments) encodeSegmentBody(w, s);
    }
}

}  // namespace

Bytes ringWorkSigningPayload(const RingWork& x) {
    Writer w;
    w.text("primechain-next/ring-work");
    writeWorkFields(w, x);
    return w.data();
}

Bytes encode(const RingWork& x) {
    Writer w;
    writeWorkFields(w, x);
    w.bytes(x.public_key);
    w.bytes(x.signature);
    return w.data();
}

bool decode(const Bytes& data, RingWork& out, std::size_t max_text, std::string& error) {
    out = {};
    Reader r(data, kMaxGeneratorBytes);
    auto fail = [&](const std::string& why) { error = r.error().empty() ? why : why + ": " + r.error(); return false; };
    std::uint64_t count = 0;
    if (!r.varint(out.version) || out.version != kVersion1) return fail("unsupported ring work version");
    if (!r.text(out.network_id, max_text) || !r.hash(out.ring_id) || !r.text(out.miner, max_text) || !r.integer(out.start))
        return fail("bad ring work header");
    if (!r.varint(count) || count == 0 || count > kMaxSegmentsDecoded) return fail("bad segment count");
    for (std::uint64_t i = 0; i < count; ++i) {
        Segment s;
        s.ring_id = out.ring_id;
        if (!r.integer(s.start)) return fail("bad segment start");
        if (!decodeSegmentBody(r, s, kDecodeLimits, error)) return false;
        out.segments.push_back(std::move(s));
    }
    if (!r.bytes(out.public_key, 1u << 16) || !r.bytes(out.signature, 1u << 16)) return fail("bad ring work signature");
    if (!r.done()) return fail("trailing bytes after ring work");
    return true;
}

Hash256 ringWorkHash(const RingWork& x) {
    Writer w;
    w.text("primechain-next/ring-work-hash");
    w.bytes(encode(x));
    return sha3(w.data());
}

Bytes encode(const RingBlock& b) {
    Writer w;
    writeRingBlock(w, b);
    return w.data();
}

Hash256 ringBlockHash(const RingBlock& b) {
    Writer w;
    w.text("primechain-next/ring-block");
    w.bytes(encode(b));
    return sha3(w.data());
}

Bytes encode(const RingBatch& x) {
    Writer w;
    w.varint(x.version);
    w.text(x.network_id);
    w.varint(x.open_height);
    w.hash(x.previous_main);
    w.varint(x.blocks.size());
    for (const auto& b : x.blocks) writeRingBlock(w, b);
    return w.data();
}

bool decode(const Bytes& data, RingBatch& out, std::size_t max_text, std::string& error) {
    out = {};
    Reader r(data, kMaxGeneratorBytes);
    auto fail = [&](const std::string& why) { error = r.error().empty() ? why : why + ": " + r.error(); return false; };
    std::uint64_t count = 0;
    if (!r.varint(out.version) || out.version != kVersion1) return fail("unsupported ring batch version");
    if (!r.text(out.network_id, max_text) || !r.varint(out.open_height) || !r.hash(out.previous_main))
        return fail("bad ring batch header");
    if (!r.varint(count) || count > data.size()) return fail("bad ring block count");
    for (std::uint64_t i = 0; i < count; ++i) {
        RingBlock b;
        std::uint64_t groups = 0;
        if (!r.varint(b.version) || b.version != kVersion1 || !r.varint(b.ring) || !r.hash(b.previous) ||
            !r.integer(b.start) || !r.varint(groups) || groups > data.size())
            return fail("bad ring block");
        for (std::uint64_t j = 0; j < groups; ++j) {
            RingGroup g;
            std::uint64_t segments = 0;
            if (!r.text(g.miner, max_text) || !r.hash(g.work_hash) || !r.varint(segments) ||
                segments > kMaxSegmentsDecoded)
                return fail("bad ring group");
            for (std::uint64_t k = 0; k < segments; ++k) {
                Segment s;
                if (!decodeSegmentBody(r, s, kDecodeLimits, error)) return false;
                g.segments.push_back(std::move(s));
            }
            b.groups.push_back(std::move(g));
        }
        out.blocks.push_back(std::move(b));
    }
    if (!r.done()) return fail("trailing bytes after ring batch");
    return true;
}

Hash256 ringBatchHash(const RingBatch& x) {
    Writer w;
    w.text("primechain-next/ring-batch");
    w.bytes(encode(x));
    return sha3(w.data());
}

}  // namespace primechain::next
