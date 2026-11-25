# Rings layer design (B1)

Status: in progress. Decisions are recorded as they are agreed. Plan:
`next-plan.md`, track B1. Certificates: `extension-certificates.md`.

## Structure (agreed earlier in discussion)

- **Ring registry:** records ring openings (certificates and discoverers),
  using commit–reveal.
- **Field chains:** one per opened ring. Each records, for every main-chain
  prime in order, how that prime factors in the ring (with generators).
- **Discoverer:** earns a share of the ring's own issuance. The main chain's
  reward split (D8) is unchanged.
- **Ring assets:** held and transferred on the main ledger, like main-chain
  primes (R1).

### Protocol messages (versioned, D12)

1. `RING_COMMIT`: discoverer address, commitment = hash(certificate ‖ address
   ‖ salt), refundable fee, signature.
2. `RING_REVEAL`: the commitment it answers, the salt, and the certificate
   (field identity D, claimed eligibility prime F, factorization of |D|,
   generators). Validators check, in order:
   1. it matches an earlier finalized commitment by the same discoverer;
   2. claimed F ≤ current chain frontier;
   3. the eligibility rule holds for F;
   4. the certificate verifies;
   5. no earlier commitment for the same D has been revealed (earliest wins).
3. `FIELD_ENTRY`: ring ID, main-chain prime q (the ring's next one, in order),
   decomposition type, generators, provider, signature.

## Decided

### R1. Structure: a DAG of ring chains (2026-11-23)

- Each ring has its own chain. A ring block contains: ring ID, previous ring
  block hash, `depends_on` (the hash of the main-chain block that finalized
  the prime(s) it records), the entries, and the validators' signatures.
- **One consensus sequence, two files (agreed 2026-11-25,** replacing "final on
  their own"**).** Ring batches (complete segments from any rings) are a third
  kind of decision, in the same sequence as ranges and primes (A3-1). Main
  blocks stay in the main file; ring blocks go to one append-only rings file,
  linked per ring, with an index.
- **Links:** ring blocks point to the main blocks that finalized their primes,
  and the next main header points back through `extension_root`. A ring entry
  never changes a main-chain mathematical record; it only adds new money.
- **`extension_root` is required for correctness** (revised 2026-11-25): a
  Merkle root over pairs (ring ID → hash of that ring's latest decided block),
  including the registry. It covers every ring batch decided since the last
  prime. **Ring money counts from the main block that first includes it,** and
  a wrong root makes that block invalid. It also lets a light client prove any
  ring fact from one main header plus short proof paths.
- **Protection of the main chain:** a prime can always close without a ring
  batch; at most one ring batch between two main decisions, with a size limit;
  validators can pause one ring or all rings (R2).
- Example: q = 13 splits in Q(√17). The miner submits `FIELD_ENTRY` (generator
  (a + b√17)/2 of norm ±13). Validators check that 13 is final on the main
  chain with 13 ≤ F, that the rows of its segment are correct and complete (R7,
  R9), the norm equation, and uniqueness (R10). It becomes Q(√17)'s next
  block, and the next main header's snapshot points to it.

### R2. Validator gates, with advance notice (2026-11-23)

Two policy gates, voted by validator quorum, following the institutional
model in which validators provide settlement and policy, as regional
Reserve Banks do:

- **Gate 1, new rings (global):** open or closed. While closed, `RING_COMMIT`
  is rejected on arrival, and no fee is taken.
- **Gate 2, mining per ring (amended 2026-11-23):** a ring accepted while
  gate 1 is open is **immediately open for mining**, with no separate
  validator approval. Validators can only **pause** mining on a ring: with
  notice like other gate changes, or immediately as an emergency pause (below).
  The discoverer share accrues while mining is not paused.
- **Notice:** gate changes take effect only after a notice period of N blocks
  following the finalized vote, so miners can predict the rules (Nash
  credibility). N is a policy constant to be set.
- **Exception:** an **emergency pause** of a single ring takes effect
  immediately, and only if its verifier is shown to be faulty. It is recorded
  on chain. An **emergency pause of all rings** works the same way
  (2026-11-25), since rings share the consensus sequence and the ledger (R1).
- Commitments finalized while gate 1 was open may still be revealed within
  the reveal window after it closes.
- Gate states are part of the policy digest in every block header (D9).

### R3. Eligibility: |D| ≤ F² (2026-11-23)

A quadratic ring Q(√D) may be submitted when **gate 1 is open and
√|D| ≤ F**, i.e. the integer check **|D| ≤ F²**, where F is the main-chain
frontier. The same rule applies to real and imaginary fields, with no π.

- Eligibility (policy) and proof (mathematics) are separate: the certificate
  still gives generators for the non-inert primes up to the Minkowski bound
  M_K. Since M_K < √|D| ≤ F, every prime a certificate refers to is already
  on the main chain.
- With genesis F = 3: |D| ≤ 9, so six fields — D = 5, 8, −3, −4, −7, −8,
  i.e. Q(√5), Q(√2), Q(√−3), Q(i), Q(√−7), Q(√−2), all PIDs with empty
  certificates.
- Higher degrees: rule to be set in B2 (for example M_K ≤ F); the typed
  certificates (R4) allow it without a format change.

### R4. Generic, typed certificates and entries (2026-11-23)

- The block and registry never look inside a certificate. A registry entry
  stores the certificate **type** (1 = quadratic; more types later), a
  **version**, and the certificate bytes. Validators dispatch on the type.
- **Ring ID** = hash(type ‖ degree ‖ canonical identity bytes). For quadratic
  fields the canonical identity is D. A degree-2 field must use type 1, so the
  same field cannot be registered twice under different types.
- **Elements** are encoded as n integer coefficients plus a denominator,
  relative to the field's defining polynomial. For quadratic fields that is
  (a, b) / 2.
- **Field-chain entries** list the prime ideals above q as (e, f, generator)
  items, with the check Σ e·f = n.
- **Adding a certificate type** (for example cubic) is a protocol upgrade
  agreed by the validator community. It activates with notice at a block
  boundary (D12 versioning, R2 notice) and changes no existing chain.
- **Subfields:** fields of prime degree (2, 3, 5, …) have no proper subfields,
  so cubic rings are separate trees, like quadratic ones. From degree 4
  onward a field can contain smaller ones (for example Q(ζ₈) ⊃ Q(i), Q(√2),
  Q(√−2)), and a prime found in the smaller ring can split further. Then a
  ring block may carry an optional `depends_on_subfield` link to the subfield
  ring's block for the same q. Validators check consistency: the bigger
  ring's primes must map down (by the relative norm) to the smaller ring's
  primes. These are the edges between rings in the DAG.

### R5. Commit–reveal for new rings only (2026-11-23)

- Applies to ring registrations (`RING_COMMIT` → `RING_REVEAL`). The reveal
  must come **within 10 blocks** after the commit is finalized, otherwise the
  commit expires.
- A small **refundable fee** accompanies the commit. It is returned with a
  valid reveal and kept if the reveal is invalid or missing (anti-spam).
- Field entries and main-chain ranges do not use commit–reveal: each prize is
  small, and copies are handled by arrival order (D4) and one entry per q per
  ring (R10). This can be strengthened later if copying is observed.

### R6. Founder record and reward schedule (2026-11-23)

Each ring's registry record carries:

- **founder (identity):** permanent attribution of who discovered the ring;
  never changes;
- **payee:** who currently receives the founder share. It starts as the
  founder and can be changed by a signed assignment;
- **liens (optional, versioned):** "pay lender L until X units are repaid,
  then return to the payee", so ring bonds can be pledged (see
  `monetary-layer.md`);
- **reward schedule:** a type plus parameters (founder share y, decay kind
  and rate, budget), **copied from the validators' current policy when the
  ring is registered and fixed for that ring afterwards**. Validators may
  change the policy for future rings only, with notice. Existing ring bonds
  keep their terms.

The actual schedule (budget per epoch, per cohort, decay per ring or across
rings) is open and will be chosen by simulation (`monetary-layer.md`); the
format covers all of the options.

### R7. Ordering by main-chain prime, in segments (revised 2026-11-25)

Replaces ordering by norm. For quadratic rings the paid primes keep the same
order, since each has norm q.

- **One row per main-chain prime.** A ring's ledger goes through 2, 3, 5, 7, …
  in order and gives every prime a row: split, ramified or inert. Each ring
  has an explicit frontier ("complete up to q"), with **q ≤ F**. This replaces
  the earlier invariant that no norm passes F (in higher degrees a row may
  contain ring primes of norm q² and up).
- **Segment:** any number of inert primes, then one or more paid (split or
  ramified) primes, then the first inert prime after them, which closes the
  segment. A segment also closes at its **32nd paid prime**, with no closing
  inert prime; this covers long runs and rings that have no inert primes at
  all, such as Q(√2, √3).
- **At most 1,024 inert primes** before the first paid prime (2026-11-25). At
  1,024 with no paid prime, the segment closes as an **empty segment**, which
  pays nothing. A miner who wants the next paid primes submits it along with
  the following segment, so the ring is not stalled. It bounds message size;
  the longest run measured is 20.
- **One submission is exactly one complete segment.** Arithmetic fixes where a
  segment ends, so miners have no choice there. Nobody can copy a segment and
  extend it. Identical submissions are a tie, settled by the round's proposer
  plus the vote (a copier with a faster path to the proposer can still win).
  Later segments may be sent ahead as separate submissions. Segments are
  decided in order.
- Each paid prime in a segment is rewarded separately (R11).
- Example, Q(√17): [2 | 3], [5, 7, 11 | 13, 17, 19 | 23],
  [29, 31, 37, 41 | 43, 47, 53, 59 | 61].
- **Why no holes:** every ring prime lies above exactly one main-chain prime,
  so covering every q covers all ring primes; since the ring is a PID, every
  other element is a product of them, up to units.
- **Measured run lengths** (all 351 PIDs with |D| ≤ 2209, primes to 20,000):
  longest inert run 20 (D = 2104), longest split/ramified run 23 (D = 721),
  latest first paid prime 41 (D = −163). Nothing guarantees a bound, so the
  rules do not assume one.
- A global order across all trees is **not** part of consensus. It is a
  derived index, as is order by norm.

### R8. Conjugates: one entry per split prime (2026-11-23)

When q splits into π and π̄, one entry is recorded with **one canonical
generator**; the conjugate is derived from it. There is one miner reward per
split q, since finding one generator finds both. As **assets**, π and π̄ are
distinct (different prime ideals, possibly different futures in larger
rings), and each receives half of the split's issuance. The canonical choice of which
conjugate (for example b > 0 in Q(i), so 2 + i rather than 2 − i) is fixed
together with canonical generators modulo units.

### R9. Inert primes are marked but not paid (revised 2026-11-25)

- The miner marks inert primes in the segment (about 2 bits each, no
  generator). Without the marks, "inert" and "not mined yet" would look the
  same after a ring's last entry.
- They pay nothing and create no asset. The main chain already paid for their
  primality.
- **Validators verify every row and never search for generators.** Inertness:
  one Kronecker symbol in quadratic rings; in degree n, irreducibility of the
  ring's polynomial mod q. The finitely many primes dividing the polynomial's
  index need a check covered by the ring's certificate (quadratics have
  none).

### R10. Units and associates (2026-11-23)

- A recorded generator must have norm ±q for a prime q > 1. Units (norm ±1)
  can never pass.
- **Exactly one row per rational prime q per ring,** so the same prime ideal
  cannot be recorded twice under different associates (π and π·ε).
- Special treatment of units (for example recording fundamental units as
  evidence) would be a new versioned entry type.

### R11. Ring reward split and upstream flow (2026-11-23, starting values)

- Each ring entry's issuance (in the same equal micro-units as the main
  chain; one unit of account) is split (revised 2026-11-24) **50% to the
  entry's miner** (found the generator), **35% upstream**, **15% to the ring's
  discoverer** (R6). The miner's proportion matches the main-chain prime
  finder's 50% (D8). There is no general validator share (`monetary-layer.md`).
