# Ring issuance simulation (2026-11-25, exploratory)

Options A-D of `docs/monetary-layer.md` on the 351 certified quadratic rings with
|D| <= 2209 (real splitting), 10,000 main blocks, miners taking the best-paid rows
first. Amounts in main rewards R. Python, not engine code; no prices or costs.

`issuance.py fast | slow | attack [rule,rule,...]` (about 4 minutes of CPU for `fast`).

| Rule | ring / main money, 12 rings | 351 rings | 12, then 300 at once: worst 100 blocks | 300 rings by one user: their share of all money | honest later income kept |
|---|---|---|---|---|---|
| flat | 5.98 | 174.93 | 999 | 57% | 100% |
| A budget per block | 0.95 | 1.00 | 1.0 | 15% | 10% |
| A2 budget per prime, equal | 1.03 | 1.09 | 2.3 | 15% | 12% |
| A3 budget per prime, 1/sqrt(j) | 1.00 | 1.05 | 1.7 | 12% | 32% |
| B 5000 per cohort of 50 | 0.50 | 3.82 | 283 | 43% | 94% |
| C1 geometric per ring | 2.20 | 64.41 | 470 | 56% | 100% |
| D1 1/j | 1.55 | 3.21 | 50 | 16% | 100% |
| D3 1/(j(j+1)) | 0.46 | 0.50 | 3 | 0.5% | 100% |

A2/A3: the row (ring j, prime q) pays 2 w_j / (sum of w over the rings open when q was
mined), at most 1 (about half the rings pay for a given prime). Primes older than the ring
("backlog") pay 10% of that, with the rings open at recording time. Both the 10% and the
moment used for backlog are arbitrary. In the abuse test the user also mines all their
rings (65%).

Not covered: prices, mining costs, voting on new rings, higher degrees, nested rings.

`a4_rows.py` (2026-11-28, B3.5): rule A4 row by row (v per ring prime times e, the backlog share
of parameter 19 for primes mined before the ring opened, the R11 split with holders at the end of
the block before), on 8 quadratic rings over 220 main blocks with competing works. Writes
`a4_vectors.txt`, which `next_ring_mine_issuance` and `next_ring_mine_persistence` replay through the
engine; every award is equal to the last unit.
