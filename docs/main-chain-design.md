# Main-chain design (A1)

Status: A1 complete (2026-11-23). All decisions D1–D14 agreed. Plan: `next-plan.md`, track A1.

## Decided

### D1. Evidence: an explicit divisor for every composite (2026-11-23)

As in v1, each composite n carries a divisor d with 1 < d < n and d | n.
Validators check one division per composite. Small-factor composites get no
implicit (sieve) shortcut.

### D2. Ranges contain only composites; the prime is separate (2026-11-23)

- A **range submission** covers consecutive integers [start, end], all
  composite, each with its divisor. It never contains a prime, so its length
  is bounded by the current prime gap.
- The **closing prime** of a block is a separate submission: the prime p, its
  Pratt witness (the factorization of p − 1 is already on chain), provider and
  signature.
- A **block** is: accepted ranges covering every integer of the gap
  (p_prev, p), then the closing prime p.

### D3. Two frontiers (2026-11-23)

- **Chain frontier:** the last finalized prime (the last closed block).
- **Accepted frontier:** the last accepted composite in the currently open
  gap. Every accepted range starts exactly at the accepted frontier + 1.
  Both are reported separately in status.

### D4. Range competition and acceptance (2026-11-23)

1. Among competing range batches for the same position, **the batch with the
   highest end wins**. This is a content rule, so it does not depend on
   network position. **Ties** (exactly the same end) are rare and are settled
   by the round's proposer, which picks the first one in its own queue; the
   vote then makes the choice final. There are no timestamps and no global
   arrival order. (Clarified 2026-11-23. v1 achieved full position
   independence with sealed commits, an agreed set and the lowest commitment
   hash, at the cost of extra rounds per integer.)
2. A batch that arrives while another is being decided is **queued**.
3. After a decision, a queued batch is re-examined against the new accepted
   frontier f:
   - if its end ≤ f, it is **stale** and dropped;
   - otherwise its **tail** [f + 1, end] is processed.
4. **Signature semantics:** a miner signs [start, end] and thereby claims every
   composite in it. Any tail [s, end] with s ≥ start may be accepted, and the
   miner is credited for exactly that tail. No re-signing is needed and
   nothing is re-attributed. The acceptance record states the accepted
   sub-range.
5. **The losing miner gets nothing** for the overlapping part. Example: X
   submits 1010–1011 and Y submits 1010–1012; Y is accepted, and X receives no
   credit for 1010–1011.
6. **At most 1,024 composites per range** (2026-11-25). Partial ranges are
   allowed, so a longer gap is filled by several ranges and nothing stalls.
   The limit is checked before the divisor list is decoded, and before any
   signature. Reason: without it a message could carry about a million
   64-byte divisors (~68 MB); with it a range stays under about 75 KB. Real
   gaps are far smaller: 219 composites below 10⁸, 539 at 738832927927 and
   1,549 (3.3 KB of smallest divisors) at 18361375334787046697. Limits on
   pending ranges per miner and on total message size belong to A3.2. The
   limit is implemented (`kMaxRangeComposites`).

### D5. The closing prime is its own decision (2026-11-23)

Once the accepted frontier reaches p − 1, the closing prime p is decided
separately. Its acceptance closes the block, and the chain frontier becomes p.

### D6. Transactions are included per block (2026-11-23)

- Main-chain transactions ride in the **closing-prime decision**. A transfer
  is final when its block closes, and its order within the block is
  preserved.
- **All money and ownership live on the main chain's ledger** (updated
  2026-11-23, Model 1): ring-prime micro-units are labeled assets on the main
  ledger, credited when ring entries are finalized. Ring chains hold only the
  mathematical evidence (see `monetary-layer.md`).
- Consequences, accepted: finality time equals block time (seconds to a
  minute with active mining); if nobody mines, transfers wait, as in v1. A
  transaction-only fallback decision can be added later without changing the
  block format.

### D7. Genesis seeds 2 and 3 (2026-11-23)

