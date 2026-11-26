// Tests for B3.2 ring mathematics (quadratic rings): certificates, segments, encodings.
// Usage: primechain-next-rings-tests <quadratic_vectors_v2.txt>
#include "primechain_next/rings.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace primechain::next;

namespace {

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// ---- test-only helpers, independent of the module
std::vector<Integer> primesBelow(unsigned long n) {
    std::vector<bool> s(n + 1, true);
    std::vector<Integer> out;
    for (unsigned long i = 2; i <= n; ++i) {
        if (!s[i]) continue;
        out.emplace_back(i);
        for (unsigned long j = i * i; j <= n; j += i) s[j] = false;
    }
    return out;
}
bool testPrime(const Integer& n) { return n > 1 && mpz_probab_prime_p(n.get_mpz_t(), 40) > 0; }
Integer firstEligiblePrime(const Integer& d, const std::vector<Integer>& primes) {
    const Integer a = d < 0 ? Integer(-d) : d;
    for (const auto& p : primes) if (p * p >= a) return p;
    throw std::runtime_error("prime list too short");
}
std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out; std::string item; std::istringstream in(text);
    while (std::getline(in, item, sep)) out.push_back(item);
    return out;
}
std::string field(const std::string& line, const std::string& key) {
    std::istringstream in(line); std::string t;
    while (in >> t) if (t.rfind(key + "=", 0) == 0) return t.substr(key.size() + 1);
    return "";
}

struct Vectors {
    std::vector<std::pair<QuadCertificate, std::vector<Integer>>> fields;  // certificate, listed roots
    std::vector<Integer> nocert;
    struct Row { Integer p; RowType type; Integer r; QuadElement g; };
    std::map<Integer, std::vector<Row>> entries;  // D -> rows in order
};

Vectors load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    Vectors v; std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("FIELD ", 0) == 0) {
            QuadCertificate c; std::vector<Integer> roots;
            c.d = Integer(field(line, "D"));
            for (const auto& f : split(field(line, "FACTORS"), '*')) {
                const auto pe = split(f, '^');
                c.factors.emplace_back(Integer(pe[0]), std::stoull(pe[1]));
            }
            const std::string gens = field(line, "GENS");
            if (!gens.empty())
                for (const auto& g : split(gens, ',')) {
                    const auto x = split(g, ':');
                    c.generators.push_back({Integer(x[0]), {Integer(x[2]), Integer(x[3])}});
                    roots.emplace_back(x[1]);
                }
            const std::string u = field(line, "UNIT");
            if (!u.empty()) { const auto x = split(u, ':'); c.unit = QuadElement{Integer(x[0]), Integer(x[1])}; }
            v.fields.emplace_back(c, roots);
        } else if (line.rfind("NOCERT ", 0) == 0) {
            v.nocert.emplace_back(field(line, "D"));
        } else if (line.rfind("ENTRY ", 0) == 0) {
            Vectors::Row row; const Integer d(field(line, "D")); row.p = Integer(field(line, "P"));
            const std::string t = field(line, "TYPE");
            row.type = t == "inert" ? RowType::Inert : (t == "split" ? RowType::Split : RowType::Ramified);
            if (row.type != RowType::Inert) {
                row.r = Integer(field(line, "R"));
                const auto x = split(field(line, "GEN"), ':');
                row.g = {Integer(x[0]), Integer(x[1])};
            }
            v.entries[d].push_back(row);
        }
    }
    return v;
}

const QuadCertificate* certificateOf(const Vectors& v, const Integer& d) {
    for (const auto& [c, r] : v.fields) if (c.d == d) return &c;
    return nullptr;
}

std::size_t rejected = 0, attempts = 0;
void mustReject(const QuadCertificate& c, const Integer& f, const std::vector<Integer>& primes,
                const CertificateLimits& lim, const std::string& what) {
    std::string error;
    ++attempts;
    require(!verifyCertificate(c, f, primes, testPrime, lim, error), "accepted a corrupted certificate: " + what);
    ++rejected;
}

