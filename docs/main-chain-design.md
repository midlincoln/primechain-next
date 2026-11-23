# Main-chain design (A1)

Status: in progress. Decisions are recorded as they are agreed; open items
are listed at the end. Plan: `next-plan.md`, track A1.

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
   highest end wins**. Ties go to the earliest arrival.
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

### D5. The closing prime is its own decision (2026-11-23)

Once the accepted frontier reaches p − 1, the closing prime p is decided
separately. Its acceptance closes the block, and the chain frontier becomes p.

### D6. Transactions are included per block (2026-11-23)

- Main-chain transactions ride in the **closing-prime decision**. A transfer
  is final when its block closes, and its order within the block is
  preserved.
- Ring assets (rings layer) are transferred inside **their own field chain**,
  so each ring is its own tree of finality. The main chain only anchors them
  through `extension_root`.
- Consequences, accepted: finality time equals block time (seconds to a
  minute with active mining); if nobody mines, transfers wait, as in v1. A
  transaction-only fallback decision can be added later without changing the
  block format.

### D7. Genesis seeds 2 and 3 (2026-11-23)

- Genesis contains the primes 2 and 3 as a seed, with no mining rewards.
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
- Split: 45% to the prime finder, 45% to composites, 10% to the validator
  pool.
- The composite share is paid **per composite**: share = 45% ÷ (number of
  composites in the gap). Each range miner receives share × (composites
  accepted from their range, i.e. the accepted tail, per D4).
- The remainder of the integer division goes to the prime finder.
- Example, gap 1130–1150 (21 composites), prime 1151: share = 450,000 / 21 =
  21,428, remainder 12. Y (11 composites) gets 235,708; Z (10 composites)
  gets 214,280; the prime finder gets 450,012; the validator pool gets
  100,000.

### Fixed by earlier work

- **Consensus:** two-phase (prevote/precommit) rounds, lock on a quorum of
  prevotes, justified unlock, any quorum of signers, rotating proposer,
  persist-before-send, quorum `ceil(2n/3)`. See `round-change-audit.md` and
  the two model tests.
- **The engine:** one in-memory state with incremental updates, never rebuilt
  per request. See `v1-baseline-measurements.md`.
- **Integers:** the main chain uses uint64; GMP is used only in certificates.
- **No lottery.**

## Open (A1)

- Block header fields: Merkle roots (ranges, transactions, state),
  `extension_root`, reserved privacy roots, policy and validator-epoch
  digests.
- Signing domains: network ID, epoch, position, round, phase.
- Timeouts: base value and growth per round.
- Versioned record and transaction types.
- The fault-model statement: three validators are crash-tolerant; four
  independent validators are needed for one Byzantine fault.
