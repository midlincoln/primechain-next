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
mpz_class mod(const mpz_class& a, const mpz_class& m) { mpz_class r = a % m; if (r < 0) r += m; return r; }
bool oneMod4(const mpz_class& d) { return mod(d, 4) == 1; }

bool parseInteger(const std::string& text, mpz_class& out) {
    if (text.empty()) return false;
    const std::size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
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
    while (in >> token)
        if (token.rfind(key + "=", 0) == 0) { value = token.substr(key.size() + 1); return true; }
    return false;
}

}  // namespace

mpz_class norm(const mpz_class& d, const mpz_class& c0, const mpz_class& c1) {
    if (oneMod4(d)) return c0 * c0 + c0 * c1 + c1 * c1 * ((1 - d) / 4);  // omega = (1+sqrt D)/2
    return c0 * c0 - (d / 4) * c1 * c1;                                   // omega = sqrt(D/4)
}

bool isRoot(const mpz_class& d, const mpz_class& q, const mpz_class& r) {
    if (r < 0 || r >= q) return false;
    const mpz_class f = oneMod4(d) ? mpz_class(r * r - r + (1 - d) / 4) : mpz_class(r * r - d / 4);
    return mod(f, q) == 0;
}

mpz_class otherRoot(const mpz_class& d, const mpz_class& q, const mpz_class& r) {
    // The two roots of f mod q sum to the trace of omega: 1 if D = 1 (mod 4), else 0.
    return mod((oneMod4(d) ? mpz_class(1) : mpz_class(0)) - r, q);
}

Decomposition decomposition(const mpz_class& d, const mpz_class& q) {
    const int k = mpz_kronecker(d.get_mpz_t(), q.get_mpz_t());
    return k == 0 ? Decomposition::Ramified : (k > 0 ? Decomposition::Split : Decomposition::Inert);
}

bool generatesLabeledPrime(const mpz_class& d, const mpz_class& q, const mpz_class& r,
                           const mpz_class& c0, const mpz_class& c1) {
    if (!isRoot(d, q, r)) return false;
    const mpz_class n = norm(d, c0, c1);
    if (d < 0 ? n != q : (n != q && n != -q)) return false;  // generates a prime ideal above q
    return mod(c0 + c1 * r, q) == 0;                          // ... namely (q, omega - r)
}

bool isFundamental(const mpz_class& d, const std::vector<std::pair<mpz_class, unsigned>>& factors,
                   const PrimeOracle& is_prime, std::string& error) {
    if (d == 0 || d == 1) { error = "D must not be 0 or 1"; return false; }
    mpz_class product = 1, previous = 0;
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
    if (two_exponent == 0) {
        if (mod(d, 4) != 1) { error = "odd D must be 1 mod 4"; return false; }
        return true;
    }
    if (two_exponent == 1 || two_exponent > 3) { error = "invalid power of 2 in D"; return false; }
    const mpz_class m4 = mod(d / 4, 4);
    if (two_exponent == 2 && m4 != 3) { error = "D = 4m with odd m requires m = 3 mod 4"; return false; }
    if (two_exponent == 3 && m4 != 2) { error = "D = 4m with even m requires m = 2 mod 4"; return false; }
    return true;
}

bool eligibleAt(const mpz_class& d, const mpz_class& frontier) { return absValue(d) <= frontier * frontier; }

Comparison compareMinkowski(const mpz_class& d, const mpz_class& x) {
    // Real: M = sqrt(D)/2, so M <= x <=> D <= 4x^2 (equality would need D = 4x^2, never fundamental).
    if (d > 0) return d <= 4 * x * x ? Comparison::Below : Comparison::Above;
    // Imaginary: M = (2/pi) sqrt|D|, so M <= x <=> 4|D| <= pi^2 x^2 (never equal: pi^2 irrational).
    const mpz_class lhs = 4 * absValue(d) * kScale;
    if (lhs < kPi2Low * x * x) return Comparison::Below;
    if (lhs > kPi2High * x * x) return Comparison::Above;
    throw std::runtime_error("pi^2 enclosure too coarse for this comparison");
}

bool verifyField(const FieldCertificate& cert, const std::vector<mpz_class>& primes,
                 const PrimeOracle& is_prime, std::string& error) {
    if (!isFundamental(cert.d, cert.factors, is_prime, error)) return false;
    // Required: for every prime q <= M_K that is not inert, a generator of the prime labeled by
    // the smaller root of f mod q (its conjugate covers the other prime above q).
    std::size_t next = 0;
    bool covered = false;
    for (const auto& q : primes) {
        if (compareMinkowski(cert.d, q) == Comparison::Below) { covered = true; break; }  // q > M_K
        if (decomposition(cert.d, q) == Decomposition::Inert) continue;
        if (next >= cert.generators.size() || cert.generators[next].q != q) {
            error = "missing generator for prime " + q.get_str();
            return false;
        }
        const auto& g = cert.generators[next++];
        const mpz_class other = otherRoot(cert.d, q, g.r);
        if (!isRoot(cert.d, q, g.r) || other < g.r) {
            error = "label for " + q.get_str() + " is not the smaller root of f mod q";
            return false;
        }
        if (!generatesLabeledPrime(cert.d, q, g.r, g.c0, g.c1)) {
            error = "element does not generate the prime labeled (" + q.get_str() + ", " + g.r.get_str() + ")";
            return false;
        }
    }
    if (!covered) { error = "prime list does not reach the Minkowski bound"; return false; }
    if (next != cert.generators.size()) { error = "certificate lists generators that are not required"; return false; }
    return true;
}

