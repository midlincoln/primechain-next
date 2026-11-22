#include "quadratic_certs.hpp"

#include <sstream>
#include <stdexcept>

namespace primechain::extensions {
namespace {

// pi^2 enclosed in [kPi2Low, kPi2High] / 10^30.
// pi^2 = 9.869604401089358618834490999876151135313699407240790626413349...
const mpz_class kScale("1000000000000000000000000000000");
const mpz_class kPi2Low("9869604401089358618834490999876");
const mpz_class kPi2High("9869604401089358618834490999877");

mpz_class absValue(const mpz_class& x) { return x < 0 ? mpz_class(-x) : x; }

bool parseInteger(const std::string& text, mpz_class& out) {
    if (text.empty()) return false;
    std::size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (start == text.size()) return false;
    for (std::size_t i = start; i < text.size(); ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    return out.set_str(text[0] == '+' ? text.substr(1) : text, 10) == 0;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(text);
    while (std::getline(in, item, separator)) out.push_back(item);
    if (!text.empty() && text.back() == separator) out.emplace_back();
    return out;
}

bool field(const std::string& line, const std::string& key, std::string& value) {
    std::istringstream in(line);
    std::string token;
    in >> token;  // record tag
    while (in >> token) {
        if (token.rfind(key + "=", 0) == 0) { value = token.substr(key.size() + 1); return true; }
    }
    return false;
}

}  // namespace

bool generatesPrimeAbove(const mpz_class& d, const mpz_class& q, const mpz_class& a, const mpz_class& b) {
    // In O_K iff a = b*d (mod 2); norm (a^2 - d b^2)/4.
    mpz_class parity = a - b * d;
    if (mpz_even_p(parity.get_mpz_t()) == 0) return false;
    const mpz_class norm4 = a * a - d * b * b;
    if (d < 0) return norm4 == 4 * q;
    return norm4 == 4 * q || norm4 == -4 * q;
}

bool isFundamental(const mpz_class& d, const std::vector<std::pair<mpz_class, unsigned>>& factors,
                   const PrimeOracle& is_prime, std::string& error) {
    if (d == 0 || d == 1) { error = "D must not be 0 or 1"; return false; }
    mpz_class product = 1;
    mpz_class previous = 0;
    unsigned two_exponent = 0;
    bool odd_squarefree = true;
    for (const auto& [p, e] : factors) {
        if (p <= previous) { error = "factorization not in increasing order"; return false; }
        if (e == 0 || !is_prime(p)) { error = "factorization contains a non-prime or zero exponent"; return false; }
        previous = p;
        mpz_class power;
        mpz_pow_ui(power.get_mpz_t(), p.get_mpz_t(), e);
        product *= power;
        if (p == 2) two_exponent = e;
        else if (e > 1) odd_squarefree = false;
    }
    if (product != absValue(d)) { error = "factorization does not multiply to |D|"; return false; }
    if (!odd_squarefree) { error = "D has an odd square factor"; return false; }
    const mpz_class r4 = ((d % 4) + 4) % 4;
    if (two_exponent == 0) {
        if (r4 != 1) { error = "odd D must be 1 mod 4"; return false; }
        return true;
    }
    if (two_exponent == 1 || two_exponent > 3) { error = "invalid power of 2 in D"; return false; }
    const mpz_class m = d / 4;
    const mpz_class m4 = ((m % 4) + 4) % 4;
    if (two_exponent == 2 && m4 != 3) { error = "D = 4m with odd m requires m = 3 mod 4"; return false; }
    if (two_exponent == 3 && m4 != 2) { error = "D = 4m with even m requires m = 2 mod 4"; return false; }
    return true;
}

Comparison compareMinkowski(const mpz_class& d, const mpz_class& x) {
    if (d > 0) {
        // M = sqrt(d)/2 ; M <= x  <=>  d <= 4 x^2. Equality would need d = 4x^2, a square times 4,
        // which is never a fundamental discriminant, so Below means M < x for valid d.
        return d <= 4 * x * x ? Comparison::Below : Comparison::Above;
    }
    // M = (2/pi) sqrt|d| ; M <= x  <=>  4|d| <= pi^2 x^2 (never equal: pi^2 is irrational).
    const mpz_class lhs = 4 * absValue(d) * kScale;
    if (lhs < kPi2Low * x * x) return Comparison::Below;
    if (lhs > kPi2High * x * x) return Comparison::Above;
    throw std::runtime_error("pi^2 enclosure too coarse for this comparison");
}

bool eligibleAt(const mpz_class& d, const mpz_class& f) { return compareMinkowski(d, f) == Comparison::Below; }

Decomposition decomposition(const mpz_class& d, const mpz_class& p) {
    const int k = mpz_kronecker(d.get_mpz_t(), p.get_mpz_t());
    return k == 0 ? Decomposition::Ramified : (k > 0 ? Decomposition::Split : Decomposition::Inert);
}

bool verifyField(const FieldCertificate& cert, const std::vector<mpz_class>& primes,
                 const PrimeOracle& is_prime, std::string& error) {
    if (!isFundamental(cert.d, cert.factors, is_prime, error)) return false;
    if (!is_prime(cert.f)) { error = "eligibility frontier F is not prime"; return false; }
    if (!eligibleAt(cert.d, cert.f)) { error = "field is not eligible at the claimed F (M_K > F)"; return false; }
    // Required: every prime q <= M_K with (D/q) != -1, in increasing order, nothing else.
    std::size_t next = 0;
    bool covered_bound = false;
    for (const auto& q : primes) {
        if (compareMinkowski(cert.d, q) == Comparison::Below) {  // q > M_K: done
            covered_bound = true;
            break;
        }
        if (decomposition(cert.d, q) == Decomposition::Inert) continue;
        if (next >= cert.generators.size() || cert.generators[next].q != q) {
            error = "missing generator for prime " + q.get_str();
            return false;
        }
        const auto& g = cert.generators[next++];
        if (!generatesPrimeAbove(cert.d, q, g.a, g.b)) {
            error = "element does not have norm +-" + q.get_str();
            return false;
        }
    }
    if (!covered_bound) { error = "prime list does not reach the Minkowski bound"; return false; }
    if (next != cert.generators.size()) { error = "certificate lists generators that are not required"; return false; }
    return true;
}

bool verifySplit(const SplitEntry& entry, const PrimeOracle& is_prime, std::string& error) {
    if (!is_prime(entry.p)) { error = "P is not prime"; return false; }
    const auto expected = decomposition(entry.d, entry.p);
    if (expected != entry.type) { error = "wrong decomposition type"; return false; }
    if (entry.type == Decomposition::Inert) {
        if (entry.has_generator) { error = "inert prime must not carry a generator"; return false; }
        return true;
    }
    if (!entry.has_generator) { error = "split/ramified prime needs a generator"; return false; }
    if (!generatesPrimeAbove(entry.d, entry.p, entry.a, entry.b)) { error = "generator norm is not +-P"; return false; }
    return true;
}

bool provesNoGeneratorImaginary(const mpz_class& d, const mpz_class& q) {
    if (d >= 0) return false;
    // a^2 + |d| b^2 = 4q  =>  |b| <= sqrt(4q/|d|), |a| <= sqrt(4q). Small: exhaustive search.
    const mpz_class n = 4 * q, ad = absValue(d);
    for (mpz_class b = 0; ad * b * b <= n; ++b) {
        const mpz_class rest = n - ad * b * b;
        if (mpz_perfect_square_p(rest.get_mpz_t()) == 0) continue;
        mpz_class a;
        mpz_sqrt(a.get_mpz_t(), rest.get_mpz_t());
        if (generatesPrimeAbove(d, q, a, b)) return false;
    }
    return true;
}

bool parseField(const std::string& line, FieldCertificate& cert, std::string& error) {
    cert = {};
    std::string d, f, factors, gens;
    if (line.rfind("FIELD ", 0) != 0 || !field(line, "D", d) || !field(line, "F", f) ||
        !field(line, "FACTORS", factors) || !field(line, "GENS", gens) ||
        !parseInteger(d, cert.d) || !parseInteger(f, cert.f)) {
        error = "malformed FIELD record";
        return false;
    }
    for (const auto& item : split(factors, '*')) {
        const auto parts = split(item, '^');
        mpz_class p, e;
        if (parts.size() != 2 || !parseInteger(parts[0], p) || !parseInteger(parts[1], e) || e < 1 || e > 64) {
            error = "malformed FACTORS";
            return false;
        }
        cert.factors.emplace_back(p, static_cast<unsigned>(e.get_ui()));
    }
    if (!gens.empty()) {
        for (const auto& item : split(gens, ',')) {
            const auto parts = split(item, ':');
            Generator g;
            if (parts.size() != 3 || !parseInteger(parts[0], g.q) || !parseInteger(parts[1], g.a) ||
                !parseInteger(parts[2], g.b)) {
                error = "malformed GENS";
                return false;
            }
            cert.generators.push_back(g);
        }
    }
    return true;
}

bool parseSplit(const std::string& line, SplitEntry& entry, std::string& error) {
    entry = {};
    std::string d, p, type, gen;
    if (line.rfind("SPLIT ", 0) != 0 || !field(line, "D", d) || !field(line, "P", p) || !field(line, "TYPE", type) ||
        !parseInteger(d, entry.d) || !parseInteger(p, entry.p)) {
        error = "malformed SPLIT record";
        return false;
    }
    if (type == "split") entry.type = Decomposition::Split;
    else if (type == "inert") entry.type = Decomposition::Inert;
    else if (type == "ramified") entry.type = Decomposition::Ramified;
    else { error = "unknown TYPE"; return false; }
    if (field(line, "GEN", gen)) {
        const auto parts = split(gen, ':');
        if (parts.size() != 2 || !parseInteger(parts[0], entry.a) || !parseInteger(parts[1], entry.b)) {
            error = "malformed GEN";
            return false;
        }
        entry.has_generator = true;
    }
    return true;
}

std::string formatField(const FieldCertificate& cert) {
    std::ostringstream out;
    out << "FIELD D=" << cert.d.get_str() << " F=" << cert.f.get_str() << " FACTORS=";
    for (std::size_t i = 0; i < cert.factors.size(); ++i)
        out << (i ? "*" : "") << cert.factors[i].first.get_str() << "^" << cert.factors[i].second;
    out << " GENS=";
    for (std::size_t i = 0; i < cert.generators.size(); ++i) {
        const auto& g = cert.generators[i];
        out << (i ? "," : "") << g.q.get_str() << ":" << g.a.get_str() << ":" << g.b.get_str();
    }
    return out.str();
}

}  // namespace primechain::extensions