- **Split and ramified entries issue the same total.** For split q, half of
  every share is paid in x and half in y (R8); for ramified q, all of it is
  paid in the single prime x.
- **Upstream** goes to the ancestors along the DAG: for quadratic and cubic
  rings that is the **holders of p's micro-units, pro rata, at the moment the
  entry is finalized** (spin-off model, `monetary-layer.md`). For nested rings (degree ≥ 4)
  it is divided along the path, halving per step, with the remainder to the
  main-chain prime's current holders (to be confirmed: halving or equal
  split). The total
  stays 35% at any depth.
- **The genesis primes 2 and 3 have no holders (D7), so their 35% goes to the
  validators** (2026-11-25), at most twice per ring. It is shared equally
  between the active validators' payout addresses with the epoch distribution
  (D17), in units of the new ring primes. The miner's 50% and the founder's
  15% are unchanged. This is the only issuance validators receive.
- Only split and ramified primes are paid (R9), so inert primes pay no
  one. A main-chain prime keeps earning for its **current holders**, not its
  original finder (confirmed 2026-11-24), each time it splits or ramifies in
  another ring.
- The numbers are starting values, to be finalized with the economics.

### R12. Galois and non-Galois rings; only degree-1 primes pay (2026-11-25)

- Rings may be opened whether or not they are Galois. A non-Galois ring K and
  its Galois closure are different rings, and each may be opened if it is a
  PID. This does not change the order of work: quadratic rings (all Galois)
  come first.