// ------------------------------------------------------------------ certificates
void certificateTests(const Vectors& v, const std::vector<Integer>& primes) {
    const CertificateLimits lim;
    std::size_t ok = 0, real = 0;
    for (const auto& [c, roots] : v.fields) {
        const Integer f = firstEligiblePrime(c.d, primes);
        std::string error;
        require(verifyCertificate(c, f, primes, testPrime, lim, error), "certificate D=" + c.d.get_str() + ": " + error);
        ++ok; real += c.d > 0;
        for (std::size_t i = 0; i < c.generators.size(); ++i)
            require(derivedRoot(c.generators[i].q, c.generators[i].g) == roots[i], "derived root matches the listed label");
        QuadCertificate back;
        const Bytes bytes = encode(c);
        require(decode(bytes, back, lim, error) && back == c && encode(back) == bytes, "certificate round trip");
        // eligibility: not at the previous prime, if its square is below |D|
        const auto it = std::find(primes.begin(), primes.end(), f);
        if (it != primes.begin() && *(it - 1) * *(it - 1) < (c.d < 0 ? Integer(-c.d) : c.d))
            mustReject(c, *(it - 1), primes, lim, "not yet eligible");
        // corruption classes (extension-certificates.md section 8, plus units and padding)
        auto bad = c;
        if (!c.generators.empty()) {
            bad = c; bad.generators[0].g.c0 += 1; mustReject(bad, f, primes, lim, "altered generator");
            bad = c; bad.generators[0].g = quadConjugate(c.d, c.generators[0].g);
            if (rowType(c.d, c.generators[0].q) == RowType::Split) mustReject(bad, f, primes, lim, "larger-root generator");
            bad = c; bad.generators.erase(bad.generators.begin()); mustReject(bad, f, primes, lim, "missing generator");
            bad = c; bad.generators.push_back(c.generators.back()); mustReject(bad, f, primes, lim, "extra generator");
            if (c.generators.size() > 1) {
                bad = c; std::swap(bad.generators[0], bad.generators[1]); mustReject(bad, f, primes, lim, "reordered");
            }
            if (c.unit)
                for (int k = 1; k <= 3; ++k) {
                    bad = c;
                    for (int j = 0; j < k; ++j) bad.generators[0].g = quadMul(c.d, bad.generators[0].g, *c.unit);
                    // a tie in height (k = 1 only) is a second valid choice, not padding
                    if (quadHeight(bad.generators[0].g) > quadHeight(c.generators[0].g)) mustReject(bad, f, primes, lim, "padded generator");
                }
        }
        bad = c; bad.d = c.d * 4; mustReject(bad, f * 2, primes, lim, "non-fundamental D");
        bad = c; bad.factors.back().second += 1; mustReject(bad, f, primes, lim, "wrong factorization");
        bad = c; bad.factors.push_back({Integer(3), 1}); mustReject(bad, f, primes, lim, "extra factor");
        if (c.d > 0) {
            bad = c; bad.unit.reset(); mustReject(bad, f, primes, lim, "missing unit");
            bad = c; bad.unit = QuadElement{1, 0}; mustReject(bad, f, primes, lim, "unit 1");
            bad = c; bad.unit->c0 += 1; if (quadNorm(c.d, *bad.unit) != 1 && quadNorm(c.d, *bad.unit) != -1) mustReject(bad, f, primes, lim, "non-unit");
        } else {
            bad = c; bad.unit = QuadElement{-1, 0}; mustReject(bad, f, primes, lim, "unit on an imaginary ring");
        }
        CertificateLimits small = lim;
        small.certificate_bytes = encode(c).size() - 1;
        mustReject(c, f, primes, small, "certificate over the size limit");
        QuadCertificate tmp;
        require(!decode(encode(c), tmp, small, error), "decoder refuses a certificate over the size limit");
        if (!c.generators.empty() || c.unit) {
            Integer biggest = 0;
            auto grow = [&](const Integer& x) { const Integer a = x < 0 ? Integer(-x) : x; if (a > biggest) biggest = a; };
            for (const auto& g : c.generators) { grow(g.g.c0); grow(g.g.c1); }
            if (c.unit) { grow(c.unit->c0); grow(c.unit->c1); }
            if (biggest > 255) {
                small = lim; small.integer_bytes = integerMagnitude(biggest).size() - 1;
                mustReject(c, f, primes, small, "integer over the size limit");
            }
        }
    }
    require(ok == 351, "all 351 certificates verified");
    // Non-PIDs: no certificate without generators passes when one is required.
    std::size_t nocert = 0;
    for (const auto& d : v.nocert) {
        QuadCertificate c; c.d = d;
        Integer a = d < 0 ? Integer(-d) : d;
        for (const auto& p : primes) {
            if (p * p > a) break;
            unsigned e = 0;
            while (a % p == 0) { a /= p; ++e; }
            if (e) c.factors.push_back({p, e});
        }
        if (a > 1) c.factors.push_back({a, 1});
        if (d > 0) c.unit = QuadElement{1, 1};  // any value; the check fails earlier or later
        std::string error;
        ++attempts;
        require(!verifyCertificate(c, firstEligiblePrime(d, primes), primes, testPrime, lim, error), "non-PID with an empty certificate");
        ++rejected; ++nocert;
    }
    std::cout << "rings certificates: " << ok << " verified (" << real << " real, with a unit), " << nocert
              << " non-PIDs refused, " << rejected << " of " << attempts << " corrupted certificates refused\n";
}

