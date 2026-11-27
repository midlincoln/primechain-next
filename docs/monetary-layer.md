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
  evidence. Awards from splits are computed from the rows and the ownership
  history and are not written per owner (R13 of `rings-design.md`).
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

## Lending: rings are land (2026-11-27)

Supersedes the collateral part of "Loans" above: plain primes are money, so they are not
the main collateral.

**Why not primes.** A unit of any prime is money worth one micro-unit and can be spent
directly. Pledging primes to borrow micro-units only makes sense when a prime trades above
face value; lending up to face value against them carries no risk and serves no purpose.
Loans are made and repaid in micro-units of any labels; nobody expects the same prime
back, so same-label loans are dropped.

**Rings are land.** A ring bond (the founder share of a ring, R6) is not spendable money:
it is an income stream, like a Treasury bond or a field.

| Land | Ring |
|---|---|
| discovered and claimed by a settler | discovered by its founder (commit and reveal) |
| yields a harvest or rent every season, without end | yields ring income with every main-chain prime, without end |
| the landowner collects the rent | the payee collects the founder share (15%) |
| tenant farmers do the work and keep most of the harvest | ring miners record the rows and keep 50% |
| title deed, land registry | the ring record and its payee history |
| sold by transferring the title | sold by `RING_PAYEE` |
| mortgage: the owner keeps farming, the bank forecloses on default | lien: the payee keeps the income, the lender takes the ring on default |
| the government opens new territory | the validators accept new rings (κ, the "algebraic Fed") |
| land value ≈ rent / interest rate | ring bond value ≈ yearly founder income / rates |

Differences that matter: under rule A4 a new ring does not reduce older rings' income (new
land does not shrink old fields), though it adds money; the supply of land is set by
vote, so ring prices partly price Fed policy; the harvest is certain (mathematics), and
the risk lies in the value of money and the speed of the main chain.

Rough value: with v = 100,000 the founder gets 15,000 micro-units per main-chain prime.
If the main chain makes N primes a year, the ring pays 15,000 · N a year, and at a 3.5%
rate it is worth about 29 years of that income.

**Who lends to whom** (rule 1 above, as at the US Fed):

    founder / individual ──ring bond──► bank ──its own ring bonds──► Fed (validators)
                         ◄─micro-units─       ◄──────micro-units─────

- **Individuals and founders borrow from banks** (mortgages on rings). The bank judges
  the ring, sets the haircut and the rate, and runs the loan in its client app.
