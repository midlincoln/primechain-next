# Extension certificates: quadratic fields (v0)

Status: draft specification, 2026-11-22. It has no effect on the live chain or
the main-chain experiments.

This document specifies the first layer of PrimeChain's ring work:

1. A **field certificate** (an "F-proof"). It shows that a number field K may
   open at main-chain frontier F, and that its ring of integers O_K is a
   principal ideal domain (PID), so every prime ideal has a single generator.
2. A **prime decomposition entry** for a field chain. It shows how a rational
   prime p factors in O_K, with a generator for every prime ideal above p.

v0 covers **quadratic fields only**. They avoid the two hard general-degree
problems: a quadratic field is identified exactly by its discriminant, and its
ring of integers is known in closed form. General degree follows in a later
version (Dedekind–Kummer decomposition, certified integral bases).

## 1. Identity: the fundamental discriminant

Every quadratic field is `K = Q(√D)` for exactly one **fundamental
discriminant** D ≠ 1:

- `D ≡ 1 (mod 4)` and D is squarefree, or
- `D = 4m` with `m ≡ 2 or 3 (mod 4)` and m squarefree.

D is the field's identity: two certificates with the same D describe the same
field, so the second one is a duplicate. The ring of integers is

- `O_K = Z[(1 + √D)/2]` when `D ≡ 1 (mod 4)`;
- `O_K = Z[√m]` when `D = 4m`.

Every element is written uniformly as `α = (a + b√D)/2`, where `a, b` are
integers with `a ≡ b·D (mod 2)`. Its norm is `N(α) = (a² − D·b²)/4`.

Squarefreeness is proved by a factorization of |D| into primes. Each prime
must be on the main chain (≤ F) or carry its own Pratt certificate.

## 2. Eligibility: Minkowski bound below the frontier

The Minkowski bound of a quadratic field is

- real (D > 0): `M_K = √D / 2`;
- imaginary (D < 0): `M_K = (2/π)·√|D|`.

The field is **eligible at frontier F** when `M_K ≤ F`. Validators must decide
this with integers only:

- real: `D ≤ 4·F²`;
- imaginary: `4·|D| ≤ π²·F²`. π² is irrational, so equality never happens.
  Validators compare against a rational interval `[L, U]` containing π²,
  narrow enough to decide `4|D| < L·F²` or `4|D| > U·F²`. The required
  precision grows with F, and the reference interval is part of the protocol
  constants.

Alternative rules (for example `|D| ≤ F`, or Bach's bound if GRH is
established) only change this section.

## 3. Class number one: the PID certificate

**Theorem (Minkowski).** Every ideal class of O_K contains an integral ideal of
norm ≤ M_K. That ideal factors into prime ideals of norm ≤ M_K. Therefore, if
every prime ideal of norm ≤ M_K is principal, the class group is trivial.

In a quadratic field, a rational prime q decomposes according to the Kronecker
symbol `(D/q)`:

| `(D/q)` | Decomposition | Prime ideals | Norms |
|---|---|---|---|
| +1 | split: `qO_K = 𝔮·𝔮'` | two conjugates | q, q |
| −1 | inert: `qO_K` is prime | one, `(q)` itself | q² |
| 0 | ramified: `qO_K = 𝔮²` | one | q |

- An inert prime ideal is `(q)`, which is principal automatically.
- For a split prime, if `𝔮 = (α)` then `𝔮' = (ᾱ)`, so one generator covers both.
- An element whose norm is ±q (q prime) generates a prime ideal above q.

**PID certificate.** For every prime `q ≤ M_K` with `(D/q) ≠ −1`, one element
`α_q = (a + b√D)/2` with `N(α_q) = ±q`.

**Verification.**
1. Enumerate the primes `q ≤ M_K` from the main chain. Compare `q² ≤ 4·M_K²`
   (real) or the π-interval form (imaginary) using integers only.
2. For each q, compute `(D/q)`.
3. Check that the certificate lists exactly the q with `(D/q) ≠ −1`, in
   increasing order, with no extras.
4. For each listed q, check `a ≡ b·D (mod 2)` and `a² − D·b² = ±4q`. For
   imaginary fields the norm is positive, so the sign is +.

No certificate exists for a non-PID field, because some required norm
equation has no solution. Example: `D = −20`, `K = Q(√−5)`, `M_K ≈ 2.85`. The
prime 2 ramifies, and `a² + 20b² = 8` has no integer solution, so 2 has no
generator.

## 4. Prime decomposition entry (the field chain)

For each rational prime p, in main-chain order, the field chain of K records:

- p and a link to the main-chain block containing p;
- the decomposition type from `(D/p)`;
- for split or ramified p, a generator `(a, b)` with `a² − D·b² = ±4p`.

Verification is steps 2 and 4 above for this single p. Completeness is
automatic for degree 2: `Σ eᵢfᵢ = 2` holds for each row of the table.

Real quadratic generators can be very large (Q(√94) already needs 7-digit
coefficients), so all arithmetic uses arbitrary-precision integers.
**Canonical generators** (one per ideal, modulo units) are not required here.
They are needed only before prime elements become assets.

## 5. Encoding (v0, experimental)

Text lines, one record per line. Integers are in decimal with an optional sign.

```
FIELD D=<D> F=<F> FACTORS=<p1>^<e1>*<p2>^<e2>*... GENS=<q>:<a>:<b>,<q>:<a>:<b>,...
SPLIT D=<D> P=<p> TYPE=<split|inert|ramified> GEN=<a>:<b>     (GEN omitted for inert)
```

A binary canonical encoding, commit–reveal wrapping, discoverer attribution and
F-proof chain blocks come after the verifier is settled.

## 6. Test expectations

- At F = 2, exactly the 8 quadratic fields D ∈ {5, 8, 12, 13, −3, −4, −7, −8}
  are eligible, and all have empty certificates. (Their Minkowski bounds are
  below 2, so no prime needs a generator.)
- At F = 3, there are 18 eligible quadratic fields. D = −15 and D = −20 are
  eligible, but no certificate exists for them.
- Valid certificates are accepted. Rejected: wrong or missing generators,
  extra entries, a non-fundamental D, wrong decomposition type, and a field
  that is not eligible at the claimed F.

## 7. First results (2026-11-22)

`experiments/extensions/` contains the PARI generator (`gen_quadratic_vectors.gp`),
its output (`quadratic_vectors.txt`, every quadratic field eligible at a prime
F ≤ 50), and the C++ verifier with tests (CTest `quadratic_field_certificates`,
which needs GMP):

- **1,165** class-number-one certificates verified, independently of PARI.
- **3,182** eligible fields have no certificate. For the **1,653** imaginary
  ones, the verifier proves non-principality itself by a finite norm search.
- The certified imaginary PIDs are exactly the nine Heegner–Stark fields,
  D ∈ {−3, −4, −7, −8, −11, −19, −43, −67, −163}.
- **2,121** prime decomposition entries verified (D = 8, 5, 28, 94, −4, −3,
  −163; p < 2000).
- **1,868** corrupted variants rejected: altered or missing generators,
  elements outside O_K, reordering, extra entries, a non-fundamental D, a
  wrong factorization, a frontier below the eligibility prime, and wrong
  decomposition types.
- Generator coefficients reach 39 digits by D = 8401, so arbitrary-precision
  arithmetic is required.
- Clean under AddressSanitizer and UndefinedBehaviorSanitizer.

Real quadratic fields without a certificate are not proved to be non-PIDs.
Their unit groups are infinite, so a finite search does not settle them.