- Genesis contains the primes 2 and 3 as a seed, with no mining rewards and no
  units. Their upstream share in rings goes to the validators (R11).
- The first mined block closes at 5, with gap {4}. From then on every gap
  contains at least one composite, so the reward rule has no special case
  (in v1, the block closing at 3 had an empty interval).
- Height equals prime index: 2 and 3 are primes #1 and #2 in genesis, and
  block #3 closes at 5.
- For the rings layer the initial frontier is F = 3, so every field with
  M_K ≤ 3 is eligible from genesis.

### D8. Rewards within a block (2026-11-23)

- `reward(p)` is a protocol function of the closing prime. At launch it
  returns 1,000,000 micro-units for every p, as in v1, and later schedules
  can change it without changing the block format.
- Split (revised 2026-11-24): **50% to the prime finder, 50% to composites**,
  the equal division: a block needs both contributions. It replaces 45/45/10;
  the 10% validator share is dropped, and validators earn from services and
  assets instead (`monetary-layer.md`).
- The composite share is paid **per composite**: share = 50% ÷ (number of
  composites in the gap). Each range miner receives share × (composites
  accepted from their range, i.e. the accepted tail, per D4).
- The remainder of the integer division goes to the prime finder.
- Example, gap 1130–1150 (21 composites), prime 1151: share = 500,000 / 21 =
  23,809, remainder 11. Y (11 composites) gets 261,899; Z (10 composites)
  gets 238,090; the prime finder gets 500,011.

### D9. Block header (2026-11-23)

Signed content: version, network ID, height, previous block hash, prime,
policy digest, validator-epoch digest, `ranges_root`, `transactions_root`,
`state_root`, `extension_root`, a reserved `privacy_root` and a reserved
`finance_root` (both empty for now; `finance_root` is for the monetary layer:
shares, bonds, loans, reserves).
The previous prime is not stored: it is the previous block's prime, which the
previous hash commits to. The block certificate (the precommit signatures)
sits outside the signed content.

### D10. Signing domains and validator keys (2026-11-23)

- Every consensus message (proposal, prevote, precommit, round change) signs:
  a new domain tag, network ID, validator-epoch digest, position (block
  height, plus the accepted frontier for range decisions), round, phase, and
  the value hash or nil. v1 votes and tags are never reused.
- **Two keys per validator** (in v1 one identity both signed and received
  rewards):
  - a **consensus key** (hot, on the server), used only to sign consensus
    messages and endpoint updates;
  - a **payout/operator key** (cold, offline), which receives validator
    rewards, owns the reserve, and can rotate the consensus key.
  - At registration both keys sign a binding: "this consensus key acts for
    this payout address". Votes are verified against the consensus key, and
    rewards are paid to the payout address. A compromised server can produce
    bad signatures (handled by the quorum and equivocation evidence), but it
    cannot take funds.

### D11. Timeouts (2026-11-23)

Round timeout: 1 s base, doubling each round, capped at 60 s, and reset after
each decision. A timeout advances the round; it never clears safety state.

### D13. Fault model, stated plainly (2026-11-23)

Three validators with a quorum of 2 tolerate **one crashed or offline
validator, not one dishonest validator**. Tolerating one Byzantine (dishonest)
validator requires **four independent validators** (separate operators, hosts
and keys), with quorum 3; the protocol does not change. Documentation and
papers must say this explicitly.

Validator policy (founder-run validators said openly, checking nodes, path for
investor-appointed validators): `validator-policy.md` (2026-11-26).

### D15. Miner keys and signatures (2026-11-23)

- **Key registry:** a miner's public key is stored once in the chain's
  lifetime, in the block where the miner's work is first accepted
  (`new_keys`). It is part of the state (`state_root`). Later submissions by
  that address may omit the key.
- **Miner signatures are journal data,** not permanent block contents.
  Validators verify them at submission time, before voting. Blocks keep the
  evidence (divisors, Pratt witness), the attribution, and the **hash of the
  full signed submission, signature bytes included**, so an archived
  signature can always be matched to its block. Replay verifies all
  arithmetic and the validators' block certificate. Only an after-the-fact
  audit of attribution against a dishonest quorum would need archived
  signatures, and that is the same trust assumption as the rest of the
  chain.
