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

- **Validators** (regional Reserve Banks): run consensus; are owned by
  their member banks through **membership stock** (see "Banks" below); run
  the discount window.
- **Banks** (member banks): any miner becomes a bank **automatically** by
  meeting on-chain criteria (subscribing to membership stock of one
  validator, required reserves, delinquency below a limit). Nobody approves
  it. Losing the criteria suspends new lending and pledging.
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
  policy) and divided by R11: 50% to the entry's miner, 35% upstream, 15% to
  the founder, no validator share. The **upstream share goes pro rata to the
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
| Validator membership stock | Fed-style: bought at par by member banks (6% of capital, half paid in), not transferable, capped dividend, elects some of the validator's directors |
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

## Loans (2026-11-23)

These are transaction types (versioned, D12) on the main ledger. Their state
is committed by `finance_root`. None is needed for the invited testnet.

1. **Simple micro-unit loan (the main type):** borrow an amount of micro-units
   in any labels; repay the amount plus interest by block H, in any labels.
   The interest must at least cover the lender's forgone spin-offs (the
   expected spin-off yield per block of the units lent): opportunity cost.
2. **Pledged loan (discount-window model):** the borrower pledges specific
   units as collateral. They stay in the borrower's wallet, locked by a lien,
   and **the owner keeps their spin-offs**, as the Fed leaves coupons with the
   pledging bank. Spin-offs on pledged units are added to the collateral. On
   default after H, the lender claims the pledged units and their locked
   spin-offs.
3. **Same-label loan (optional, later):** units of prime p must be repaid as
   units of p, like securities lending.

Transactions: TRANSFER, PLEDGE (signed by both parties), REPAY, CLAIM (valid
only after H). Automatic rules: spin-off credit, locked spin-offs on pledged
units, no transfer of locked units, and the supply invariant.

**Labels are nearly equal in expectation.** For a fixed prime p, p splits in
about half of all quadratic rings (the Kronecker symbol (D/p) is +1 for about
half of discriminants), and every open ring records all primes up to F. So
every prime has roughly the same expected spin-off yield; only realized
outcomes differ. That justifies "all micro-units equal" economically, and
simple loans with repayment in any labels are fair in expectation.

**Good and bad money.** If some primes become clearly more fertile, people
will spend barren units and hoard fertile ones (Gresham's law). Whoever
predicts fertility well profits, and that is intended: it rewards insight.
Note that ring **founders choose D**, and so partly decide which primes split
in their rings (p splits iff (D/p) = +1). A founder holding certain primes
can favour them, although the spin-off is shared by all holders of those
primes. This should be watched, but it is not necessarily harmful. Tools if
needed: same-label loans, and market rates per label.

## Cash (exploratory, 2026-11-23; still being thought through)

**Decided (2026-11-24):** cash cryptography uses the class-group action on
supersingular curves (isogeny-based blind signatures in the CSI-FiSh
family), built strictly from published constructions. Cash is printed only
once the evidence layer has certified the class group it needs. Other cash
details stay open.

The model follows real currency.

- **Validators print, banks distribute.** Banks buy notes from their
  validator with reserves (labeled units), wholesale. Customers withdraw and
  deposit cash only at banks. Validators never deal with customers.
- **Notes:** unlabeled, fixed denominations, **100% backed** by labeled units
  in the issuing validator's reserve. They earn no spin-offs. The backing
  keeps earning spin-offs, and that seigniorage goes to the validator
  (dividends) and to the protocol treasury (evidence bounties).
- **Privacy, Chaum-style:** notes are signed **blind**, so neither the bank
  nor the validator can recognize a note later. The blind-signing key is a
  secret walk on supersingular curves (an isogeny-based blind signature,
  building on CSI-FiSh, which needs the certified class group of Z[√−p]).
  **Cash can only be printed once the evidence layer has certified that
  class group.** Chaum's original 1982 ecash used RSA blind signatures, which
  are not post-quantum; the isogeny-based version is recent research.
- **Payments are off chain:** notes pass directly between wallets (QR code,
  message, NFC). On chain: issuance totals; withdrawals as account debits;
  and, at deposit or swap, the notes' serial numbers recorded as spent, so a
  note cannot be spent twice. An offline variant (Chaum–Fiat–Naor) reveals
  only someone who spends the same note twice.
- **Notes live in wallets,** like physical cash: losing an unbacked-up wallet
  loses its notes.
- **Note series:** old series must be exchanged by some block height, so
  spent-serial records can be pruned.
- **Legal tender (principle):** (1) any valid note can be **redeemed on chain
  directly** against the issuing validator's backing, so redemption cannot
  be blocked; (2) accepting cash deposits from anyone, for notes of any
  validator, is a **condition of bank status**, and refusals can be reported
  on chain.
- **Measures, at every block:** notes in validator vaults, in bank vaults,
  and with the public (M0); backing (= M0); deposits; M1 = cash + deposits.

### Note design ideas (2026-11-24; display and metadata, not protocol)

- **Mint marks:** each note shows the seal of the validator that signed it,
  as US notes show their Federal Reserve district. The signature already
  identifies the validator.
- **Series named after ring cohorts:** for example "the Q(√17) series",
  honoring the ring's founder, with a motif from the ring (13 = (3+2i)(3−2i),
  fundamental units, …). Series must be retired eventually anyway, to prune
  spent serials.
