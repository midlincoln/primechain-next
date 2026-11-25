# Cash cycle experiments (2026-11-25)

Test code for `docs/monetary-layer.md`, on the three-ring tower of
`../nested_rings`. Accounting and protocol only, **no security**: RSA is 768 bits
with a trusted dealer, the class group has 27 elements (p = 419), and one-time
note keys are shortened. Not engine code.

- `cash.py`, `cycle.py rsa|419 [limit]`: print, sell, resell, swap, exchange back
- `attacks.py` (RSA), `attacks419.py` (class group): double spending, forgery,
  theft in transit, dishonest validators, counterfeiting bounded by the limit
- `redemption.py`: which units leave the vault (why "fewest pending first")
- `isogeny.py`: the class-group action by Velu isogenies, the CSI-Otter blind
  signature (256 repetitions) and the overlapping-session forgery (M&M'S, 2023)
- `two_validators.py`: why separate blind signatures from any two validators
  fail, and one key per pair works

In every run backing = cash in circulation and no units were created or lost.
The recorded RSA run used oldest-first redemption; the 419 run used fewest pending
first and a limit of 1,000,000.
