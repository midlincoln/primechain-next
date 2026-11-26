# Degrees 3 to 7: certificates and rows (2026-11-25, exploratory)

PARI (`gen.gp`, `run.sh`) writes multiplication tables, PID generators and rows;
`degree.py` checks them for **any degree** with integers only and nothing from PARI.
`forms.py` lists cubic fields; `check.py file...` runs the checks; `runs.gp` (PARI only)
measures runs and subrings. Python, not engine code. Run under a memory cap.

| Degree | certificates ok | rows ok | corruptions caught | largest certificate | check, certificate / row |
|---|---|---|---|---|---|
| 3, abs(D) <= 2209 (all 370 fields; 59 not PIDs) | 311 | 5,287 | 2,249 of 2,249 | 115 B | 8 / 1.6 ms |
| 4, abs(D) <= 2209 (2 not PIDs) | 167 | 2,839 | 1,117 of 1,117 | 176 B | 10 / 2.5 ms |
| 5, 25 small fields | 25 | 425 | 203 of 203 | 300 B | 18 / 5 ms |
| 6, 40 small fields | 40 | 680 | 284 of 284 | 492 B | 40 / 10 ms |
| 7, 6 small fields | 6 | 102 | 44 of 44 | 742 B | 69 / 18 ms |

Findings:

1. **Cubic names:** reduced binary cubic forms give exactly PARI's 370 fields.
2. **Labels:** a prime of norm q is named by the values of the basis elements mod q. One
   rule for all degrees (for quadratics it is the root r), also where a polynomial mod q
   has too few roots (14 cubic rows, 2 quartic).
3. **Checks a certificate needs above degree 2:** that the ring is a field (residue
   degrees alone cannot decide 13 quartic fields, all biquadratic), and its signature (a wrong
   one gives too small a Minkowski bound). Both were missing in the first version.
4. **Padding:** "no single unit shortens it" (the quadratic rule) fails with two or more
   units: generators up to 7 times the best length got stuck. A cap works instead:
   longest coefficient at most bits(q)/n + total length of the listed units (and their
   inverses); 9 of 7,378 generators were above it, by less than 1 bit. No proof yet that
   a margin always suffices.
5. **All n primes found:** the last generator is q divided by the others, so n - 1
   suffice (461 cubic rows).
6. **Runs to 20,000:** longest unpaid 22, longest paid 27; the limits 1,024 and 32 hold.
   Average pay per row 0.2450-0.3322 against 1/n.
7. **Subrings:** 6 of 39 quadratic subfields of the biquadratic PIDs are not PIDs (for
   example Q(sqrt 10)), so they can never be opened and are skipped (R11).
8. **Equal discriminants:** 15 pairs, told apart by the first prime 3 to 43.

## Mining (A, 2026-11-26)

`mine.py` finds generators: the prime is a lattice; LLL under the complex embeddings gives
a short element whose norm is q times a small m; m = 1 gives the generator, otherwise the
miner divides by generators of earlier rows (the chain is the miner's factor base). Then
it shortens with the units. Every result is checked with `degree.py`.

- 5 small cubic rings, all 310 rows up to 300: all found, 22 ms per row (Python).
- `bigmine.py`: six complex cubic PIDs with abs(D) from 1.5·10⁵ to 9.4·10⁸, primes of
  3, 9 and 15 digits: every generator found, 7 to 141 ms per prime, no division needed.
- Generators grow with the regulator, as for quadratics: largest coefficient about
  14–28 bits at abs(D) ≈ 10⁵, up to 980 bits at 3·10⁷ and 5,400 bits at 9·10⁸.
- Certificate generators: about 0.1 KB at abs(D) ≈ 1.5·10⁵, 5 KB at 3.6·10⁶, 59 KB at
  3.4·10⁷, 0.7–1.3 MB at 9·10⁸ (above the 256 KB limit).
- Mining cost hardly depends on q: finding is as cheap as checking (as on the main
  chain). Real cubic rings (two units) are not measured yet.

- Real cubic rings (two units, `realcubic.txt`): every generator found, 6–74 ms per
  prime; their units and generators stay small (largest unit 190 bits at D ≈ 3·10⁹), so
  certificates are only 14–48 KB at D ≈ 2–3·10⁹. Complex cubics (one large unit) are the
  costly case: 0.7–1.3 MB at abs(D) ≈ 9·10⁸.

## Completeness of links (2026-11-26)

`links.gp`: the number of ways a subring F fits into K is the number of roots of F's
polynomial in K. Each root reduces to a different root mod a prime of K of norm q, so the
count of roots mod q is an upper bound, and one prime where it equals the listed number
proves the list complete. Tested: 125 quadratic subrings of 97 quartic rings, 48 subrings
of 14 sextic rings, 74 subrings of 9 octic rings: always proved, by a prime at most 83.
Such a prime need not exist in every case (a group can be a union of proper subgroups);
then another proof would be needed.

Not covered: degree 4 and up mining, engine records.