// ------------------------------------------------------------------ segments
struct Cut { std::size_t from, to; };  // row indices [from, to)
std::vector<Cut> cuts(const std::vector<Vectors::Row>& rows, const SegmentLimits& lim) {
    std::vector<Cut> out;
    std::size_t i = 0;
    while (i < rows.size()) {
        std::uint64_t paid = 0;
        std::size_t k = i;
        bool closed = false;
        for (; k < rows.size(); ++k) {
            if (rows[k].type != RowType::Inert) { if (++paid == lim.paid_rows) { closed = true; break; } }
            else if (paid > 0 || k - i + 1 == lim.inert_rows) { closed = true; break; }
        }
        if (!closed) break;  // the last, incomplete segment
        out.push_back({i, k + 1});
        i = k + 1;
    }
    return out;
}
Segment build(const Integer& d, const std::vector<Vectors::Row>& rows, Cut c) {
    Segment s;
    s.ring_id = quadraticRingId(d);
    s.start = rows[c.from].p;
    for (std::size_t k = c.from; k < c.to; ++k) {
        s.rows.push_back(rows[k].type);
        if (rows[k].type != RowType::Inert) s.generators.push_back(rows[k].g);
    }
    return s;
}
std::vector<Integer> primesFrom(const std::vector<Vectors::Row>& rows, std::size_t from) {
    std::vector<Integer> out;
    for (std::size_t k = from; k < rows.size(); ++k) out.push_back(rows[k].p);
    return out;
}
bool segmentOk(const Integer& d, const std::optional<QuadElement>& unit, const Segment& s, const std::vector<Integer>& primes,
               const SegmentLimits& lim, std::vector<RingPrime>& out, std::string& error) {
    return checkSegment(d, unit, 5, s, primes, lim, out, error);
}

