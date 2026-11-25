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

Not covered: mining (finding generators), large discriminants, engine records.