- **Portraits and motifs by denomination,** each mathematician with the
  mathematics the chain uses: 1 Euclid (infinitely many primes), 2 Fermat
  (little theorem, the heart of Pratt certificates), 5 Euler (Euler product),
  10 Gauss (the Gaussian integers and their primes), 20 Dirichlet, 50 Riemann
  (zeta and its zeros), 100 Dedekind (ideal factorization), 500 Minkowski
  (lattices; the PID certificates), 1,000 Noether or Ramanujan, and a special
  Deuring note (supersingular curves, the privacy engine). Living
  mathematicians only with their permission; otherwise honor the theorem
  (e.g. Fermat's Last Theorem, 1995). Use public-domain portraits or
  original drawings.
- **Per-note picture of the class-group action:** the background is the
  isogeny graph of the series prime (supersingular curves as nodes,
  ℓ-isogenies for ℓ = 3, 5, 7, … as colored edges; for p = 419 the whole
  graph fits). A walk derived from the note's own random serial is
  highlighted, so every note is unique and literally pictures the
  mathematics that blind-signed it, without revealing anything (the serial
  is random and hidden from the issuer).
- Optional: prime denominations (2, 3, 5, 7, 11, …) as branding; collectible
  notes (prime serials, famous series) will probably arise on their own; truly
  physical cash (paper QR notes with online swap, or secure-hardware cards)
  is a separate project.

## Banks: membership and evolution (2026-11-24)

Decided: **mimic the Federal Reserve as closely as possible.**

### Membership stock

A member bank of the Fed must buy stock in its regional Reserve Bank. We copy
the rules:

| Fed rule | PrimeChain |
|---|---|
| Subscription equal to **6% of the member bank's capital and surplus** | 6% of the bank's **capital** (units it owns itself, excluding depositors' fund shares); the percentage is a governance parameter, starting at 6% |
| **Half paid in, half callable** by the Reserve Bank | 3% paid in units on registration; 3% callable by quorum governance with notice; not paying a call suspends membership |
| Par value, **cannot be sold, transferred or pledged** | fixed par in units; no share market; returned at par when the bank leaves or loses membership |
| Subscription **adjusted when capital changes** (15% threshold) | recomputed from chain data at epoch boundaries; the bank tops up or is refunded when its capital moves by 15% or more |
| **Capped dividend:** 6% a year; large banks get the lesser of 6% and the 10-year Treasury yield | capped dividend per 1,000 blocks from the validator's income; large banks get the lesser of the cap and the **base rate** (expected spin-off yield, our Treasury analogue). Excess stays with the validator / protocol treasury |
| Member banks **elect directors** (Class A: bankers, Class B: public); Class C appointed by the Board of Governors | member banks of a validator elect part of its board; the rest is appointed by the validators jointly (our Board of Governors). The board chooses the operator; consensus keys stay with the operator (D10) |
| Stock gives **no control over monetary policy** | stock never adds consensus weight; policy levers (κ, y, z, rates) are set by validator governance |
| Membership gives access to the **discount window and cash services** | access to the validator's loans (pledged primes as collateral) and the right to run a cash desk |

Each bank is a member of **one validator** (the Fed's district), chosen at
registration. Paid-in capital is the validator's **reserve**, which it lends
back to member banks through the discount window.

### Stages

1. **Stage 0, at launch: bank = bond portfolio.** Assets: prime and ring-prime
   holdings plus membership stock. Income: spin-offs (protocol-created
   interest) and dividends. No deposits, no loans.
2. **Stage 1: deposits as on-chain fund shares.** Depositors buy a pro-rata
   share of the bank's portfolio and receive their fraction of its spin-offs,
   computed by the protocol (per-unit accumulator). Purpose: diversify
   spin-off luck across thousands of primes.
3. **Stage 2: lending from deposits (full reserve)**, together with
   **fixed-rate deposits**, since both put the bank at risk; the chain checks
   reserves against promises.
4. **Stage 3, only if demand justifies it:** fractional-reserve deposit
   tokens with protocol-enforced reserve ratios; validators as lenders of
   last resort.

Note: in PrimeChain **money is created by the protocol** (spin-offs, steered by
κ and y), not by bank lending. Banks start as holders of interest-bearing
money; credit creation is a deliberate, later step.

### Infrastructure: the chain is the bank's back office

Money rules (membership, deposits, fund shares, loans, reserves, dividends)
run on chain, so depositors need not trust a bank's database. A bank's own
software stays thin: the reference client's single executable gets a
`bank` mode (full node, ~30 MB RAM; hot operational key plus an offline cold
treasury key, as for validators; customer API; later the cash desk). Stages
0–1 need only a wallet; the cash desk is the first piece that needs a
server.

## Validator income without an issuance share (2026-11-24)

**Decided:** the 10% validator share of block and ring rewards (D8, R11) is
dropped. As at the Fed, validators earn from services and assets, so they
earn only when the economy grows:

1. **Seigniorage:** banks buy cash notes with prime units; the validator holds
   those units, which earn spin-offs.
2. **Interest** on discount-window loans to member banks.
3. **Fees:** transactions, ring registrations, cash services.
4. **Spin-offs on paid-in capital** (membership stock is held as primes).

At launch validator income is close to zero (the founder runs all validators).

**Decided (2026-11-24): the freed 10% goes back to the miners.** The main chain
splits 50/50 between the prime finder and the composites (D8). The matching
ring split is 50/35/15 (entry's miner / upstream / founder), the same for split
and ramified primes (R11). Not chosen: lower issuance, a rule-based bounty treasury,
a coupon to all prime holders.

## Shares and BFT independence

Membership stock lets banks influence validators, but consensus must stay
independent:

- one validator, one consensus vote, always; shares never add consensus
  weight;
- concentration limits: no holder above X% of any validator, and limits on
  cross-ownership (nobody controls more than one validator);
- the board (partly elected by member banks) chooses a validator's operator,
  but consensus keys stay with the operator (D10). A change of control is a key rotation with notice.

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

- **Prime bond:** 50% of reward(p) up front, then 35% of one entry in each
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
