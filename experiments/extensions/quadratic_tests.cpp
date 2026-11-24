// Tests for the quadratic-field certificate verifier against PARI-generated vectors.
// Usage: primechain-quadratic-cert-tests <quadratic_vectors.txt>
#include "quadratic_certs.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace primechain::extensions;

namespace {

std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// Test oracle: deterministic trial division (inputs here are small). On chain this is a
// finalized block or a Pratt certificate.
bool trialPrime(const mpz_class& n) {
    if (n < 2) return false;
    if (!n.fits_ulong_p()) throw std::runtime_error("test oracle input too large");
    const unsigned long v = n.get_ui();
    for (unsigned long k = 2; k * k <= v; ++k) if (v % k == 0) return false;
    return true;
}

std::vector<mpz_class> primesUpTo(unsigned long limit) {
    std::vector<mpz_class> out;
    for (unsigned long k = 2; k <= limit; ++k) if (trialPrime(k)) out.emplace_back(k);
    return out;
}

std::vector<std::pair<mpz_class, unsigned>> factorize(unsigned long n) {
    std::vector<std::pair<mpz_class, unsigned>> out;
    for (unsigned long p = 2; p * p <= n; ++p) {
        unsigned e = 0;
        while (n % p == 0) { n /= p; ++e; }
        if (e) out.emplace_back(mpz_class(p), e);
    }
    if (n > 1) out.emplace_back(mpz_class(n), 1u);
    return out;
}

// The prime at which a ring becomes eligible: the first prime p with |D| <= p^2 (R3).
long eligiblePrime(const mpz_class& d) {
    for (unsigned long p = 2;; ++p)
        if (trialPrime(p) && eligibleAt(d, p)) return static_cast<long>(p);
}

void expectReject(const FieldCertificate& cert, const std::vector<mpz_class>& primes, const std::string& what) {
    std::string error;
    require(!verifyField(cert, primes, trialPrime, error), "mutation accepted: " + what + " for " + formatField(cert));
}

void mutateField(const FieldCertificate& good, const std::vector<mpz_class>& primes, std::size_t& mutations) {
    if (!good.generators.empty()) {
        const auto& g0 = good.generators[0];
        auto c = good; c.generators[0].c0 += 1; expectReject(c, primes, "altered c0");
        c = good; c.generators[0].c1 += 1; expectReject(c, primes, "altered c1");
        c = good; c.generators.erase(c.generators.begin()); expectReject(c, primes, "missing generator");
        mutations += 3;
        const mpz_class other = otherRoot(good.d, g0.q, g0.r);
        if (other != g0.r) {  // split: claiming the larger root breaks the label rule
            c = good; c.generators[0].r = other; expectReject(c, primes, "label is the larger root");
            ++mutations;
        }
        if (good.generators.size() > 1) {
            c = good; std::swap(c.generators[0], c.generators[1]); expectReject(c, primes, "generators out of order");
            ++mutations;
        }
    }
    auto c = good; c.generators.push_back({mpz_class(997), 0, 1, 1}); expectReject(c, primes, "extra generator");
    c = good; c.d *= 9; c.factors.clear(); expectReject(c, primes, "non-fundamental D");
    c = good; c.factors.front().second += 1; expectReject(c, primes, "wrong factorization");
    mutations += 3;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: primechain-quadratic-cert-tests <quadratic_vectors.txt>");
        std::ifstream in(argv[1]);
        require(static_cast<bool>(in), "open vectors file");
        const auto primes = primesUpTo(200);
        std::map<long, std::size_t> eligible_by_prime;  // eligibility prime -> fields (with or without certificate)
        std::set<long> imaginary_pids, seen;
        std::size_t fields = 0, nocert = 0, nocert_proved = 0, entries = 0, recorded = 0, mutations = 0, line_number = 0;
        long max_abs_d = 0;
        std::string line, error;
        while (std::getline(in, line)) {
            ++line_number;
            if (line.empty() || line[0] == '#') continue;
            const std::string where = " (line " + std::to_string(line_number) + ")";
            if (line.rfind("FIELD ", 0) == 0) {
                FieldCertificate cert;
                require(parseField(line, cert, error), error + where);
                require(formatField(cert) == line, "round-trip encoding" + where);
                require(verifyField(cert, primes, trialPrime, error), "valid certificate rejected: " + error + where);
                require(seen.insert(cert.d.get_si()).second, "field listed twice" + where);
                ++eligible_by_prime[eligiblePrime(cert.d)];
                if (cert.d < 0) imaginary_pids.insert(cert.d.get_si());
                max_abs_d = std::max(max_abs_d, std::abs(cert.d.get_si()));
                ++fields;
                if (fields % 5 == 1) mutateField(cert, primes, mutations);
            } else if (line.rfind("NOCERT ", 0) == 0) {
                std::string d_text = line.substr(line.find("D=") + 2);
                d_text = d_text.substr(0, d_text.find(' '));
                const mpz_class d(d_text);
                require(seen.insert(d.get_si()).second, "field listed twice" + where);
                ++eligible_by_prime[eligiblePrime(d)];
                ++nocert;
                if (d < 0) {  // prove it: some required prime ideal has no generator
                    bool proved = false;
                    for (const auto& q : primes) {
                        if (compareMinkowski(d, q) == Comparison::Below) break;
                        if (decomposition(d, q) != Decomposition::Inert && provesNoGeneratorImaginary(d, q)) {
                            proved = true;
                            break;
                        }
                    }
                    require(proved, "could not prove non-principality of imaginary NOCERT field" + where);
                    ++nocert_proved;
                }
            } else if (line.rfind("ENTRY ", 0) == 0) {
                FieldEntry entry;
                require(parseEntry(line, entry, error), error + where);
                require(verifyEntry(entry, trialPrime, error), "valid entry rejected: " + error + where);
                ++entries;
                if (entry.type != Decomposition::Inert) ++recorded;
                if (entries % 7 == 1) {
                    auto bad = entry;
                    bad.type = entry.type == Decomposition::Split ? Decomposition::Inert : Decomposition::Split;
                    require(!verifyEntry(bad, trialPrime, error), "wrong type accepted" + where);
                    if (entry.has_generator) {
                        bad = entry; bad.c0 += 1;
                        require(!verifyEntry(bad, trialPrime, error), "wrong generator accepted" + where);
                        const mpz_class other = otherRoot(entry.d, entry.p, entry.r);
                        if (other != entry.r) {
                            bad = entry; bad.r = other;
                            require(!verifyEntry(bad, trialPrime, error), "larger-root label accepted" + where);
                            ++mutations;
                        }
                    } else {
                        bad = entry; bad.has_generator = true; bad.r = 0; bad.c0 = 1; bad.c1 = 1;
                        require(!verifyEntry(bad, trialPrime, error), "inert entry with generator accepted" + where);
                    }
                    mutations += 2;
                }
            } else {
                throw std::runtime_error("unknown record" + where);
            }
        }
        // Completeness: every fundamental discriminant eligible at a prime <= 47 appears exactly once.
        const long bound = 47 * 47;
        std::size_t expected = 0;
        for (long n = 3; n <= bound; ++n)
            for (const long d : {n, -n}) {
                std::string ignored;
                if (isFundamental(d, factorize(static_cast<unsigned long>(n)), trialPrime, ignored)) {
                    ++expected;
                    require(seen.count(d) == 1, "missing field D=" + std::to_string(d));
                }
            }
        require(seen.size() == expected, "no extra fields beyond the eligible range");
        // R3 with genesis {2, 3}: |D| <= 4 at p = 2 (D = -3, -4); |D| <= 9 at p = 3 (adds 5, 8, -7, -8).
        require(eligible_by_prime[2] == 2 && eligible_by_prime[2] + eligible_by_prime[3] == 6,
                "2 rings eligible at 2, 6 by genesis frontier 3");
        // Heegner-Stark: exactly nine imaginary quadratic fields have class number one (all |D| <= 163).
        require(imaginary_pids == std::set<long>{-163, -67, -43, -19, -11, -8, -7, -4, -3},
                "certified imaginary PIDs are exactly the nine Heegner-Stark fields");
        require(fields > 300 && entries > 2000 && nocert > 900, "vector file is complete");
        std::cout << "quadratic certificates: PASS\n"
                  << "fields eligible at a prime <= 47 (|D| <= 2209): " << expected << ", each listed once\n"
                  << "field certificates verified: " << fields << "\n"
                  << "eligible fields without certificate: " << nocert << " (imaginary, proved non-PID: "
                  << nocert_proved << ")\n"
                  << "field-chain entries verified: " << entries << " (recorded split/ramified: " << recorded << ")\n"
                  << "mutations rejected: " << mutations << "\nchecks: " << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "quadratic certificate test failed: " << e.what() << '\n';
        return 1;
    }
}
