# Money supply vs rings accepted (exploratory, 2026-11-26)

Simple yearly models, Python, no prices. Unit: one year of main-chain issuance, constant
speed, no halvings.

- `per_ring.py`: rule A4 (agreed): parameter 17 is the value v of each new ring prime.
  Year-11 supply growth and the yield to holders of main-chain primes (35% of the 10%
  backlog share of each new ring), for v = 1,000,000 / 100,000 / 10,000 and 10 or 20
  rings a year. Result in `docs/protocol-parameters.md`.
- `year11.py`: the same question under the replaced rule A3 (one budget shared by all
  rings). Growth does not depend on the ring count. Its yield column counts the whole
  backlog; holders get 35% of it.
- `fed.py`: first look over 20 years, A3 against a per-ring budget, including rings needed
  to hold 10% growth (an assumption later dropped).