- Effect (engine A2.1, chain to 19,997): 13.2 MB → 1.30 MB (576 bytes per
  block). Validator certificates (A3) will add ML-DSA signatures per block;
  checkpointing is to be decided there.

### D16. Transfers (2026-11-24)

v1's transfer model with Next's types (D14). A transfer contains: version,
network ID, sender, inputs [(asset ID, amount)], outputs [(asset ID, amount,
receiver)] (several outputs allowed, so a bulk send is one transaction, up
to a policy cap), fee (asset ID, amount), nonce (per sender, increasing by
exactly 1), an optional sender key (D15 registry), and a signature.

- Conservation per asset: inputs = outputs + fee. The fee is set by policy
  (v1: 1 micro-unit) and paid into the validator fee pool.
- Asset IDs are main-chain primes now, and ring primes later in the same
  format.
- Transfers are included with the closing prime (D6), in order;
  `transactions_root` is the Merkle root over the encoded transfers.

### D17. Automatic distributions (2026-11-24)

Every epoch (N blocks), the protocol itself pays out the fee pool (the
validator reward pool is gone, D8) to the active validators' **payout addresses** (D10):
equal shares, remainder by a fixed deterministic rule. This is checked in
replay. v1's distribution transactions and ops scripts are no longer
needed. Participation-weighted shares (as in v1) may be added later.

### D18. Validator records (2026-11-24)

A validator record binds a consensus key to a payout address, signed by
both (D10). In A2.2 the genesis validators are defined in genesis; adding
and removing validators by quorum vote comes with consensus (A3), and
reserve and admission rules with the monetary layer.

### D19. Persistence (2026-11-24)

An append-only block file with an index, and a state snapshot every K
blocks written atomically (write, sync, rename). On restart: load the
latest snapshot and replay the blocks after it. State is never rebuilt per
request.

### A3-1. Units of consensus (2026-11-24)

- Each decision is one consensus **instance (h, k)**: decision number k within
  open block h. Range decisions are k = 0, 1, …; the last instance at height h
  is the prime decision (closing prime plus transactions, D5–D6), which
  seals block h.
- Within an instance, **rounds r** follow the two-phase model: the proposer
  proposes a value (a range batch, the closing prime plus transactions, or
  nil); validators prevote; a quorum of prevotes (polka) locks the value and
  leads to precommits; a quorum of precommits decides. Timeouts start at 1 s
  and double, capped at 60 s (D11).
- **Proposer:** validator (h + k + r) mod n in validator-set order. It is
  deterministic and independent of miners' submissions.
- **Ring batches are a third kind of decision in the same sequence** (revised
  2026-11-25; formerly a sequence of their own). They are always optional, at
  most one between two main decisions and limited in size, so a prime can
  always close without one (R1).
- Every vote signs the D10 domain: network ID, validator-set digest,
  (h, k, r), phase, and the value hash or nil.
- Block decisions are **objective** (validity only). Subjective policy is
  decided by A3-2.

### A3-2. Governance votes (2026-11-24)

- Covered: everything v1 voted on (transfer fee, minimum reserve, validator
  set changes), plus ring policy (κ, pauses, reward schedules for new
  rings), protocol upgrades (new certificate and transaction versions),
  economic parameters and, later, monetary-layer rules.
- **Votes do not depend on the chain position**, which fixes v1's
  maintenance-mode problem: a vote signs the proposal content, effective
  height, expiry height, sequence number, validator-set digest, network ID
  and a governance tag, and nothing about the current tip.
- **On-chain tally:** each validator submits its vote as its own small
  signed transaction, at any time. Blocks include votes like transactions.
  When a quorum is reached, the change is scheduled at its effective
  height, which must be at least N blocks later (notice). Without a quorum
  by the expiry height, the proposal lapses.
- Votes count only for the validator set named by their digest.