- **Each prime above q with f = 1 (norm exactly q) issues e/n of the entry's
  full amount,** where n is the degree. Primes with f > 1 issue nothing and
  create no asset. In quadratic rings this is R8, R9 and R11 unchanged: split
  1/2 + 1/2, ramified 1, inert 0.
- The average issuance per row is 1/n in every ring (a transitive group has
  one fixed point on average; Chebotarev). Measured for primes to 100,000 in
  nine rings of degree 2 to 5, Galois or not: within 0.004 of 1/n.
- A prime with f = 1 is never merely extended from a smaller ring, so no prime
  ideal is issued twice, whatever order rings open in. Below it lie only
  primes with f = 1.

### R13. Upstream payouts are claimed (2026-11-25)

- When a ring entry pays, its miner and founder are credited at once. For the
  upstream share the chain writes **one record per new prime and per level
  below** (which prime earned how much of which new prime, at block h). No
  owner lists are written: the ledger already fixes who owned what at h.
- Owners receive their share by a **claim**, pro rata to the balance they held
  at h. The protocol computes the amount and every validator checks it; a
  marker per address and asset prevents double payment. No proof of past
  ownership is needed, since the chain keeps the balance history.
- **Anyone may trigger a claim for any address**, paying the fee. The payout
  can only go to the rightful address, so wallets or a distribution script may
  do it, and nothing is lost if a script stops.

