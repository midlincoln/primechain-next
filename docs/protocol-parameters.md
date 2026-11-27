# Protocol parameters (2026-11-25)

**Decided (D20): every number is a protocol parameter.** It has a starting value and can
be changed by validator vote (A3-2) instead of being settled in the design. Design work
goes into the rules; starting values only need to be reasonable and safe.

## Rules for parameters

1. **All parameters live in one versioned table,** part of the state and of the policy
   digest in every header (D9). Every node computes with the same numbers.
2. **A change takes a quorum vote and notice** (A3-2, R2), and takes effect at a block
   height. Parameters are named in the vote, as old value → new value.
3. **Never retroactive.** A row, reward or fee uses the values in force at its own main
   block h. The table's history is kept, because ring awards are computed later (R13).
   The value of a ring prime (17) is fixed per ring when it opens (rule 4), so nobody
   gains by timing a row around a vote.
4. **Ring terms are fixed when the ring opens** (R6): founder share and schedule
   parameters and the value of a ring prime (17). A vote changes them for later rings
   only.
5. **Every parameter needs protocol approval to change** (agreed 2026-11-25): the same
   vote with notice for all of them, with no exception for small ones.
   **Ranges:** a model built later will suggest a comfortable range for each parameter.
   Hard bounds in code (proposed) will come from it, so that a vote cannot set a message
   limit to 0 or a reward to 2^100. Changing a bound is a protocol upgrade.
6. Shares of one reward always sum to 100%, so the last one is not a parameter.

## Not parameters

Fixed by the mathematics or by a protocol upgrade: the rules themselves, what counts as a
proof, 2 and 3 as genesis, number types and encodings, signature and hash algorithms,
the quorum ceil(2n/3), one vote per validator, 100% backing of cash. Nothing already
issued can be changed by a vote.

## Table (starting values)

"Engine" means the value exists in the engine today; only the minimum fee can be voted on
so far. The rest is designed or planned.

| # | Parameter | Start | Where |
|---|---|---|---|
| | **Main chain** | | |
| 1 | main reward per prime (micro-units) | 1,000,000 | D8, engine |
| 2 | prime finder's share (composites get the rest) | 50% | D8, engine |
| 3 | composites per range, at most | 1,024 | D4, engine |
| 4 | main-chain integer size, at most | 64 bytes | D14, engine |
| | **Transfers** | | |
| 5 | minimum fee (micro-units) | 1 | D16, engine, votable |
| 6 | outputs per transfer, at most | 1,000 | D16, engine |
| 7 | inputs per transfer, at most | to be set | A3.2 |
| 8 | transfers per block and waiting, at most | to be set | A3.2 |
| 9 | fee by size | to be set | A3.2 |
| | **Validators and consensus** | | |
| 10 | epoch length (fee pool payout) | 100 blocks | D17, engine |
| 11 | notice for voted changes | 100 blocks | A3-2, engine |
| 12 | proposer skipped after missed turns | 3 | A3-3, engine |
| 13 | round timeout, base and cap | 1 s, 60 s | D11, engine |
| 14 | certificate checkpoint every / recent window | 1,000 / 1,000 blocks | A3-3, engine |
| 15 | message size and pending submissions per miner | to be set | A3.2 |
| 16 | validator minimum reserve | to be set | A3-2 |
| | **Ring rewards** | | |
| 17 | **value of a new ring prime (micro-units)** | 100,000 | R6, rule A4 |
| 18 | exponent x of the weight 1/j^x of ring j (rule A3 only, unused under A4) | 1/2 | R6 |
| 19 | share paid for primes older than the ring | 10% | R6 |
| 20 | ring miner's share (gets what 21 and 22 leave) | 50% | R11 |
| 21 | upstream share (owners of the primes below) | 35% | R11 |
| 22 | founder's share | 15% | R11 |
| 23 | kept by holders on the way down | 1/2 | R11 |
| | **Opening rings** | | |
| 24 | gate 1 (new rings) | open | R2 |
| 25 | new rings per epoch, at most (kappa) | to be set | R2 |
| 26 | reveal window | 10 blocks | R5 |
| 27 | ring deposit | to be set | R5 |
| 28 | eligibility: abs(D) <= F^t | t = 2 | R3 |
| 29 | certificate size, at most | 256 KB | R4 |
| 30 | certificate integer size, at most | 4,096 bytes | D14, engine |
| | **Mining rings** | | |
| 31 | paid rows per segment, at most | 32 | R7 |
| 32 | inert rows before a paid one, at most | 1,024 | R7 |
| 33 | segments per signature, at most | 64 | R7 |
| 34 | mining message size, at most | 256 KB | ring-records |
| 35 | ring batch size, at most | 256 KB | ring-records |
| 36 | pause of one ring / all rings | off | R2 |
| | **Cash (closed at launch)** | | |
| 37 | total cash limit | 0 (at most 5% of supply when cash opens, proposed) | monetary-layer |
| 38 | limit per kind (tranche) | 0 (at most 1% of supply when cash opens, proposed) | monetary-layer |
| 39 | denominations | 1,000 / 10,000 / 100,000 / 1,000,000 micro-units | monetary-layer |
| 40 | RSA key size, at least | 3,072 bits | monetary-layer |
| 41 | note exchange deadline; freeze deadline | to be set | monetary-layer |
| 42 | swap fee (paid separately in units) | to be set | monetary-layer |
| | **Later (banks, loans)** | | |
| 43 | membership stock, part paid in, dividend cap, loan limits | 6%, half, to be set | monetary-layer |
| | **Banks and the Fed (2026-11-27)** | | |
| 44 | **reserve ratio on demand deposits (ρ), a policy tool** | open (100% was proposed) | monetary-layer |
| 45 | reserve ratio on term deposits (lendable until their date) | open | monetary-layer |
| 46 | grace period to move a deposit's reserve to the Fed | to be set | monetary-layer |
| 47 | share of ring income on reserves paid to the bank (interest on reserves) | 100% | monetary-layer |
| 48 | proof interval for banks (fingerprint and total of customer balances), in blocks | 10,000 (to tune on the testnet) | monetary-layer |
| 49 | Fed lending: discount rate (per 1,000 blocks), haircut on ring bonds and notes | to be set | monetary-layer |
| 50 | blocks a bank has to pay a withdrawal claim before the Fed pays and the bank is frozen | to be set | monetary-layer |
| 51 | blocks a frozen bank has to fix the cause before wind-down | to be set | monetary-layer |
| 52 | withdrawal claim deposit | to be set | monetary-layer |
| 53 | minimum withdrawal claim | to be set | monetary-layer |
| 54 | cash freeze: presentation window (blocks) | to be set | monetary-layer |
| 55 | cash redemption speed limit per kind | 5% of the kind per 1,000 blocks (proposed) | monetary-layer |
| 56 | quorum for an emergency freeze | to be set | monetary-layer |
| | **Prudential rules for banks (2026-11-27)** | | |
| 57 | **capital ratio k** (capital to risk-weighted assets), a policy tool | open | monetary-layer |
| 58 | risk weights by kind of asset | open (0 / 20 / 50 / 100 / 150% proposed) | monetary-layer |
| 59 | leverage ratio ℓ | open | monetary-layer |
| 60 | liquidity coverage: horizon N (blocks), run-off share of demand deposits | open | monetary-layer |
| 61 | stable funding limit | open | monetary-layer |
| 62 | concentration limit c (per issuer address, per ring) | open | monetary-layer |
| 63 | value of overdue notes over time | open | monetary-layer |
| 64 | steps of enforcement (thresholds for limits, freeze, wind-down) | open | monetary-layer |

