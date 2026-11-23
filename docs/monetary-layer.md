# Monetary layer (design note)

Status: exploratory design, 2026-11-23. Not part of the invited-testnet
scope. Formats are reserved now (`finance_root` in D9; payee and liens in
R6) so the layer can be added later as versioned transaction types without a
fork.

## Motivation

The institutional model behind the validators follows the U.S. Federal
Reserve System. Miners are the banks, and validators are the regional Reserve
Banks: arithmetic decides what is valid; the institution decides who gets
credit under a common rule, and is paid for running the settlement service.
This note extends that analogy to capital, lending and monetary policy.

## Structure: three tiers and a ladder

```
miner ──(buys validator shares + holds reserves)──► bank ──(enough capital + meets criteria)──► validator
  │                                                  │                                            │
  pledges bonds to a bank, as a customer             pledges bonds to its validator,              consensus,
                                                     lends to customers                           discount window
```

- **Validators** (regional Reserve Banks): run consensus; are owned through
  **ordinary (voting) shares**; run the discount window.
- **Banks** (member banks): any miner becomes a bank **automatically** by
  meeting on-chain criteria (holding at least S validator shares, required
  reserves, delinquency below a limit). Nobody approves it. Losing the
  criteria suspends new lending and pledging.
- **Customers:** ordinary users who borrow from banks.
- **Path to BFT:** banks are the pool of new, independent validators. Bank →
  validator requires capital and criteria and is activated by quorum with
  notice (it changes consensus). This replaces v1's admission by "mining
  enough for the reserve". Four or more independent validators give real
  Byzantine tolerance (D13).

## Money and bonds are separate (2026-11-23)

- **Money:** all micro-units stay **equal in value** and fungible, as in v2.
  Payments need no prices, and lending against cash stays free of price
  oracles.
- **Prime bonds:** each main-chain prime has a **bond record**, the twin of
  the ring record (R6): **finder** (permanent; taken from the block's prime
  submission), **payee/owner** (the current holder; transferable by a signed
  transfer recorded on chain), and optional **liens**. The prime's ring
  income (R11 upstream share) is paid to **the owner of record at the moment
  the ring entry for p is finalized** (a snapshot; earlier payments stay with
  earlier owners, and there are no later claims). Anyone can trace the owner
  on chain.
- **Saving:** buying a prime means buying its bond: the right to its future
  ring income. Fertile primes (ones that split in many rings) are worth more
  as bonds, while the money itself stays one equal unit.

## Instruments

| Instrument | What it is |
|---|---|
| Validator ordinary shares | ownership of a validator, with votes on the validator as an institution |
| **Prime bonds** | income from a main-chain prime: its upstream share each time it splits or ramifies in a ring (R11) |
| **Ring bonds** | income from a ring: the founder share (R6) |
| Loans | on-chain objects: lender, borrower, amount, schedule by block height, collateral or lien |
| Reserves | units a bank keeps locked at its validator |

## Rules enforced by the chain (from chain data)

1. Only **banks** pledge bonds to validators. Others pledge to banks.
2. A loan against a bond is bounded by the bond's observed income (k × income
   over the last N blocks); total liens on one stream are capped.
3. Loans are registered with repayment schedules by block height; payments
   and arrears are visible on chain.
4. A bank's required reserves rise with its customers' arrears (a loan-loss
   reserve).
5. Validator surplus beyond costs and dividends goes to a **protocol
   treasury** that funds evidence bounties.

## Shares and BFT independence

Ordinary shares let miners influence validators, but consensus must stay
independent:

- one validator, one consensus vote, always; shares never add consensus
  weight;
- concentration limits: no holder above X% of any validator, and limits on
  cross-ownership (nobody controls more than one validator);
- shareholders elect a validator's operator, but consensus keys stay with
  the operator (D10). A change of control is a key rotation with notice.

Today all validators are run by the founder. This structure is the path away
from that dependence.

## Time

There are no clocks. **Time is main-chain block height**, one tick per prime,
and the same clock serves all rings (no ring passes the main frontier, R7).
Loan terms, schedules, arrears and dividends are in blocks. Rates are
expressed per block (or per 1,000 blocks), with fixed integer rounding.
Issuance per block is known exactly, so real rates are computable on chain. A
median validator timestamp may be shown for human-facing displays only.
Example: 1,000,000 micro-units at 1 basis point per 1,000 blocks, due at
H + 50,000, accrues 5,000 micro-units of interest.

## Bond values and policy levers

- **Prime bond:** 45% of reward(p) up front, then 35% of one entry in each
  ring where p splits or ramifies (at most one per ring). With K rings open, a
  prime earns at most K payments, typically about K/2. Its income depends on
  future ring openings.
- **Ring bond:** nothing up front (a discovery reward is optional), then the
  founder share of each entry, from about half of all future main-chain
  primes. Its income depends on the main chain's pace.
- Long-run, found rings earn more than found primes. This is intended:
  discovering a ring is harder and creates a new productive chain.

Validator levers, all voted with notice and **fixed at issuance** for the
rings they apply to:

| Lever | Sets |
|---|---|
| κ | how many rings may open per epoch (gate 1 as a quota) |
| y | the founder share for newly opened rings |
| z, decay | how much rings may issue |

**No rate vote:** prime-bond yields follow from κ, and lending rates follow
from bond yields. Unlike conventional monetary expansion, a higher κ
*raises* prime-bond income. This needs to be understood before κ is set.

## Open: controlling issuance from rings

Without control, issuance per block ≈ reward(p) + ½·R·K, so it grows with
the number of open rings K. Candidates:

| Option | Mechanism |
|---|---|
| A | budget per epoch shared by all rings (Kaspa-like) |
| B | lifetime budget per cohort of rings opened by one decision |
| C | decay per ring: geometric (finite total), hyperbolic, or halving per N entries |
| D | decay across rings: later rings start with lower rewards |

These can be combined (for example C plus an A-style ceiling). Goals:

1. monetary stability (bounded, predictable issuance);
2. profitable mining (main and rings);
3. workable lending (predictable bond income);
4. balance between main mining, ring discovery and ring mining.

**Method:** a small, exploratory economic simulation of options A–D under
scenarios (slow or fast mining, few or many rings, varying κ), measuring
inflation paths, miners' income, and bond values and yields. It will be
built only after the founder's go-ahead.

**Decided (2026-11-23): one unit of account.** All micro-units are equal,
whether issued by the main chain or by any ring. Ring issuance is the same
money, so budgets across rings (options A–D) are well defined.
