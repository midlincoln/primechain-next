// Tests for the quadratic-field certificate verifier against PARI-generated vectors.
// Usage: primechain-quadratic-cert-tests <quadratic_vectors.txt>
#include "quadratic_certs.hpp"

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

// Test oracle: deterministic trial division (inputs here are < 10^6). On chain this is a
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

mpz_class previousPrime(const mpz_class& p) {
    for (mpz_class k = p - 1; k >= 2; --k) if (trialPrime(k)) return k;
    return 0;
}

void expectReject(const FieldCertificate& cert, const std::vector<mpz_class>& primes, const std::string& what) {
    std::string error;
    require(!verifyField(cert, primes, trialPrime, error), "mutation accepted: " + what + " for " + formatField(cert));
}

void mutateField(const FieldCertificate& good, const std::vector<mpz_class>& primes, std::size_t& mutations) {
    if (!good.generators.empty()) {
        auto c = good; c.generators[0].a += 2; expectReject(c, primes, "altered generator a");
        c = good;  // +2|b|+2 keeps parity and never lands on the conjugate (a, -b), which is also valid
        c.generators[0].b += 2 * abs(good.generators[0].b) + 2; expectReject(c, primes, "altered generator b");
        c = good; c.generators.erase(c.generators.begin()); expectReject(c, primes, "missing generator");
        c = good; c.generators[0].a += 1; expectReject(c, primes, "element outside O_K");
        mutations += 4;
        if (good.generators.size() > 1) {
            c = good; std::swap(c.generators[0], c.generators[1]); expectReject(c, primes, "generators out of order");
            ++mutations;
        }
    }
    auto c = good; c.generators.push_back({good.f + 1000, 1, 1}); expectReject(c, primes, "extra generator");
    c = good; c.d *= 9; c.factors.clear(); expectReject(c, primes, "non-fundamental D");
    c = good; c.factors.front().second += 1; expectReject(c, primes, "wrong factorization");
    mutations += 3;
    const mpz_class earlier = previousPrime(good.f);
    if (earlier != 0) {
        c = good; c.f = earlier;
        std::string error;
        bool accepted = false;
        try { accepted = verifyField(c, primes, trialPrime, error); } catch (const std::exception&) {}
        require(!accepted, "accepted at a frontier below the field's eligibility prime: " + formatField(good));
        ++mutations;
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: primechain-quadratic-cert-tests <quadratic_vectors.txt>");
        std::ifstream in(argv[1]);
        require(static_cast<bool>(in), "open vectors file");
        const auto primes = primesUpTo(200);
        std::map<long, std::size_t> eligible_by_f;
        std::set<long> imaginary_pids;  // F -> fields (with or without certificate)
        std::size_t fields = 0, nocert = 0, nocert_proved = 0, splits = 0, mutations = 0;
        std::size_t line_number = 0;
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
                const mpz_class earlier = previousPrime(cert.f);
                require(earlier == 0 || !eligibleAt(cert.d, earlier), "F is not the first eligible prime" + where);
                ++eligible_by_f[cert.f.get_si()];
                if (cert.d < 0) imaginary_pids.insert(cert.d.get_si());
                ++fields;
                if (fields % 7 == 1) mutateField(cert, primes, mutations);  // sample across the file
            } else if (line.rfind("NOCERT ", 0) == 0) {
                // NOCERT D=<D> F=<F> H=<h>: the field is eligible but not a PID.
                std::string d_text = line.substr(line.find("D=") + 2);
                d_text = d_text.substr(0, d_text.find(' '));
                std::string f_text = line.substr(line.find("F=") + 2);
                f_text = f_text.substr(0, f_text.find(' '));
                const mpz_class d(d_text), f(f_text);
                require(eligibleAt(d, f), "NOCERT field not eligible at its F" + where);
                ++eligible_by_f[f.get_si()];
                ++nocert;
                if (d < 0) {
                    // Prove it: some required prime ideal has no generator.
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
            } else if (line.rfind("SPLIT ", 0) == 0) {
                SplitEntry entry;
                require(parseSplit(line, entry, error), error + where);
                require(verifySplit(entry, trialPrime, error), "valid split entry rejected: " + error + where);
                ++splits;
                if (splits % 11 == 1) {
                    auto bad = entry;
                    bad.type = entry.type == Decomposition::Split ? Decomposition::Inert : Decomposition::Split;
                    require(!verifySplit(bad, trialPrime, error), "wrong type accepted" + where);
                    if (entry.has_generator) {
                        bad = entry; bad.a += 2;
                        require(!verifySplit(bad, trialPrime, error), "wrong generator accepted" + where);
                    } else {
                        bad = entry; bad.has_generator = true; bad.a = 1; bad.b = 1;
                        require(!verifySplit(bad, trialPrime, error), "inert entry with generator accepted" + where);
                    }
                    mutations += 2;
                }
            } else {
                throw std::runtime_error("unknown record" + where);
            }
        }
        // Cross-check with the hand analysis: 8 quadratic fields at F = 2, 18 by F = 3,
        // and exactly D = -15, -20 lack certificates by F = 3.
        require(eligible_by_f[2] == 8, "8 quadratic fields at F = 2");
        require(eligible_by_f[2] + eligible_by_f[3] == 18, "18 quadratic fields by F = 3");
        require(mpz_class(-20) != 0 && !provesNoGeneratorImaginary(-4, 2) && provesNoGeneratorImaginary(-20, 2),
                "Q(sqrt(-5)): no element of norm 2; Q(i): 1 + i has norm 2");
        // Heegner-Stark: exactly nine imaginary quadratic fields have class number one.
        // All nine are eligible by F = 11, well inside this file (F <= 50).
        require(imaginary_pids == std::set<long>{-163, -67, -43, -19, -11, -8, -7, -4, -3},
                "certified imaginary PIDs are exactly the nine Heegner-Stark fields");
        require(fields > 1000 && splits > 2000 && nocert > 3000, "vector file is complete");
        std::cout << "quadratic certificates: PASS\n"
                  << "field certificates verified: " << fields << "\n"
                  << "eligible fields without certificate: " << nocert << " (imaginary, proved non-PID: "
                  << nocert_proved << ")\n"
                  << "prime decomposition entries verified: " << splits << "\n"
                  << "mutations rejected: " << mutations << "\nchecks: " << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "quadratic certificate test failed: " << e.what() << '\n';
        return 1;
    }
}