### A3-3. Certificates and validator-set changes (2026-11-24)

- **Storage:** a block's certificate (bitmap of signers plus their
  precommit signatures, in validator order; any valid quorum is acceptable)
  is not part of the block's identity (D9). Kept permanently: certificates at
  every validator-set change and a checkpoint every K = 1,000 blocks. Kept
  for the last 1,000 blocks: every certificate. All other certificates are
  pruned; blocks are never pruned. One certified block vouches for its whole
  history through the hash chain. About 13 MB of certificates per million
  blocks, instead of 6.6 GB.
- **Transitions:** a set change is voted by the old set (A3-2) and takes
  effect at height H after notice. The last block before H (whose state
  contains the new set) is certified by the old set, and the first block at
  H by the new set; both certificates are kept permanently, forming a chain
  of trust from genesis.
- **Liveness proof for newcomers:** an added validator counts only after an
  on-chain "ready" message signed with its new consensus key arrives before
  H; otherwise the inclusion does not activate.
- **Set binding:** every vote and certificate names its validator-set
  digest and is checked against the set active at that height, taken from
  certified state. A removed validator can never form a quorum alone.
- **Losing validators:** any quorum decides, so one lost validator cannot
  stall the chain. The proposer rotation **skips a validator after M
  consecutive missed proposer turns** (deterministic from certificates:
  decisions in round > 0), until it signs again. A lost hot key is replaced
  by rotating in a new consensus key with the cold payout key (D10), with no
  vote needed. Cold payout keys never live on validator servers. Losing both
  keys means removal by vote. Losing a quorum (two of three) cannot be
  handled by any protocol, so a documented emergency procedure is needed:
  a new set announced by the operators, restarting from the last certified
  block.
- **Old keys:** permanent checkpoints held by peers guard against forged
  old histories if an entire old set's keys leak.
- **Rule:** no single validator is ever indispensable. Every feature is
  checked against it (for example, cash from a lost validator remains
  redeemable on chain from its backing).

### A3-4. Networking and catch-up (2026-11-24)

- **Two kinds of links:** validator ↔ validator links are authenticated
  (each side proves its consensus key), persistent, and carry proposals,
  votes and certificates. Public links (miners, wallets, clients) have
  per-client limits and never compete with validator traffic (F13).
- **Messages:** proposals, prevotes, precommits; submissions (ranges,
  primes, transfers, governance votes, ring messages); and block and
  certificate requests. All are versioned (D12) and size-bounded.
- **Catch-up:** a node that is behind fetches certified blocks in chunks by
  height, verifies each one (all mathematics, hash chain, certificate
  against the set active at that height), keeps partial progress, and
  retries another peer on failure (F7). Any local block that is
  uncertified, or conflicts with a certified block, is rolled back
  automatically (F6). **A fresh node syncing from genesis is a mandatory
  test** (F18).
- **Mempool:** revalidated against finalized state after every block (F9).
- **Handshake:** protocol version and build commit are exchanged; the build
  commit is also reported in status and at startup (F12).
- Errors and logs carry full context, and decisions are logged as
  structured events (F8, F19). One executable with subcommands (F11).

### A3.1 implementation notes (2026-11-24)

The consensus core (`next/include/primechain_next/consensus.hpp`) follows
A3-1 to A3-3 and the model test. Choices the design left open, for review:

1. **The previous certificate travels in each value.** A proposed value
   embeds the certificate of the previous instance (like Tendermint's
   LastCommit), so the proposer-skip counters come from agreed data and every
   validator computes the same proposer. Counting: in that certificate's
   instance, the proposers of rounds 0 … r−1 each gain a missed turn; the
   round-r proposer and every signer reset to zero. Counters therefore lag
   one instance.
2. **Skipping:** the proposer is candidates[(h + k + r) mod |candidates|],
   where candidates is the active set (in validator order) minus validators
   with ≥ M missed turns; if that is empty, nobody is skipped. M is
   `Policy::proposer_skip_after` (3).
