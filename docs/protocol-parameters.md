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
   Exception (proposed): the ring budget counts as of the block in which the row's
   main-chain prime was mined, so that nobody gains by timing a row around a vote.
4. **Ring terms are fixed when the ring opens** (R6): founder share and schedule
   parameters. A vote changes them for later rings only. The budget and the number of
   open rings are common to all rings and can change.
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
| 17 | **ring budget per main-chain prime (micro-units)** | 1,000,000 | R6 |
| 18 | exponent e of the weight 1/j^e of ring j | 1/2 | R6 |
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
| 37 | total cash limit | 0 | monetary-layer |
| 38 | limit per kind (tranche) | 0 | monetary-layer |
| 39 | denominations | to be set | monetary-layer |
| 40 | RSA key size, at least | 3,072 bits | monetary-layer |
| 41 | note exchange deadline; freeze deadline | to be set | monetary-layer |
| 42 | swap fee | to be set | monetary-layer |
| | **Later (banks, loans)** | | |
| 43 | membership stock, part paid in, dividend cap, loan limits | 6%, half, to be set | monetary-layer |

The ring budget (17) is a number of its own: it starts equal to the main reward (1) and
does not follow it when either is changed. A value of 0 stops ring issuance for new primes.

Local settings (snapshot interval, resend times, awards file, sync sizes) do not affect
what is valid, so each node chooses its own. A lower limit on sizes or key strength is
bounded in code (rule 5).

## Ring issuance (starting rule A3, 2026-11-25)

Each main-chain prime q has a **ring budget** (parameter 17). The entry of ring
j for q issues

  budget · min(1, 2 w_j / (sum of w over the rings open when q was mined)), w_j = 1/j^e.

About half of the open quadratic rings pay for a given prime, hence the factor 2; R12 adds
its degree factor later. Over all rings, ring money stays close to main-chain money (1.0
to 1.1 times in simulation with all rows recorded). The result depends only on numbers
on chain, not on who records first. Primes mined before the ring opened ("backlog") pay
the reduced share; which rings count for them is open. The rule has a type and a version
(R6), so another rule can replace it for later rings.

Exact integer arithmetic for w_j is to be fixed with the implementation.

The ring count and the limit on new rings matter most: in simulation, 300 new rings cost
40 older ones two thirds of their later income, under this rule.
