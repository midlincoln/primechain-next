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

- Where transactions go (which decisions carry them) and when a transfer is
  final.
- Block header fields: Merkle roots (ranges, transactions, state),
  `extension_root`, reserved privacy roots, policy and validator-epoch
  digests.
- Signing domains: network ID, epoch, position, round, phase.
- Timeouts: base value and growth per round.
- Reward as a function of the prime (a constant at launch), and how range
  shares are computed.
- Versioned record and transaction types.
- The fault-model statement: three validators are crash-tolerant; four
  independent validators are needed for one Byzantine fault.