3. **What a certificate certifies (decided 2026-11-24):** the value hash
   of an instance. A prime value includes the **hash of the sealed block
   header**, so the certificate of a block's prime decision certifies that
   header directly: a block fetched from anyone is checked against its
   certificate without re-executing (F7), and light clients can follow
   certified headers. The proposer takes the hash from its dry run; every
   validator recomputes it in `checkPrimeValue` and prevotes nil on a
   mismatch, and sealing a decided value checks it again. Range values
   carry no header (they seal no block); the embedded previous certificates
   chain them to the next prime decision.
4. **Proposer's choice:** a valid closing prime first (a valid prime means
   the gap is covered), otherwise the highest-end pending batch. Nil is
   only a vote. A proposer with nothing to propose still runs the propose
   timeout, and proposes as soon as a submission arrives during its propose
   step.
5. **Delivery inside the core:** newly accepted votes and proposals are
   relayed once (gossip, which is also how equivocations reach everyone), and
   every node re-sends what it signed in the current instance every 2 × base
   timeout. A node that sees messages from a later instance asks the sender
   for decisions (`SyncRequest`, answered with up to 32 `Decision`s, each
   checked against its certificate and applied). This in-process catch-up
   stands in for A3-4's block-file catch-up, so a node can only catch up
   within the decision window.
6. **Governance (A3-2) details:** a vote counts if it is signed by an active
   validator's consensus key, names the active set's digest, is included at
   a height ≤ expiry, its sequence is above the last passed sequence, and the
   voter has not voted on it yet. At quorum the change is scheduled at
   max(effective height, inclusion + `Policy::governance_notice`) and the
   sequence is consumed. Tallies without quorum are dropped once their expiry
   passes. Changes effective at H are applied right after sealing block
   H − 1, so the new set signs from H on. A fee change replaces the transfer
   minimum fee. A removal never removes the last validator.
7. **Ready message:** a `GovernanceTx` of kind Ready, signed by the new
   consensus key over the proposal ID, must be included at a height below H.
   At H the binding (both keys) is checked again; a duplicate consensus key is
   ignored. Each ready entry stores the proposal's expiry and sequence. An
   entry is pruned as soon as its proposal **can no longer pass** (expired,
   or its sequence is used up by a passed proposal) and is not scheduled;
   expired entries go at the same block as the lapsed tally. So the
   governance state, state root and snapshots stay bounded. A ready message
   for such a proposal is rejected at inclusion and dropped from the
   mempool, as is a duplicate of a recorded one. A passed proposal keeps
   its ready entry until it takes effect, even past its expiry.
8. **Block format:** blocks gain a governance list after the transfers;
   `transactions_root` covers transfers, then governance transactions. The
   governance state (minimum fee, last sequence, tallies, ready messages,
   schedule) is in the state root and the snapshot, and so is the current
   validator set. The two new policy fields are in the policy digest.
9. **Retention:** certificates are kept for every block in the last W
   blocks, every K-th block, and both blocks around a set change (the last
   block of the old set and the first of the new). Decided values are kept for
   the window only.
10. **Restart:** `Durable` holds the decisions of the open block (ranges of
    the open block are not in the Store), certificates, skip counters, the
    lock, the valid value, the round, and every vote and proposal already
    signed in the current instance, which are re-sent and never re-signed.
    The simulated harness captures it at the crash, which equals persisting
    after every event because crashes fall between events; a real node must
    write it to disk before sending.
11. **Evidence** covers votes only; a second proposal from the same proposer
    in a round is ignored (the first one wins).
12. **Known limitation, n = 3:** with q = 2, one Byzantine validator can make
    the two honest validators decide different values (test
    `next_consensus_byzantine3` shows it). The equivocator's two conflicting
    precommits are in the two certificates, so the evidence can always be
    extracted afterwards. n = 4 tolerates one equivocator
    (`next_consensus_byzantine4`).
13. **Engine changes for consensus:** `applyDecidedRange` applies exactly the
    decided batch; `submitPrime` queues primes; `buildPrimeContents` and
    `checkPrimeValue` dry-run block sealing on a state copy (without block
    history); `applyDecidedPrime` seals with strict checks and revalidates the
    mempool (F9).

