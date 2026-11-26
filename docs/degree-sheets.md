# Degree sheets: adding rings of a new degree (2026-11-25)

**Agreed:** every degree answers the same list of questions (its degree sheet). A degree
is added by a validator vote plus a software upgrade on each validator, with activation
after notice (R4, R2, D12). No smart-contract machine is needed.
**Proposed:** the safety steps and the general sheet below.

## Three layers

| Layer | Contents | Changed by |
|---|---|---|
| Common rules | PID required; only primes of norm q pay, e/n each (R12); computed awards (R13); opening (R5) | protocol upgrade |
| Degree sheet | the answers below, and its checking code | vote, plus upgrade, with notice |
| Parameters | sizes, limits, rewards (`protocol-parameters.md`) | vote with notice |

## The sheet

Degree 2 is agreed (`extension-certificates.md`, `ring-records.md`). Degree 3 and the
general sheet are **proposed**, tested in `experiments/degrees/` (Python, not engine code).

| # | Question | Degree 2 | Degree 3 (proposed) | Any degree n (proposed) |
|---|---|---|---|---|
| 1 | Unique name | D | reduced binary cubic form (a, b, c, d) | hash of the certificate, with the duplicate test below |
| 2 | Full ring of integers | D fundamental, Z[ω] | table of the form; Pohst–Zassenhaus test at each p with p² dividing D | multiplication table on a basis with e₀ = 1; the same test |
| 2b | Is it a field? | D is not a square | the form has no rational root | irreducible characteristic polynomial of a generating element |
| 3 | Signature, Minkowski bound | √D/2 or (2/π)√abs(D) | sign of D; 2√D/9 or 8√abs(D)/(9π) | signature of the trace form; (n!/nⁿ)(4/π)^r₂ √abs(D) |
| 4 | Eligibility | abs(D) ≤ F² | abs(D) ≤ F² | abs(D) ≤ F² (the bound is then below F) |
| 5 | Galois, subfields | Galois, none | usually not Galois, none | from the certificate, optional automorphisms |
| 6 | Generator, norm | c₀ + c₁ω | three integers on 1, ω, θ; determinant | n integers; determinant |
| 7 | PID certificate | generators up to the bound | a generator of every prime of norm ≤ bound | the same; the residue degrees give the number required |
| 8 | Padding | not shortened by ε or 1/ε | length cap from the listed units | the same cap |
| 9 | Splitting types | 3 | 5: PQR; P·Q (norms q, q²); inert; P²Q; P³ | by residue degrees |
| 10 | Label of a prime of norm q | root r of f mod q | values of (1, ω, θ) mod q | values of the basis mod q |
| 11 | What pays | norm q | norm q | norm q |
| 12 | Generators per row | 1 | one per prime of norm q; with all three, two suffice | the same; with all n, n − 1 suffice |
| 13 | Awards below | main chain only | main chain only | open PID subrings (R11); non-PID subrings are skipped |
| 14 | Limits | 256 KB certificate | same | same; row checks grow with n |

Notes:

- **Labels are one rule for all degrees.** The map O → F_q with the ideal as kernel is
  given by its values on the basis. For quadratics these are (1, r). It also works where a
  polynomial mod q has too few roots. Assets (ring, q, i) are ordered by label.
- **Cubic form:** ωθ = −ad, ω² = −ac + bω − aθ, θ² = −bd + dω − cθ. Reduced forms give
  exactly PARI's 370 cubic fields with abs(D) ≤ 2209.
- **2b and 3 are needed for safety:** residue degrees cannot prove that biquadratic rings are
  fields, and a wrong signature gives too small a Minkowski bound, which could admit a
  non-PID.
- **Length cap:** every coefficient of a generator has at most bits(q)/n + the sum of the
  lengths of the listed units (the longer of u and 1/u) + a margin. The unit rule of
  degree 2 fails with two or more units. With no margin, 9 of 7,378 generators were above
  it, by less than a bit. **Open:** a proof that a margin always suffices, and that the
  listed units have full rank.
- **Open:** how miners find generators, large discriminants, byte formats.

## Rings inside rings (B, proposed 2026-11-26)