void segmentTests(const Vectors& v) {
    const SegmentLimits lim;
    std::size_t segments = 0, rows_checked = 0, new_primes = 0, refused = 0, tried = 0, codec = 0;
    std::size_t entries = 0;
    for (const auto& [d, rows] : v.entries) {
        entries += rows.size();
        const QuadCertificate* cert = certificateOf(v, d);
        require(cert != nullptr, "ring has a certificate");
        const auto& unit = cert->unit;
        for (const auto& c : cuts(rows, lim)) {
            const Segment s = build(d, rows, c);
            const auto primes = primesFrom(rows, c.from);
            std::vector<RingPrime> out;
            std::string error;
            require(segmentOk(d, unit, s, primes, lim, out, error), "segment D=" + d.get_str() + " at " + s.start.get_str() + ": " + error);
            ++segments; rows_checked += s.rows.size(); new_primes += out.size();
            // the assets: split rows give i = 0 (listed root) and i = 1; ramified rows one prime with e = 2
            std::size_t j = 0;
            for (std::size_t k = c.from; k < c.to; ++k) {
                if (rows[k].type == RowType::Inert) continue;
                require(out[j].q == rows[k].p && out[j].i == 0 && out[j].root == rows[k].r && out[j].ring == 5, "asset i = 0");
                if (rows[k].type == RowType::Split) {
                    require(out[j + 1].i == 1 && out[j + 1].e == 1 && out[j + 1].root == otherRoot(d, rows[k].p, rows[k].r), "asset i = 1");
                    j += 2;
                } else { require(out[j].e == 2, "ramified e = 2"); j += 1; }
            }
            require(j == out.size(), "no extra assets");
            // encoding
            Segment back;
            const Bytes bytes = encode(s);
            require(decode(bytes, back, lim, 4096, error) && back == s && encode(back) == bytes, "segment round trip");
            for (std::size_t n = 0; n < bytes.size(); ++n) {
                Bytes cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
                require(!decode(cut, back, lim, 4096, error), "truncated segment refused");
                ++codec;
            }
            Bytes extra = bytes; extra.push_back(0);
            require(!decode(extra, back, lim, 4096, error), "trailing byte refused"); ++codec;
            // corruptions
            auto reject = [&](const Segment& bad, const std::vector<Integer>& p, const std::string& what) {
                std::vector<RingPrime> o; std::string e;
                ++tried;
                require(!segmentOk(d, unit, bad, p, lim, o, e), "accepted a corrupted segment: " + what + " D=" + d.get_str() + " at " + s.start.get_str());
                ++refused;
            };
            Segment bad = s;
            bad.rows[0] = bad.rows[0] == RowType::Inert ? RowType::Split : RowType::Inert; reject(bad, primes, "wrong row type");
            bad = s; bad.rows.pop_back(); reject(bad, primes, "ends early");
            bad = s; bad.rows.push_back(RowType::Inert); reject(bad, primes, "continues past its end");
            bad = s; bad.generators.push_back(s.generators.front()); reject(bad, primes, "extra generator");
            bad = s; bad.generators.pop_back(); reject(bad, primes, "missing generator");
            bad = s; bad.generators[0].c0 += 1; reject(bad, primes, "altered generator");
            bad = s; bad.ring_id[0] ^= 1; reject(bad, primes, "another ring");
            if (primes.size() > 1) { reject(s, std::vector<Integer>(primes.begin() + 1, primes.end()), "wrong start"); }
            reject(s, std::vector<Integer>(primes.begin(), primes.begin() + static_cast<std::ptrdiff_t>(s.rows.size() - 1)), "past the frontier");
            for (std::size_t k = 0, g = 0; k < s.rows.size(); ++k) {
                if (s.rows[k] == RowType::Inert) continue;
                if (s.rows[k] == RowType::Split) { bad = s; bad.generators[g] = quadConjugate(d, s.generators[g]); reject(bad, primes, "larger-root generator"); }
                if (unit) {
                    bad = s; bad.generators[g] = quadMul(d, quadMul(d, s.generators[g], *unit), *unit);
                    reject(bad, primes, "padded generator");
                }
                break;
            }
        }
    }
    require(entries == 2424, "all 2,424 entries loaded");
    // Q(sqrt 17): [2 | 3], [5, 7, 11 | 13, 17, 19 | 23]
    {
        const Integer d = 17;
        const auto c = cuts(v.entries.at(d), lim);
        const auto& rows = v.entries.at(d);
        require(c.size() > 2 && rows[c[0].from].p == 2 && rows[c[0].to - 1].p == 3, "Q(sqrt 17) first segment [2 | 3]");
        require(rows[c[1].from].p == 5 && rows[c[1].to - 1].p == 23 && c[1].to - c[1].from == 7, "Q(sqrt 17) second segment ends at 23");
    }
    // Limits: the 2nd paid row ends a segment; an empty segment after 2 inert rows (D = -163: 2, 3 inert)
    {
        SegmentLimits two{2, 2};
        const Integer d = -163;
        const auto& rows = v.entries.at(d);
        std::vector<RingPrime> out; std::string error;
        Segment s = build(d, rows, {0, 2});
        require(rows[0].type == RowType::Inert && rows[1].type == RowType::Inert, "D=-163: 2 and 3 are inert");
        require(segmentOk(d, std::nullopt, s, primesFrom(rows, 0), two, out, error) && out.empty(), "empty segment after inert_rows rows");
        const auto c = cuts(rows, two);
        for (const auto& cut : c) {
            const Segment t = build(d, rows, cut);
            require(segmentOk(d, std::nullopt, t, primesFrom(rows, cut.from), two, out, error), "segments under small limits: " + error);
            require(t.generators.size() <= 2, "at most 2 paid rows");
        }
        Segment longer = build(d, rows, {0, 3});
        require(!segmentOk(d, std::nullopt, longer, primesFrom(rows, 0), two, out, error), "an empty segment ends at inert_rows");
    }
    // Malformed bytes
    {
        const auto& rows = v.entries.at(Integer(17));
        Segment s = build(17, rows, cuts(rows, lim)[1]);  // 7 rows: one byte with padding
        Bytes b = encode(s);
        Segment out; std::string error;
        const std::size_t types_at = 1 + 32 + 2 + 1;  // version, ring id, start (len 1 + 1 byte), row count
        Bytes pad = b; pad[types_at + 1] |= 0xC0; require(!decode(pad, out, lim, 4096, error), "non-zero padding bits refused");
        Bytes three = b; three[types_at] |= 3; require(!decode(three, out, lim, 4096, error), "row type 3 refused");
        Bytes version = b; version[0] = 2; require(!decode(version, out, lim, 4096, error), "unknown version refused");
        QuadCertificate c; c.d = 5; c.factors = {{Integer(5), 1}}; c.unit = QuadElement{0, -1};
        Bytes cb = encode(c); cb[1] = 2;
        require(!decode(cb, c, CertificateLimits{}, error), "wrong certificate type refused");
        // negative zero: hand-made certificate with D written as sign 1, magnitude empty
        Writer h; h.varint(1); h.varint(1); h.u8(1); h.integer(0); h.varint(0); h.varint(0); h.u8(0);
        require(!decode(h.data(), c, CertificateLimits{}, error), "negative zero refused");
        Writer f; f.varint(1); f.varint(1); f.u8(0); f.integer(5); f.varint(0); f.varint(0); f.u8(2);
        require(!decode(f.data(), c, CertificateLimits{}, error), "unit flag 2 refused");
        codec += 6;
    }
    std::cout << "rings segments: " << segments << " segments, " << rows_checked << " rows, " << new_primes
              << " new ring primes; " << refused << " of " << tried << " corrupted segments refused; " << codec
              << " malformed encodings refused\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("usage: primechain-next-rings-tests <quadratic_vectors_v2.txt>");
        const Vectors v = load(argv[1]);
        const auto primes = primesBelow(5000);
        certificateTests(v, primes);
        segmentTests(v);
        std::cout << "rings: PASS (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "rings test failed: " << e.what() << '\n';
        return 1;
    }
}