### D14. Number types (2026-11-23)

| Kind | Examples | Type | Encoding |
|---|---|---|---|
| Mathematical integers | integers, primes, divisors, witnesses, discriminants, generators | arbitrary precision (GMP), checked | canonical minimal length-prefixed bytes; maximum size per field type (main-chain integers 64 bytes; certificate integers up to 4096 bytes) |
| Amounts | balances, rewards, fees, supply | unsigned 128-bit, checked (overflow is rejected, never wrapped) | fixed or varint |
| Counters | height, round, version, epoch | unsigned 64-bit | varint |

One integer type is used across the main chain, rings, curves and landmarks.
Money is not 64-bit, because total supply could pass 2⁶⁴ micro-units.

### D20. Every number is a protocol parameter (2026-11-25)

Numbers are not settled in the design. Each is a parameter with a starting value, in one
versioned table that is part of the state and the policy digest, changed by vote with
notice (A3-2) and never retroactively. Rules, bounds and the table:
`protocol-parameters.md`. The engine's `Policy` holds some of them today, and only the
minimum fee can be voted on; the rest follows with B3 and A3.2.

### B3.1 implementation notes (2026-11-26)

The parameter table (`next/include/primechain_next/parameters.hpp`). Choices the design left
open, for review:

1. **Ids and units:** 36 parameters with stable ids 1–36 (never renumbered or reused), all
   unsigned integers: micro-units, blocks, bytes, milliseconds, basis points (10,000 =
   100%), and the ring weight exponent as two parameters (numerator, denominator). Shares
   whose partner takes the rest are not parameters: composites (D8) and the ring miner (R11).
   "To be set" entries of the table are not included yet.
2. **Hard bounds** are placeholders in code. The range limit (1,024) and the main-chain
   integer size (64 bytes) cannot be raised by vote, because decoders reject larger
   messages before any check; raising them is an upgrade. Combined rules: ring upstream plus
   founder ≤ 100%, timeout cap ≥ base.
3. **History:** per parameter, (from height, value). The value in force for block h is the
   last entry with from ≤ h. The table and its history are in the snapshot and in the state
   root (through the governance digest).
4. **Headers:** `policy_digest` is the digest of the table in force for that block. The
   genesis configuration digest (network, limits, starting table) only guards snapshots.
5. **One kind of vote:** `ParamChange` with a list of (id, old value, new value), replacing
   the fee-only vote. Form check: known ids in increasing order, new values within bounds and
   different from the old ones. At inclusion of each vote: the old values must be the values
   in force for that block, and the result must satisfy the combined rules; otherwise the vote
   is not included. At activation the change applies only if every old value still holds;
   otherwise it is dropped whole (an earlier change moved it). The notice is the value in force
   at the block that completes the quorum.
6. **Consensus:** the proposer-skip count is read for the block being decided (it affects who
   may propose). Timeouts and certificate retention are not validity rules; a node follows
   the table unless its operator overrides them locally (the tests do).
7. **Engine reads:** reward, finder share, range limit, integer size, minimum fee, outputs,
   epoch length and notice, each for the block being built. Ring and cash parameters are only
   stored for now.

### Fixed by earlier work

- **Consensus:** two-phase (prevote/precommit) rounds, lock on a quorum of
  prevotes, justified unlock, any quorum of signers, rotating proposer,
  persist-before-send, quorum `ceil(2n/3)`. See `round-change-audit.md` and
  the two model tests.
- **The engine:** one in-memory state with incremental updates, never rebuilt
  per request. See `v1-baseline-measurements.md`.
- **No lottery.**

### D12. Versioning (2026-11-23)

Every record, transaction, certificate and message type carries a version.
Unknown versions are rejected. New evidence types and privacy transactions
arrive as new versions, without a fork.

## Open (A1)

None. A1 is complete; this document is the specification input for A2 (the
core engine).

