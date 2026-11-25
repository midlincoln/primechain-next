# Extension certificates: quadratic fields

Status: specification v1, 2026-11-24. It supersedes the v0 draft of
2026-11-22, implementing rings decisions R3, R4 and R7–R10
(`rings-design.md`) and the spin-off model (`monetary-layer.md`). This is
certificate type 1 (R4); general degree will follow as further types.

The rings layer needs two kinds of evidence:

1. A **field certificate**: the ring may open, and its ring of integers is a
   principal ideal domain (PID).
2. **Field-chain entries**: how each main-chain prime q splits or ramifies in
   the ring, with a generator of the labeled prime above q.

## 1. Identity: the fundamental discriminant

Every quadratic field is K = Q(√D) for exactly one **fundamental
discriminant** D: either D ≡ 1 (mod 4) and squarefree, or D = 4m with
m ≡ 2 or 3 (mod 4) and m squarefree. D is the ring's identity, so two
certificates with the same D are the same ring. D ≡ 2, 3 (mod 4) is never a
discriminant, and values such as 20 or 36 belong to smaller rings, not to
new fields.

Squarefreeness is proved by a factorization of |D|. Each prime factor is on
the main chain or has its own Pratt certificate.

## 2. Ring elements: the standard generator ω

- If D ≡ 1 (mod 4): ω = (1 + √D)/2, a root of f(x) = x² − x + (1 − D)/4.
- If D ≡ 0 (mod 4): ω = √(D/4), a root of f(x) = x² − D/4.

Then O_K = Z[ω], and every element is **c₀ + c₁ω with integers c₀, c₁**.
There is no denominator and no parity condition. The generic encoding (R4)
keeps a denominator field for higher degrees; it is 1 here.

Norms:

- D ≡ 1 (mod 4): N(c₀ + c₁ω) = c₀² + c₀c₁ + c₁²·(1 − D)/4.
- D ≡ 0 (mod 4): N(c₀ + c₁ω) = c₀² − (D/4)·c₁².

Example, Q(√17): ω = (1 + √17)/2, so (3 − √17)/2 = 2 − ω, and
N(2 − ω) = 4 − 2 − 4 = −2.

## 3. Ring-prime labels (Dedekind–Kummer)

Since O_K = Z[ω], the prime ideals above a rational prime q correspond
exactly to the irreducible factors of f(x) mod q:

| f mod q | q in K | prime ideals |
|---|---|---|
| two distinct roots r₁, r₂ | split | (q, ω − r₁) and (q, ω − r₂), conjugates |
| one double root r | ramified | (q, ω − r), squared |
| no root | inert | (q) itself |

**The label of a ring prime is (ring, q, r)**, with 0 ≤ r < q. For split q,
x is the prime with the smaller root and y the one with the larger root.
The two roots sum to the trace of ω (1 if D ≡ 1 mod 4, otherwise 0), so the
conjugate root is computed in one step. Labels name ideals, not
generators, so units never cause ambiguity.

The polynomial must be ω's: with x² − D, the prime 2 would be misclassified
in rings like Q(√17) (x² − 17 ≡ (x − 1)² mod 2 looks ramified, but
f = x² − x − 4 ≡ x(x + 1) shows that 2 splits).

**Assets (spin-off model):** a ring prime is named (ring number, q, i), with
i = 0 for the smaller root and 1 for the larger (R8, 2026-11-25; formerly
hash(ring ID, q, r)). The two conjugates are separate assets, each receiving
half of the split's issuance (R8); a ramified prime is one asset.

## 4. Eligibility (R3)

A ring may be submitted while **gate 1 is open and |D| ≤ F²**, where F is the
main-chain frontier. Nothing is claimed in the certificate: validators check
against the current frontier. The ring's **eligibility prime**, used for
history and display, is the first prime p with p² ≥ |D|. With genesis
{2, 3} (F = 3), D ∈ {5, 8, −3, −4, −7, −8} are eligible from the start.

## 5. PID certificate

**Theorem (Minkowski).** Every ideal class contains an integral ideal of norm
≤ M_K, where M_K = √D/2 for D > 0 and M_K = (2/π)·√|D| for D < 0. If every
prime ideal of norm ≤ M_K is principal, the class group is trivial.