Example: K = Q(⁴√2), from x⁴ − 2 (a PID, D = −2048, not Galois) contains Q(√2). At q = 7,
K has two primes of norm 7 and one of norm 49; Q(√2) has two primes of norm 7, with roots
3 and 4. **Both norm-7 primes of K lie over the same prime of Q(√2), and which one depends
on how Q(√2) sits inside K:** √2 ↦ x² puts them over root 4, √2 ↦ −x² over root 3. Both
are valid. The same happens at 23, 31, 47, 71 and 79 (tested with PARI).

Award flow for one prime X of K (R11): miner 50%, founder 15%, 35% upstream. With Q(√2)
open, the Q(√2) prime under X receives the 35%; its holders keep 17.5% and pass 17.5% to
the holders of 7. The other Q(√2) prime over 7 receives nothing from this row. In
Q(ζ8) = Q(i, √2) at q = 17, each prime of K lies over three primes, one in each quadratic
subring: each receives 11.67%, keeps 5.83%, and 17 receives 3 × 5.83% = 17.5%.

**This is one subring with two possible names,** not two subrings: x² and −x² generate the
same elements. Q(√2) has a symmetry (√2 ↔ −√2) that swaps its two primes over 7, and
nothing in K prefers one name. Genuinely different subrings also occur (a cubic ring sits
in three different ways inside the degree-6 ring that contains all three of its versions).

Rules (agreed 2026-11-26):

1. **Every way of fitting in counts, with an equal part** (as R11 already says). In the
   example each of the two Q(√2) primes over 7 receives 8.75% of each new prime and 7
   receives 17.5%. A genuine tie is split; nobody chooses who is paid. For genuinely
   different subrings this is the same as counting each once.
2. **Links are recorded with all ways,** as the images of the subring's basis in K.
   Validators check that each is a ring map and that the list is complete (method to be
   tested). If the subring is already open, the certificate of K carries the link;
   otherwise **anyone may submit it later**, with a normal fee. Awards use it from the main
   block that includes it, never before; until then the subring is skipped (R11).
3. Labels go down directly: the label of X restricted to the subring's basis.

Checked (`experiments/degrees/twice.py`): in x⁴ − 2 over Q(√2) (666 new primes, q < 5000)
and in Q(ζ8) over its three quadratic subrings (644 new primes), each new prime passes
exactly 35% down, less at most 2 units of rounding, and only to primes over the same q.
No prime is counted twice.

## Activation (proposed)

1. The proposal names the sheet, its test vectors and the exact code commit (by hash).
2. Validators vote (A3-2). A passed vote alone does not start anything.
3. Each operator upgrades, **one validator at a time**. The new code stays off.
4. The validator runs the test vectors. Its "ready" message (as for new validators, A3-3)
   carries the hash of the results. Equal results are required from all active
   validators, or the degree does not activate.
5. At the activation height the degree opens. Before it, blocks with such rings are
   invalid.

Reasons and consequences:

- **The same result everywhere:** exact integers only, no floating point, bounded work.
- **A validator without the code stops** at the first block with such rings, since ring
  money is in `state_root`. With three validators and quorum 2, one laggard leaves no
  spare. Hence "all ready", not only a quorum. (If one validator refuses forever, it has
  to be removed by vote first.)
- **Old rules stay in the code,** by height, so that a replay gives the same chain.
- **A faulty checker:** emergency pause of the degree (R2, per ring today; per degree to be
  added). Units already issued are not taken back.
- Miners, wallets and explorers must upgrade too.
- Trial on a test network first.

## General sheet (proposed)

One checker for any degree: polynomial, integral basis with proof of maximality,
generators by coefficients, norm by determinant. Then a degree could be enabled by a vote
on limits, without new code. Special sheets (quadratic, cubic) stay shorter and faster.

**Duplicates** (15 pairs of equal discriminant were told apart by a prime at most 43):
a new ring is compared only with open rings of the same degree and
discriminant. If some prime q below a limit splits differently in the two, they are
different. Otherwise it is refused as a duplicate. No ring can enter twice. Up to degree 6
different fields always differ at some prime (Perlis; to be checked); from degree 7 a few
pairs look alike, and the second of them is refused.