## Proposed (awaiting agreement)

- **Claim details (proposed 2026-11-25):** payouts apply before the block's
  transfers; units held by no one (fee pool) earn nothing; only claimed units
  earn from higher rings, the level's share being divided among them; holder
  amounts are rounded down and the remainder is never issued.
- **Merkle `state_root` (proposed 2026-11-25):** today it is one flat hash over
  every asset, key and nonce. A tree rehashes only what changed and allows
  short proofs.
- **Passing the 35% down a tower (proposed 2026-11-25):** of whatever reaches a
  prime, its holders receive half (rounded down) and the rest is passed in
  equal parts to the primes directly below; the main-chain prime keeps all
  that reaches it. Rings that are not PIDs, not open, or have not reached q
  are skipped. A non-Galois ring that sits in a larger ring in several ways
  counts once per embedding.
- **Segments above degree 2 (proposed 2026-11-25):** a segment ends at an
  unpaid row rather than an inert one (some rings have no inert primes); the
  32 and 1,024 limits stay. The same thing in quadratic rings.
- **Lower rings first (proposed 2026-11-25):** a higher ring waits for its PID
  subrings, so that lower owners receive their share.
- **Automorphisms (proposed 2026-11-25):** no Galois flag. A certificate of
  degree ≥ 3 may list automorphisms; the ring is shown as Galois when n
  distinct ones verify. They shorten entries and never affect rewards.
- **Ties (reviewed 2026-11-23):** there is no global arrival order and there
  are no timestamps. Main-chain range ties (same end) are settled by the
  round proposer's choice plus the vote (D4). Ring certificates go to the
  earliest finalized commit (R5). Field entries: one per q per ring (R10); if
  two compete, the proposer's choice plus the vote decides. A content-hash
  tie-break without sealed commits was rejected, because a copier could grind
  a lower hash.

## Open

- Reveal window length; commit fee amount and refund rule.
- Ring-asset issuance per field-chain entry.
- Canonical field identity for degree ≥ 3; canonical generators modulo units
  (needed before ring assets exist).