Inert primes give the principal ideal (q). For split q, a generator of one
prime gives the conjugate's generator by conjugation. So the certificate
lists, for **every prime q ≤ M_K that is not inert**, in increasing order:

  (q, r, c₀, c₁), where r is the **smaller** root of f mod q and c₀ + c₁ω
  generates (q, ω − r).

**Validators check:**

1. D is fundamental, from the factorization of |D|.
2. Walk through the primes q ≤ M_K (from the chain). Comparisons use
   integers only: q² ≤ D/4 for real fields; for imaginary fields
   4|D| ≤ π²q², with a stored rational enclosure of π², and equality is
   impossible.
3. For each q: its type from the Kronecker symbol (D/q). Inert q is
   skipped. Otherwise the next certificate item must have this q, f(r) ≡ 0
   (mod q), r must be the smaller root (or the double root), and the
   generator must satisfy **N = ±q** (positive for D < 0) and
   **c₀ + c₁r ≡ 0 (mod q)**, so it generates exactly the labeled prime.
4. Nothing is missing and nothing is extra.

**To be added with B3 (agreed 2026-11-25):** for D > 0 a unit ε ≠ ±1, used to
reject padded generators (R10), and a size limit of 256 KB (R4). The current
verifier and vectors predate both.

No certificate exists for a non-PID. Example: Q(√−5) (D = −20,
M_K ≈ 2.85). The prime 2 ramifies, and no element has norm 2.

## 6. Field-chain entries (R7–R10)

Entries follow main-chain primes in order, up to the frontier, in segments
(R7, revised 2026-11-25). **Inert primes are marked rows without a generator;
only split and ramified primes are paid** (R9). An entry is

  (ring, p, type, r, c₀, c₁): for split p, the smaller root r and a
  generator of (p, ω − r); for ramified p, the double root.

Validators check that p is a finalized main-chain prime; the type matches
(D/p); f(r) ≡ 0 (mod p); r is the smaller root for split p, or the double
root for ramified p; N = ±p; and c₀ + c₁r ≡ 0 (mod p). They also check that
every row of the segment has the right type, that no prime is missing, and
that there is one row per p per ring (R10). One entry gives one proof and one
miner reward, while issuing two assets for split p (R8, spin-offs).

Why a generator proves primality: if π = αβ, then ±p = N(α)N(β), so one
factor has norm ±1 and is a unit. Primality of the ring prime reduces to the
main chain's Pratt certificate for p.

## 7. Encoding (experimental text form)

```
FIELD D=<D> FACTORS=<p1>^<e1>*... GENS=<q>:<r>:<c0>:<c1>,...
ENTRY D=<D> P=<p> TYPE=<split|ramified|inert> [R=<r> GEN=<c0>:<c1>]
```

`TYPE=inert` lines appear in the test vectors only, to test the skip rule;
the v1 vectors predate marked inert rows (R9). The canonical binary encoding,
commit–reveal wrapping (R5), the founder record (R6) and the on-chain ring
blocks come with B3.

## 8. Implementation and results (2026-11-24)

`experiments/extensions/`: the PARI generator (`gen_quadratic_vectors.gp`),
its output (`quadratic_vectors.txt`: every quadratic field eligible at a
prime ≤ 47, i.e. |D| ≤ 2209, plus entries for p < 2000 in eight rings), and
the GMP verifier with tests (CTest `quadratic_field_certificates`).

- **Completeness:** the test independently enumerates every fundamental
  discriminant with |D| ≤ 2209 (1,343 fields) and checks that each appears
  exactly once.
- **351** PID certificates verified, independently of PARI; **992**
  eligible non-PIDs. For the **664** imaginary ones, the verifier proves
  non-principality itself by a finite norm search.
- The certified imaginary PIDs are exactly the nine Heegner–Stark fields.
- **2,424** field-chain entries verified (1,181 split or ramified, the rest
  inert), including Q(√94) (D = 376).
- **1,375** corrupted variants rejected: altered generators, a larger-root
  label, a missing or extra generator, reordering, a non-fundamental D, a
  wrong factorization, a wrong type, and an inert prime with a generator.
- Eligibility at genesis: 2 rings at F = 2, and 6 by F = 3.
- Clean under AddressSanitizer and UndefinedBehaviorSanitizer.

Real quadratic fields without a certificate are not proved to be non-PIDs
(their unit groups are infinite), which does not matter: only PIDs open.