The value of a ring prime (17) is a number of its own and does not follow the main
reward (1) when either is changed. A value of 0 stops ring issuance for new primes.

Local settings (snapshot interval, resend times, awards file, sync sizes) do not affect
what is valid, so each node chooses its own. A lower limit on sizes or key strength is
bounded in code (rule 5).

## Ring issuance (starting rule A4, agreed 2026-11-26)

**Parameter 17 is the value of each new ring prime,** not a budget shared by all rings.
The entry of a ring for main-chain prime q issues

  for each of its primes of norm q:  v · e,

where v is parameter 17 (starting value 100,000 micro-units, a tenth of the main reward)
and e is the prime's ramification index. By R12 a ring has on average one prime of norm
q per row (counted with e), so every ring earns about v per main-chain prime, in every
degree. Primes mined before the ring opened ("backlog") pay the share in parameter 19
(10%). Each payment is split by R11: ring miner 50%, holders of the main-chain prime 35%,
founder 15%. v is fixed for each ring when it opens (rule 4), so a vote on v changes
only later rings.

**Every accepted ring adds money, so the number of rings the validators accept is the
monetary lever** (the "algebraic Fed"), together with v. The limit on new rings (κ,
parameter 25) bounds it. Model, year 11 at constant speed, rings accepted every year
from year 1 (`experiments/fed_rings/per_ring.py`; unit: one year of main issuance):

| v (micro-units) | rings per year | supply growth, year 11 | yield to holders of main primes | supply, year 11 |
|---|---|---|---|---|
| 1,000,000 | 10 | 20% | 35% | 726 |
| **100,000** | **10** | **19%** | **3.5%** | **83** |
| 100,000 | 20 | 19% | 7% | 154 |
| 10,000 | 10 | 14% | 0.3% | 18 |

The yield comes from the backlog: each new ring pays 10% of v for every older main-chain
prime, and its holders get 35% of that, so

  yearly yield ≈ 35% · 10% · (v / main reward) · (rings accepted per year).

With main-chain issuance alone, growth in year 11 is 10% and there is no yield. Which
rings count for the backlog is still open.

### Previous rule A3 (2026-11-25, replaced)

Each main-chain prime q has a **ring budget** (parameter 17). The entry of ring
j for q issues

  for each of its primes of norm q:  budget · e · w_j / W,

where e is the prime's ramification index, w_j = 1/j^x (parameter 18 is x), and W is the
sum of w over the rings open when q was mined. An entry never issues more than the whole
budget; if it would, its primes are reduced in proportion. **The same rule for every
degree (agreed 2026-11-25):** by R12 a ring has on average one prime of norm q per row
(counted with e), so rings of all degrees earn alike. Over all rings, ring money stays close to main-chain money (1.0
to 1.1 times in simulation with all rows recorded). The result depends only on numbers
on chain, not on who records first. Primes mined before the ring opened ("backlog") pay
the reduced share; which rings count for them is open. The rule has a type and a version
(R6), so another rule can replace it for later rings.

Exact integer arithmetic for w_j is to be fixed with the implementation.

The ring count and the limit on new rings matter most: in simulation, 300 new rings cost
40 older ones two thirds of their later income, under this rule.
