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
4. **Ring terms are fixed when the ring opens** (R6): founder share and schedule
   parameters. A vote changes them for later rings only. The budget and the number of
   open rings are common to all rings and can change.
5. **Hard bounds in code** for every parameter (proposed, values to be set), so that a
   vote cannot, for example, set a message limit to 0 or a reward to 2^100. Changing a
   bound is a protocol upgrade.
6. Shares of one reward always sum to 100%, so the last one is not a parameter.

## Not parameters

Fixed by the mathematics or by a protocol upgrade: the rules themselves, what counts as a
proof, 2 and 3 as genesis, number types and encodings, signature and hash algorithms,
the quorum ceil(2n/3), one vote per validator, 100% backing of cash. Nothing already
issued can be changed by a vote.

## Table (starting values)

| Parameter | Start | Where |
|---|---|---|
| main reward per prime | 1,000,000 | D8 |
| finder / composites | 50 / 50% | D8 |
| composites per range | 1,024 | D4 |
| transfer fee minimum; outputs per transfer | 1; 1,000 | D16 |
| epoch length (fee pool payout) | 100 blocks | D17 |
| governance notice | 100 blocks | A3-2 |
| proposer skip after | 3 missed turns | A3-3 |
| round timeout base / cap | 1 s / 60 s | D11 |
| certificate checkpoint / window | 1,000 / 1,000 blocks | A3-3 |
| ring miner / owners / founder | 50 / 35 / 15% | R11 |
| kept by holders on the way down | 1/2 | R11 |
| **ring budget per main-chain prime** | 1,000,000 (as the main reward) | below |
| weight of ring j in the budget | 1/sqrt(j) | below |
| share paid for primes older than the ring | 10% | below |
| new rings per epoch (kappa), gate 1 | open, no limit yet | R2 |
| reveal window | 10 blocks | R5 |
| ring deposit | to be set | R5 |
| eligibility bound | abs(D) <= F^2 | R3 |
| certificate size | 256 KB | R4 |
| paid rows / inert rows per segment | 32 / 1,024 | R7 |
| segments per signature | 64 | R7 |
| ring batch size | 256 KB | ring-records |
| cash: total limit, limit per kind, key size | 0, 0, 3072 bits | monetary-layer |

## Ring issuance (starting rule A3, 2026-11-25)

Each main-chain prime q has a **ring budget equal to its main reward**. The entry of ring
j for q issues

  budget · min(1, 2 w_j / (sum of w over the rings open when q was mined)), w_j = 1/sqrt(j).

About half of the open quadratic rings pay for a given prime, hence the factor 2; R12 adds
its degree factor later. Over all rings, ring money stays close to main-chain money (1.0
to 1.1 times in simulation with all rows recorded). The result depends only on numbers
on chain, not on who records first. Primes mined before the ring opened ("backlog") pay
the reduced share; which rings count for them is open. The rule has a type and a version
(R6), so another rule can replace it for later rings.

Exact integer arithmetic for w_j is to be fixed with the implementation.

The ring count and the limit on new rings matter most: in simulation, 300 new rings cost
40 older ones two thirds of their later income, under this rule.
