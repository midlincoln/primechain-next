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

## Money: labeled micro-units; splits are spin-offs (2026-11-23)

Supersedes an earlier draft with separate "deeds" per prime.

- **Every prime is an asset with micro-units:** main-chain primes (2, 3, 5, …)
  and ring primes (prime ideals in rings, e.g. the primes above 13 in
  Q(√17)). Wallets hold concrete primes, as in v1. **All micro-units have
  equal value**; the label records which prime they belong to.
- **A split is a spin-off.** When main-chain prime q splits or ramifies in a
  ring K, new micro-units of K's primes above q are issued (amount set by K's
  policy) and divided by R11: 45% to the entry's miner, 35% upstream, 10% to
  the founder, 10% to validators. The **upstream share goes pro rata to the
  holders of q's micro-units at the moment the entry is finalized**. q itself
  is unchanged; its holders keep their q-units and additionally receive
  units of the new primes.
- **Conjugates are distinct assets.** When q splits into x and y (different
  prime ideals), they are two assets, each receiving half of the split's
  issuance. There is still one entry, one proof and one miner reward per
  split (R8). They must be distinguishable because in larger rings they can
  have different futures. Which one is x is fixed by the canonical-generator
  rule.
- **Nesting:** when a ring prime splits further in a larger ring, its holders
  receive the spin-off, and the upstream share is divided along the path down
  to the main-chain prime (R11). Assets form a family tree that mirrors the
  factorization.
- **Ledger:** all money and ownership live on the main chain's ledger
  (committed by `finance_root`). Ring chains hold only the mathematical
  evidence. Pro-rata payouts use a per-unit accumulator, so the cost does not
  depend on the number of holders.
- **Supply invariant,** checked every block: the change in total supply
  equals everything issued in that block (main reward plus ring payouts
  credited). Fees move money and never create it.
- **Saving:** holders of fertile primes (ones that split in many rings)
  accumulate more units over time.

## Instruments

| Instrument | What it is |
|---|---|
| Validator ordinary shares | ownership of a validator, with votes on the validator as an institution |
| **Prime holdings** | micro-units of a prime; they receive spin-off units whenever it splits or ramifies in a ring |
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
