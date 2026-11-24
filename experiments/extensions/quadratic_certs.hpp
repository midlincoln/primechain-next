#pragma once
// Verifier for quadratic-field certificates (docs/extension-certificates.md).
// Independent of PARI: checks only integer identities, Minkowski's theorem and
// Dedekind–Kummer (prime ideals above q <-> roots of f mod q).
//
// Conventions: D is a fundamental discriminant; omega = (1 + sqrt(D))/2 if D = 1 (mod 4),
// omega = sqrt(D/4) if D = 0 (mod 4); f is omega's minimal polynomial and O_K = Z[omega].
// Elements are c0 + c1*omega. The prime ideal labeled (D, q, r) is (q, omega - r), where r is a
// root of f mod q.
#include <gmpxx.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace primechain::extensions {

// "This integer is prime" as the chain knows it: in a finalized block, or by Pratt certificate.
using PrimeOracle = std::function<bool(const mpz_class&)>;

struct Generator {
    mpz_class q;   // rational prime below the ideal
    mpz_class r;   // label: root of f mod q (the smaller root for split q)
    mpz_class c0;  // generator c0 + c1*omega of the ideal (q, omega - r)
    mpz_class c1;
};

struct FieldCertificate {
    mpz_class d;                                          // fundamental discriminant (the ring's identity)
    std::vector<std::pair<mpz_class, unsigned>> factors;  // |D| = prod p^e
    std::vector<Generator> generators;                    // increasing q: every non-inert q <= M_K
};

enum class Decomposition { Split, Inert, Ramified };

struct FieldEntry {  // one field-chain entry (spec section 4)
    mpz_class d;
    mpz_class p;
    Decomposition type;
    bool has_generator{false};
    mpz_class r, c0, c1;
};

enum class Comparison { Below, Above };  // M_K < x, or M_K > x (equality is impossible where used)

mpz_class norm(const mpz_class& d, const mpz_class& c0, const mpz_class& c1);  // N(c0 + c1*omega)
bool isRoot(const mpz_class& d, const mpz_class& q, const mpz_class& r);         // f(r) = 0 mod q, 0 <= r < q
mpz_class otherRoot(const mpz_class& d, const mpz_class& q, const mpz_class& r); // the conjugate root mod q
Decomposition decomposition(const mpz_class& d, const mpz_class& q);

// The element generates exactly the prime ideal labeled (d, q, r): norm +-q (positive if d < 0)
// and membership in (q, omega - r), i.e. c0 + c1*r = 0 mod q.
bool generatesLabeledPrime(const mpz_class& d, const mpz_class& q, const mpz_class& r,
                           const mpz_class& c0, const mpz_class& c1);

bool isFundamental(const mpz_class& d, const std::vector<std::pair<mpz_class, unsigned>>& factors,
                   const PrimeOracle& is_prime, std::string& error);

// Eligibility (R3): |D| <= F^2. eligibleAtPrime = the first prime p with p^2 >= |D|.
bool eligibleAt(const mpz_class& d, const mpz_class& frontier);

// Minkowski comparison, integers only (pi^2 enclosed rationally).
Comparison compareMinkowski(const mpz_class& d, const mpz_class& x);

// Full check of a field certificate. `primes` must contain every prime up to M_K (from the chain).
bool verifyField(const FieldCertificate& cert, const std::vector<mpz_class>& primes,
                 const PrimeOracle& is_prime, std::string& error);

bool verifyEntry(const FieldEntry& entry, const PrimeOracle& is_prime, std::string& error);

// Imaginary fields only: a finite search proves that no element has norm q.
bool provesNoGeneratorImaginary(const mpz_class& d, const mpz_class& q);

bool parseField(const std::string& line, FieldCertificate& cert, std::string& error);
bool parseEntry(const std::string& line, FieldEntry& entry, std::string& error);
std::string formatField(const FieldCertificate& cert);

}  // namespace primechain::extensions
