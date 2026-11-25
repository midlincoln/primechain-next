# Nested rings: reward flow simulation (2026-11-25)

Tower Q < Q(sqrt 2) < Q(zeta_16)+ < Q(zeta_32)+ (degrees 2, 4, 8), main-chain
primes to 60,000, four miners with 40/30/20/10% of the power; A, B and C founded
the three rings. Experimental Python, not engine code.

- `gen_tower.gp`: PARI generators (`gp -q -s 100M gen_tower.gp > tower_generators.txt`)
- `simulate.py`: verifies all generators, then compares paying at once with
  claiming later, under random sales, owners and claims (5 runs)
- `lookup.py`: awards computed instead of written (R13), abstract tower, three mistakes
- `spend_test.py`: the same with real rows, fees, restarts, replay and refused spending,
  including a prime sold before and in the block of its split, and four mistakes;
  `spend_test.py scale` measures look-ups (220 MB on disk in SCALE_DIR)
- `checks.py`: four deliberate mistakes (all must be caught) and income tables

Results: 5,276 generators verified; for every prime but 2, one generator gives
all n primes through the automorphisms. Both methods agree on all balances
(about 155,000 per run); held + rounding loss = issued; loss 0.0005%. The three
rings issue 0.87 million per main-chain prime (1/2 + 1/4 + 1/8).

Counting only claimed units would favor ring miners and founders: D's level-1
income fell 6% with claims every 25 blocks and 15% with one claim at the end.
So all owned units earn (R13).

Computed awards (R13 as revised): 5 runs of `spend_test.py`, 8,476 transfers accepted
and 11,384 refused alike by three nodes and by the pay-at-once ledger; 105,341 balances
equal; all mistakes caught. Signatures, consensus and the engine are not covered.

Not covered: several parent rings, transfer fees, ring opening and consensus.
The eligibility |D| <= F^2 is assumed for degrees 4 and 8 (open in R3).
