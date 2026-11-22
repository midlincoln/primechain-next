#pragma once
// Verifier for quadratic-field certificates (docs/extension-certificates.md, v0).
// Independent of PARI: checks only integer identities and the Minkowski argument.
#include <gmpxx.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace primechain::extensions {

// Deterministic primality oracle for integers the verifier must trust as prime.
// On chain this is "the prime is in a finalized block, or carries a Pratt certificate".
using PrimeOracle = std::function<bool(const mpz_class&)>;

struct Generator {
    mpz_class q;  // rational prime below the generated prime ideal
    mpz_class a;  // element (a + b*sqrt(D)) / 2
    mpz_class b;
};

struct FieldCertificate {
    mpz_class d;                                       // fundamental discriminant
    mpz_class f;                                       // claimed eligibility frontier (a prime)
    std::vector<std::pair<mpz_class, unsigned>> factors;  // |D| = prod p^e
    std::vector<Generator> generators;                 // increasing q, exactly the non-inert q <= M_K
};

enum class Decomposition { Split, Inert, Ramified };

struct SplitEntry {
    mpz_class d;
    mpz_class p;
    Decomposition type;
    bool has_generator{false};
    mpz_class a, b;
};

enum class Comparison { Below, Above };  // strict: equality is impossible where used

// True when (a + b*sqrt(d))/2 is in O_K and has norm +-4q/4 = +-q (positive for d < 0).
bool generatesPrimeAbove(const mpz_class& d, const mpz_class& q, const mpz_class& a, const mpz_class& b);

bool isFundamental(const mpz_class& d, const std::vector<std::pair<mpz_class, unsigned>>& factors,
                   const PrimeOracle& is_prime, std::string& error);

// Minkowski bound comparisons using integers and a rational enclosure of pi^2.
Comparison compareMinkowski(const mpz_class& d, const mpz_class& x);  // M_K vs x (x > 0)
bool eligibleAt(const mpz_class& d, const mpz_class& f);              // M_K <= f

Decomposition decomposition(const mpz_class& d, const mpz_class& p);

// Full check of a field certificate. `primes` must contain every prime up to M_K
// (taken from the main chain).
bool verifyField(const FieldCertificate& cert, const std::vector<mpz_class>& primes,
                 const PrimeOracle& is_prime, std::string& error);

bool verifySplit(const SplitEntry& entry, const PrimeOracle& is_prime, std::string& error);

// Imaginary fields only: a finite search proves that no element has norm q, so a
// required prime ideal above q is not principal and the field has no certificate.
bool provesNoGeneratorImaginary(const mpz_class& d, const mpz_class& q);

// Text encoding from the spec (section 5).
bool parseField(const std::string& line, FieldCertificate& cert, std::string& error);
bool parseSplit(const std::string& line, SplitEntry& entry, std::string& error);
std::string formatField(const FieldCertificate& cert);

}  // namespace primechain::extensions