bool verifyEntry(const FieldEntry& entry, const PrimeOracle& is_prime, std::string& error) {
    if (!is_prime(entry.p)) { error = "P is not prime"; return false; }
    if (decomposition(entry.d, entry.p) != entry.type) { error = "wrong decomposition type"; return false; }
    if (entry.type == Decomposition::Inert) {
        if (entry.has_generator) { error = "inert prime must not carry a generator"; return false; }
        return true;
    }
    if (!entry.has_generator) { error = "split/ramified prime needs a label and a generator"; return false; }
    if (!isRoot(entry.d, entry.p, entry.r)) { error = "label is not a root of f mod p"; return false; }
    const mpz_class other = otherRoot(entry.d, entry.p, entry.r);
    if (entry.type == Decomposition::Ramified ? other != entry.r : other <= entry.r) {
        error = "label is not the canonical (smaller) root";
        return false;
    }
    if (!generatesLabeledPrime(entry.d, entry.p, entry.r, entry.c0, entry.c1)) {
        error = "generator does not generate the labeled prime";
        return false;
    }
    return true;
}

bool provesNoGeneratorImaginary(const mpz_class& d, const mpz_class& q) {
    if (d >= 0) return false;
    // Norm form is positive definite: 4*N(c0 + c1*omega) = (2c0 + c1)^2 + |D| c1^2 (D = 1 mod 4)
    // or c0^2 + |D|/4 c1^2 (D = 0 mod 4). Searching a^2 + |D| b^2 = 4q covers both.
    // A solution (a, b) corresponds to an element only if a = b (mod 2) for D = 1 (mod 4)
    // (a = 2c0 + c1, b = c1), or a is even for D = 0 (mod 4) (a = 2c0, b = c1).
    const mpz_class n = 4 * q, ad = absValue(d);
    for (mpz_class b = 0; ad * b * b <= n; ++b) {
        const mpz_class rest = n - ad * b * b;
        if (mpz_perfect_square_p(rest.get_mpz_t()) == 0) continue;
        mpz_class a;
        mpz_sqrt(a.get_mpz_t(), rest.get_mpz_t());
        const bool element = oneMod4(d) ? mod(a - b, 2) == 0 : mod(a, 2) == 0;
        if (element) return false;  // an element of norm q exists
    }
    return true;
}

bool parseField(const std::string& line, FieldCertificate& cert, std::string& error) {
    cert = {};
    std::string d, factors, gens;
    if (line.rfind("FIELD ", 0) != 0 || !field(line, "D", d) || !field(line, "FACTORS", factors) ||
        !field(line, "GENS", gens) || !parseInteger(d, cert.d)) {
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
    if (!gens.empty())
        for (const auto& item : split(gens, ',')) {
            const auto parts = split(item, ':');
            Generator g;
            if (parts.size() != 4 || !parseInteger(parts[0], g.q) || !parseInteger(parts[1], g.r) ||
                !parseInteger(parts[2], g.c0) || !parseInteger(parts[3], g.c1)) {
                error = "malformed GENS";
                return false;
            }
            cert.generators.push_back(g);
        }
    return true;
}

bool parseEntry(const std::string& line, FieldEntry& entry, std::string& error) {
    entry = {};
    std::string d, p, type, r, gen;
    if (line.rfind("ENTRY ", 0) != 0 || !field(line, "D", d) || !field(line, "P", p) || !field(line, "TYPE", type) ||
        !parseInteger(d, entry.d) || !parseInteger(p, entry.p)) {
        error = "malformed ENTRY record";
        return false;
    }
    if (type == "split") entry.type = Decomposition::Split;
    else if (type == "inert") entry.type = Decomposition::Inert;
    else if (type == "ramified") entry.type = Decomposition::Ramified;
    else { error = "unknown TYPE"; return false; }
    const bool has_r = field(line, "R", r), has_gen = field(line, "GEN", gen);
    if (has_r != has_gen) { error = "R and GEN go together"; return false; }
    if (has_gen) {
        const auto parts = split(gen, ':');
        if (parts.size() != 2 || !parseInteger(r, entry.r) || !parseInteger(parts[0], entry.c0) ||
            !parseInteger(parts[1], entry.c1)) {
            error = "malformed R/GEN";
            return false;
        }
        entry.has_generator = true;
    }
    return true;
}

std::string formatField(const FieldCertificate& cert) {
    std::ostringstream out;
    out << "FIELD D=" << cert.d.get_str() << " FACTORS=";
    for (std::size_t i = 0; i < cert.factors.size(); ++i)
        out << (i ? "*" : "") << cert.factors[i].first.get_str() << "^" << cert.factors[i].second;
    out << " GENS=";
    for (std::size_t i = 0; i < cert.generators.size(); ++i) {
        const auto& g = cert.generators[i];
        out << (i ? "," : "") << g.q.get_str() << ":" << g.r.get_str() << ":" << g.c0.get_str() << ":" << g.c1.get_str();
    }
    return out.str();
}

}  // namespace primechain::extensions