- **Banks borrow from the Fed** at the discount window, against ring bonds they hold;
  the Fed lends to member banks only. Banks also lend to each other against ring bonds
  (the money market, at a rate just below the Fed's).
- **Base rate:** the holder yield of money (about 3.5% at 10 rings a year, rule A4),
  since a lender gives up that income. Loan rates are the base rate plus risk.
  Accepting more rings raises the yield and so the rates, the reverse of a real Fed,
  because new money reaches holders as income rather than as cheap credit.

**Where the Fed's micro-units come from** (open):

| | A. Fed lends its reserves | B. Fed creates the units |
|---|---|---|
| source | paid-in membership stock of its banks | new micro-units, destroyed on repayment |
| supply | unchanged: money moves | rises while loans are open |
| like | a credit union | the real Fed |
| limit | the size of the reserves | haircuts, rate, a voted cap |

Proposed: **A at the testnet; B modelled next.** B gives the Fed a second lever (rings
set long-term growth, the discount rate sets short-term credit) but changes the supply
rule (loans outstanding count), so it needs a model before a decision.

### Deposits, reserves and the Fed's view (2026-11-27)

**Principle (the founder, 2026-11-27): the protocol sets the rules for banks; banks build
their own software.** As with mining clients, we specify what the chain checks (bank
registration, address roles, the reserve rule, the proof of liabilities, settlement) and
provide a **reference banking setup** (bank mode of the reference client, a sample
customer app) as an example only. Customer apps, online banking, interest schemes and
internal ledgers are each bank's own choice. **On chain, per bank and per proof interval:
one transaction with a 32-byte fingerprint of the customer balances and their total.**
The balances themselves never leave the bank.

**Proposed: a thin bank layer on chain,** changing "banking is client side only"
(2026-11-25) to: deposits, reserves and the loan register are on chain; rates, credit
checks and customer service stay in client apps.

- **Bank addresses by role (2026-11-27, the founder's design).** A bank registers its
  addresses at its validator, each with one role:

  | Role | In | Out | Counts as |
  |---|---|---|---|
  | **deposit** | from customers | to customers | in = deposit, out = withdrawal |
  | **operations** | loan repayments, FX purchases, income | loan payouts, FX sales, costs | bank's own business |
  | **reserve account at the Fed** | from the bank | released by the Fed under the rule below | reserves, held by the Fed |
  | **cash** | units backing notes the bank bought | units returned when notes are redeemed | cash vault (cash is a separate design) |

  Transfers between one bank's own addresses are internal and change no total.
  **Deposit total per bank** = everything customers sent to its deposit addresses minus
  everything sent back, plus interest the bank credits (posted as one total per proof interval,
  not per customer). Inflows to operations addresses from outside must name a loan
  number or an FX trade; untagged inflows count as deposits, so a bank cannot hide
  deposits in operations.
- **Privacy this gives:** the chain sees a customer's deposit and withdrawal (address and
  amount). In between the money is mixed with all the bank's money, and only the bank's
  books say whose it is. The bank collects the ring income on everything it holds and
  pays customers what it promised, which can match or beat the holder yield because the
  bank also lends.
- **Every withdrawal is on chain (the founder, 2026-11-27),** whole or partial, whether
  paid to an address or in cash: a withdrawal to an address is the transfer itself; a
  cash withdrawal is a record by the bank with the amount and the hash of the customer's
  signed instruction. Cash deposits are recorded the same way. The deposit total is
  therefore exact at every block, not only at each proof.
- **Proof of liabilities (agreed 2026-11-27, required):** every proof interval (parameter
  48, counted in blocks; not the 100-block epoch) the bank publishes
  a fingerprint of all customer balances, keyed by customer key, that adds up to a total.
  The chain stores the fingerprint and the total; **the total must equal the chain's
  deposit total**, which closes the fake-withdrawal gap (a "withdrawal" to the bank's own
  address lowers the chain total but not what customers are owed). Each customer's app
  checks only its own entry. The full list stays with the bank and goes to the Fed only
  if the bank fails. A missing proof or a mismatch freezes new deposits and reserve
  releases until fixed.
- **Interbank settlement:** a payment from a customer of bank A to a customer of bank B
  is booked by the two banks and settled once per proof interval, netted, from A's reserve account
  to B's (as through Fedwire). The chain sees only "A → B: net amount", which lowers A's
  deposit total and raises B's.
- **Reserves are held by the Fed (decided 2026-11-27).** Each bank has a **reserve
  account** at its validator: a protocol account with no private key, so no single key
  can be stolen. Only two things move it: a release to the bank's deposit address that
  leaves the rule intact (checked by the chain), or a quorum decision of the validators
  (for example to pay depositors of a failed bank).
  - **Rule:** reserves ≥ ρ · demand deposits + ρ_term · term deposits. **The reserve
    ratio is a policy tool of the Fed** (agreed 2026-11-27; parameters 44 and 45, voted
    like any parameter): 100% on demand deposits and 0% on term deposits to start, so
    banks lend only money promised to stay (maturity matching). Lowering ρ later, once
    the discount window is tested, lets deposits fund more loans.
    A deposit must be matched by the bank's transfer to its reserve account within a
    grace period (a parameter); until then the bank can move nothing else out.
  - **Withdrawals:** the bank pays from its deposit address; if short, it asks for a
    release, which the chain allows when the rule still holds after the withdrawal.
  - **Income on reserves (agreed 2026-11-27):** reserves are primes and earn ring income, which the
    Fed collects. The Fed pays the bank interest on reserves at a voted share of it
    (100% at first) and keeps the rest: the Fed's own interest-on-reserves lever and a
    source of validator income.
  - **Failure:** the Fed already holds the reserves, so it can pay a failed bank's
    depositors directly, from the bank's proof of liabilities.
- **Withdrawal claim and freeze (agreed 2026-11-27).** A customer whose signed withdrawal
  is not paid can file a **claim on chain**: the signed request plus their entry in the
  bank's last proof of liabilities, checked against the stored fingerprint. If the bank
  does not pay within N blocks (parameter 50), the Fed pays the customer from the bank's
  reserve account and **the bank's addresses are frozen.**
  - **What can be claimed:** the claimant's balance in the bank's last proof of
    liabilities (the claim is signed by that entry's key) **plus deposits sent from the
    claimant's address to the bank's deposit addresses since that proof** (visible on
    chain), so a customer who deposited after the last proof is covered too.
  - **Rebuttal by the bank:** withdrawals paid to the claimant since the proof (on chain,
    counted automatically), or the customer's own signed instructions since the proof.
    Every customer instruction (withdraw, pay, transfer) carries a sequence number and the
    balance after it, signed by the customer; the proof of liabilities records each
    entry's sequence number, so a newer signed instruction with a lower balance beats an
    old proof. A rebutted claim is cancelled.
  - **Against spam:** a claim deposit (parameter 52), refunded when the claim is paid and
    given to the bank when it is rebutted; a minimum claim amount (parameter 53); one
    open claim per key.
  - **Freeze triggers:** an unpaid claim past N; a missing or mismatched proof of
    liabilities; reserves below the rule after the grace period.
  - **Frozen means:** nothing leaves the bank's deposit, operations or cash addresses
    except payments to customers (withdrawals and claims); new deposits to a frozen bank
    are invalid, so nobody pays into it by mistake; incoming loan repayments are still
    accepted. The reserve account pays claims only.
  - **Unfreezing:** automatic once the cause is fixed (claims paid, proof posted, reserves
    restored), or by a validator quorum.
  - **Wind-down:** if the cause is not fixed within M blocks (parameter 51), a validator
    quorum winds the bank down: the Fed pays every customer in the last proof of
    liabilities from the reserves, then from the bank's other addresses, and the bank
    loses its status.
  Term deposits that may be lent out until a deadline are an option for later.
- **Loan register:** `LOAN_OPEN`, `LOAN_REPAY`, `LOAN_WRITE_OFF`, filed by the bank and
  acknowledged by the borrower's wallet signature. An off-chain borrower appears only as
  a hash of an identity checked by a licensed attester. Arrears are computed by the chain
  from the schedule and the repayments, and raise the required reserve.
- **The Fed's view is computed, not reported:** every bank's balance sheet (deposits by
  type, reserves, primes, ring bonds, loans, arrears) follows from the chain at every
  block, a live Call Report. Hidden bad loans are impossible for registered loans; an
  unregistered loan makes the bank look poorer. Fake loans to the bank's own addresses
  remain: borrower acknowledgement, attestation, examination and loss of bank status
  answer them, as in the US.
- **Privacy (agreed 2026-11-27):** the Fed sees totals per bank, the depositor sees their
  own balance. **Names are not required by the protocol** (decided 2026-11-27): an account
can be just a key; a bank collects names only if its services or its country require
it (unsecured loans, dollar desks). The
  role addresses above give this without new cryptography; this replaces the earlier
  plan of hidden amounts with proofs. A shielded pool (hidden parties and amounts, with
  ring income paid to the pool's public totals) stays a later option. Cash is separate.

**Two kinds of depositors (agreed 2026-11-27):**

1. **On-chain depositors** (for example a miner) deposit micro-units. The chain sees the
   deposit transfer, then only the bank's own activity. Hiding the entry as well is a job
   for the privacy layer.
2. **Off-chain depositors** bring dollars, bitcoin or USDT; the customer never appears on
   chain. Order:
   - **Testnet:** the bank converts at its **FX desk** (sells micro-units from its own
     stock at its quoted price) and records a micro-unit deposit. No protocol change.
   - **Next:** **bank-issued tokens** (for example "Bank X USD", "Bank X BTC"), backed 1:1:
     a new asset type on the ledger, with proof of backing (BTC from the bank's public
     bitcoin addresses, dollars by attestation). Micro-units and tokens can then be
     exchanged on chain, with banks as market makers.
   - **Later:** direct cross-chain swaps (hash and time locks on both chains).

**Model** (`experiments/lending/deposits.py`): a bank with 600M own units, 400M demand and
600M term deposits, 300M ring bonds; loans at 12% with 5% defaults; the Fed lends at 5%
against ring bonds with a 30% haircut. Return on capital is 13% at full reserve and 15%
at 0%. A run of 80% of demand deposits in five days needs no Fed help at full reserve,
100M at ρ = 50%, 193M at 20%, and exhausts the discount window at 0%. Paying 5% on term
deposits costs about 3 points of return. Off-chain consumer loans and the borrower's
currency risk: `experiments/lending/offchain_loan.py`.

### Liens on rings (proposed)

The one protocol piece lending needs. A ring record already has a payee and a reserved
lien field (R6, `rings-design.md`). Proposed ring-record transactions on the main chain:

| Transaction | Signed by | Effect |
|---|---|---|
| `RING_LIEN` | payee and lender | records lender L, amount X, deadline H and the lien type |
| `RING_LIEN_REPAY` | payee | pays L what is still owed (in the same transaction) and ends the lien |
| `RING_LIEN_CLAIM` | lender | valid only after H with X not repaid: L becomes the payee |

Lien types:

1. **Mortgage (default):** the payee keeps the founder income; the ring is only locked.
2. **Income repays (annuity):** from the next block the founder income goes to L until X
   has reached L, then back to the payee automatically (computed from the rows, R13).

Rules: while a lien is active the payee cannot change without the lender's signature, so a
pledged ring cannot be sold; one lien per ring at first. Rates, schedules, haircuts and
loan books stay in client apps; the chain only records the lien. This is a ring record,
not bank logic, so it fits "banking is client side" (2026-11-25).

## Cash (exploratory, 2026-11-23; still being thought through)

**Decided (2026-11-25, replaces the class-group decision of 2026-11-24):** cash
starts with **RSA blind signatures, in small tranches**, with a **post-quantum
version planned later**: a STARK-based pool is the main candidate, and another
scheme may replace it if it proves better. Everything else in Next stays
post-quantum (ML-DSA).

- At least 3072 bits, following a published standard (RFC 9474, blind RSA); one
  key per series and denomination, shared so that 2 of 3 validators must sign
  (published threshold RSA). Key generation without a trusted dealer is open.
- **Cash is not post-quantum until then, and we say so.** A large quantum
  computer could forge notes; the loss is bounded by the cash outstanding
  (limit below). Payer privacy does not depend on RSA: the blinding is perfect.
- **Migration:** every note has a version and series. A post-quantum series is
  added as a new version; holders swap old notes for new ones until a deadline.
  This must happen before forgery is possible, since forged and honest RSA
  notes cannot be told apart.
- **Main post-quantum candidate: a note pool with STARK proofs** (the structure
  of Zcash's Orchard pool, without its elliptic curves). Notes are leaves of an
  on-chain hash tree. A spender proves "my note is in the tree" without showing
  which one and publishes a nullifier, so it cannot be spent twice. Fixed
  denominations, no hidden amounts. It rests on hash functions, and there is no
  signing key to break or steal. Anyone's note enters the tree when its backing
  enters the vault, so no validator signs notes.
  - Not measured yet: proof size (estimate about 100 KB, 120–150 KB for ten notes
    in one proof), proving time on a phone, verification time. Proofs could be
    pruned from storage, as signatures are (D15).
  - Counterfeiting through a flaw in the proof system would be invisible inside
    the pool, so the cash limit applies to the pool too.
  - To study first: Neptune Cash (described as private, post-quantum and built on
    STARKs; not checked by us).
- **Other candidates:** lattice blind signatures (Tanuki, Wombat), and the
  class-group action on supersingular curves (`experiments/cash_cycle/`, p = 419).
  The unknown class group of larger primes is no longer the obstacle (PEGASIS,
  2025, evaluates the action without it). Open for the curves are speed, size
  (about 70 KB per signature, our estimate), security proofs and maturity; no
  threshold blind signature from isogenies was found.

**Starting rules (2026-11-25; they replace the matching points below):**

1. Notes are unlabeled and 100% backed by units in an on-chain vault.
2. **Only banks buy notes from the vault; anyone redeems** (changed 2026-11-27).
3. Par both ways. Vault income (spin-offs) is seigniorage; backing is untouchable.
4. Anyone exchanges cash back directly on chain. Units with the fewest pending
   payouts leave the vault first ("oldest first", random and pro rata let a
   user trade barren primes for fertile ones at par).
5. Resale on chain is one atomic transaction; off chain, the receiver should
   swap at once.
6. Each note has its own one-time key that authorizes its swap or deposit (a
   bare serial can be stolen in transit).
7. Every validator checks a cash limit (raised only by vote with notice, zero
   until cash opens) and never accepts more notes of a kind than were printed.
   A kind is one series and denomination, with its own key.
8. **Tranches:** each kind also has its own small limit, so one broken RSA key
   puts only that kind at risk.
9. **Freeze:** if a kind's key is suspected broken (for example, more notes
   presented than printed), validators freeze the kind by vote. Its remaining
   backing is then paid out pro rata to the notes presented by a deadline, at most
   at par. Forged and honest notes cannot be told apart, so holders of that kind
   share the loss; no other kind is touched.

**Cash and the bank rules (2026-11-27):**

- **Vaults are keyless Fed accounts,** one per kind (series and denomination), like the
  reserve accounts; a frozen kind touches only its own vault. Ring income on the backing
  (seigniorage) goes to the Fed, as with reserves.
- **Banks buy notes** from their cash address and get units back there when they swap
  notes in. **Anyone can redeem** a note on chain directly; redemption cannot be blocked.
- **Withdrawals and deposits in cash are on chain** (see "every withdrawal is on chain"
  in the deposit rules): the bank records each one against the customer's signed
  instruction, so the deposit total stays exact at every block. The notes themselves stay
  unlinkable (blind signatures).
- **Refresh (agreed 2026-11-27):** a digital note can be copied, so whoever receives one
  should make the giver's copy worthless. Anyone holding a note can **refresh** it: one
  transaction with the old note (serial, signature, its one-time key signing the refresh)
  and a blinded new serial. The validators check the old note, record its serial as
  spent, and blind-sign the new one (2 of 3, partial signatures); the wallet unblinds it.
  Same kind and value, no units move, small fee; old and new notes cannot be linked.
  **The first spend of a serial wins** (redeem or refresh); a later one is rejected.
  Practice for receivers (not a protocol rule): banks credit a cash deposit, and shops
  deliver, only after their refresh or redemption is confirmed, so the giver's copy can
  never win. A bank recirculates deposited notes only after refreshing them. Notes taken
  offline, with no chance to refresh, are taken on trust.
- **No limit per bank** on cash held, as in the US; the risk (a broken key) is bounded by
  the total and per-kind limits whoever holds the notes. **Notes a bank holds do not
  count as reserves** (bearer notes cannot be shown to stay put); a bank swaps them for
  units to add reserves. Counting vault cash can come later.
- **No reporting of customers' cash** in the protocol; a bank whose country requires it
  does so in its own software.
- **Starting values (parameters, to be voted):** denominations 1,000 / 10,000 / 100,000 /
  1,000,000 micro-units; swap fee paid separately in units so notes stay round; each kind
  at most 1% of supply and all cash at most 5% when cash opens; backing of expired notes
  goes to the Fed treasury.
- **RSA keys:** at the testnet one party generates and splits the keys, said openly; later
  a key ceremony on chain (`ceremonies.md`).

Open: how a freeze is triggered and voted, how a freeze is triggered
and voted, swap fees (a unit fee breaks round notes into change), denominations,
backing of expired notes (treasury proposed), distributed key generation, and a
ring founder exchanging barren primes before opening a ring (costs seigniorage
only, so it is bounded by the limit).

The model follows real currency.

- **Validators print, banks distribute.** Banks buy notes from their
  validator with reserves (labeled units), wholesale. Customers withdraw and
  deposit cash only at banks. Validators never deal with customers.
- **Notes:** unlabeled, fixed denominations, **100% backed** by labeled units
  in the issuing validator's reserve. They earn no spin-offs. The backing
  keeps earning spin-offs, and that seigniorage goes to the validator
  (dividends) and to the protocol treasury (evidence bounties).
- **Privacy, Chaum-style:** notes are signed **blind**, so neither the bank
  nor the validator can recognize a note later: RSA first, as in Chaum's 1982
  ecash (see the decision above). The planned post-quantum version hides the
  payer through a proof instead of a blind signature, if the pool is chosen.
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

**Superseded for now (2026-11-25):** banking is client side only. The protocol
gets no bank transactions; cash is settled when we come to it. A ring is not a
bank: it is a productive asset (ring bond). The text below is kept for later.

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

5. **The 35% upstream share of 2 and 3 in each ring** (R11, 2026-11-25), since
   the genesis primes have no holders. At most two entries per ring; the only
   issuance validators receive. It ties validator income to ring openings,
   which validators control through κ; to be watched when κ is set.

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

**Simulated and chosen (2026-11-25):** a budget per main-chain prime, equal to its main
reward and shared by the open rings, older ones getting more (a variant of A with D).
Decay per ring (C) does not bound the total: 351 rings issue about 64 times the main
chain. A bounded total means that new rings dilute old ones (A) or earn little (D), so
the limit on new rings (κ) is needed with any rule. Numbers are parameters (D20,
`protocol-parameters.md`).

**Changed (2026-11-26): rule A4.** Each ring prime has a fixed value (parameter 17,
100,000 micro-units to start) instead of sharing one budget, so every accepted ring adds
money. The number of rings accepted and that value are the monetary levers; with 10
rings a year, supply grows about 19% in year 11 and holders of main-chain primes earn
about 3.5% a year from the backlog shares of new rings (`protocol-parameters.md`).

**Decided (2026-11-23): one unit of account.** All micro-units are equal,
whether issued by the main chain or by any ring. Ring issuance is the same
money, so budgets across rings (options A–D) are well defined.
