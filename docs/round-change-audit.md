# Inherited round-change mechanism versus PBFT

Status: audit draft, 2026-11-21. Changes no code.

This document reviews the finalization and round-change path inherited from the
live chain (protocol v3, rule `fixed-2-of-3-mldsa65-rounds-locks-v4`) against
Castro and Liskov, *Practical Byzantine Fault Tolerance* (OSDI 1999). The v2
whitepaper cites that paper as the model for this path. Tendermint is used only
as a reference for round-based locking. The goal is to decide what PrimeChain
Next keeps, fixes, or replaces before range and prime-block decisions run
through it.

Code references are to this repository, inherited at `f7138dd`:

- `src/core/consensus.cpp`: `requiredValidatorQuorum`
- `src/node/sync_server.cpp`: `finalizationCommitteeOrdered`,
  `makeLocalFinalizationVote`, `verifyRoundChangeVote`,
  `acceptVerifiedRoundChangeVote`, `activeFinalizationRound`, lock loading,
  and the timeout loop near "stalled for integer"
- `src/protocol/records.cpp`: `verifyRoundChangeCertificate`,
  `verifyRecordFinalization`
- `src/crypto/signature.cpp`: vote and round-change signing payloads

## The bound

PBFT tolerates `f` arbitrary (Byzantine) replicas when `n >= 3f + 1`, so
`f_max = floor((n - 1) / 3)`:

| n | Byzantine faults tolerated | Inherited quorum `ceil(2n/3)` |
|---:|---:|---:|
| 3 | 0 | 2 |
| 4 | 1 | 3 |
| 7 | 2 | 5 |
| 10 | 3 | 7 |

The inherited quorum formula is sound for every `n` against that `f`. Two
quorums overlap in more than `f` validators (`2q - n > f`), and a quorum
remains reachable with `f` validators silent (`q <= n - f`). Next should keep
the formula. With three validators, though, it buys tolerance of one *crash*,
not one *Byzantine* validator. Wording such as "Byzantine attack resistance"
applies only once there are at least four independent validators.

## How the inherited path works

For each position `(previous_record_hash, integer)` and round `r`:

1. **Committee.** A deterministic committee of exactly `q` validators is
   chosen. The rotation start comes from
   `sha3("...committee-rotation-v1:" || previous_hash || integer)` plus
   `(r - 1)`. The first member is the proposer.
2. **Proposal.** The proposer signs the candidate. The other committee members
   sign only a candidate that carries a valid proposer signature for that round.
3. **Lock.** The first time a validator signs a candidate at a position, it
   persists a safety lock (`finalization_locks_`, kept for 10,000 integers).
   It then refuses to sign any different candidate at that position in any
   round.
4. **Certificate.** The record is final when it carries signatures from exactly
   the assigned committee for its round.
5. **Round change.** After a fixed timeout (5,000 ms on the live network),
   validators sign `RoundChangeVoteV1(previous_hash, integer, new_round,
   locked_round, locked_candidate)`. Each validator signs at most one round
   change per `(integer, new_round)`. A quorum of round changes activates
   `new_round`. The new candidate must equal the highest-round lock reported
   in that certificate. Two different locks tied at the highest round are
   rejected.

## What matches PBFT and Tendermint

- **Deterministic proposer per round**, rotating with the round, as in a PBFT
  view change.
- **Quorum intersection carries a possible decision forward.** For crash
  faults at `n = 3`, any committee that finalized X and any later round-change
  quorum share at least one validator. That validator reports its lock on X,
  so the next round must re-propose X. By induction over rounds, no honest
  validator signs a conflicting decision. This is the core of the safety
  argument, and it holds under the crash-fault assumption.
- **A round change carries justification.** Locked candidate payloads travel
  inside the round-change certificate, so a later proposer's choice can be
  checked. This plays the role of PBFT's view-change `P` set.
- **Safety state is persisted before signing,** and it survives restarts.
- **Votes are domain separated by message type** (finalization, plain round
  change, locked round change).

## Gaps

### G1. One voting phase: signing *is* locking (high impact)

PBFT has pre-prepare, prepare and commit. Tendermint has proposal, prevote and
precommit. In both, a replica only becomes *locked* after it sees a quorum of
first-phase votes for a value (a "prepared certificate" or "polka"). The
inherited path has one phase, so a validator becomes locked as soon as it signs
once, even when no quorum ever formed.

Consequence: a lock is evidence that one validator signed, not that a quorum
could have decided. Locks therefore form much more easily than the protocol
needs, and the next gap turns them into stalls.

### G2. Locks are never released by later evidence (high impact, liveness)

A validator holding a lock on X@r1 refuses Y forever, even after a quorum
round-change certificate proves that Y@r2 is the justified value.
PBFT and Tendermint let a replica move to a value that carries a
higher-round quorum justification.

Example at `n = 3`: A signs X@1, but no certificate forms. In round 2, B and C
lock Y@2. If C then goes offline, A and B are both honest and online, but A
will never sign Y and B will never sign X, so the chain stops for good.
The Next conflict tests (`prime_block_conflict_boundaries`) reproduce this
class of permanent stall. It is the likeliest source of the "round stalled"
episodes in the live logs. That is not yet confirmed from the live data.

### G3. Fixed committee of exactly `q` per round (medium impact)

PBFT accepts *any* `2f + 1` matching votes. Here only the assigned `q`
validators may sign. For one round in three, at least, a single offline
validator is assigned to the committee and blocks it. Progress then waits for a
full round-change timeout. This costs latency, not safety. It also explains the
frequent "local validator is not assigned to this finalization committee"
warnings in the live log, which are expected behavior rather than faults.

### G4. Fixed timeout (medium impact, liveness)

The round timeout is constant (`finalization_timeout_ms_`). PBFT doubles the
view-change timeout on each successive view change. Tendermint increases it
per round. Without growth, a network whose message delay is consistently
greater than the timeout can change rounds forever.

### G5. Signing domain does not bind network, epoch or phase (medium impact)

`recordFinalizationVoteSigningPayload` covers `(tag, candidate_hash, round,
validator)`. The round-change payloads cover the position and lock. Neither
binds a network or chain identifier, a validator-epoch digest, or a vote phase.
The candidate hash commits to the previous record, which limits cross-chain
replay in practice. Even so, the same keys are now used on the v1 testnet and
in Next experiments, and Next adds a second vote phase. Next needs an explicit
domain: network ID, epoch or validator-set digest, policy digest, position,
round, and phase.

### G6. Equivocation is rejected but not kept as evidence (low now, needed for BFT)

A conflicting round-change vote, or tied highest locks, is refused with an
error and then discarded. PBFT itself does not punish faults. However, the
whitepaper's plans for removal or slashing need two signed, conflicting
messages from the same validator at the same position, round and phase,
persisted and shared between validators.

### G7. A lock is self-reported (meaningful only once `f >= 1`)

A round-change vote declares the sender's own lock, and nothing proves the
sender is not hiding a higher lock. At `n = 3, f = 0` this is irrelevant. With
`f >= 1`, PBFT relies on the quorum intersection containing at least one
*honest* validator that reports truthfully. That holds only with `n >= 4`.

## What Next should do

These recommendations apply to both range decisions and closing-prime
decisions. Each decision is ordered by its consensus position, as set out in
`proposal-recovery-design.md`.

1. **Keep** the `ceil(2n/3)` quorum, the deterministic rotating proposer, the
   persist-before-send rule, and justified round changes.
2. **Use two vote phases** (prevote and precommit). Lock only on a quorum of
   prevotes for the same value in the same round, and commit only on a quorum
   of precommits. *(Fixes G1.)*
3. **Allow a later justified unlock.** A validator locked on X@r may prevote Y
   in round r' > r only when the proposal carries a quorum prevote certificate
   for Y from a round r'' with r < r'' < r'. *(Fixes G2.)*
4. **Accept any quorum** of signers from the active set, not only an assigned
   committee. Keep a designated proposer per round. *(Fixes G3.)*
5. **Grow the timeout with the round,** for example `base * 2^(r-1)` up to a
   cap, and reset it after each decision. *(Fixes G4.)*
6. **Use a new signing domain** with distinct tags for proposal, prevote,
   precommit and round change. Each binds network ID, validator-epoch digest,
   policy digest, position, round, and value digest or nil. Do not relabel v1
   votes. *(Fixes G5.)*
7. **Persist equivocation evidence** as pairs of conflicting signed messages.
   Gossip it, and leave penalties to a later governance decision. *(Fixes G6.)*
8. **Operating mode.** The protocol is written for general `n` with
   `f = floor((n-1)/3)`. Three validators with 2-of-3 run as crash-tolerant
   only, and the documentation says so. A fourth independent validator
   (separate operator, host and keys) gives `f = 1` with no protocol change.
   *(Addresses G7.)*

Points 2 to 5 are, in substance, the Tendermint algorithm (Buchman, Kwon and
Milosevic, 2018, Algorithm 1). Before writing PrimeChain's own version, compare
an existing reviewed implementation against the gates in
`proposal-recovery-design.md`.

## Tests to add

Add these to the existing peer and conflict harnesses:

- **G2 repro:** A locks X@1, B and C lock Y@2, C goes offline. Today this
  stalls. After the change, A and B must decide.
- **No conflicting decisions:** for every combination of partition, delayed
  old-round vote and crash with `n = 3, f = 0`, no two honest validators decide
  different values.
- **Byzantine case at `n = 4, f = 1`:** one equivocating signer cannot produce
  two valid conflicting certificates.
- **Liveness under rising delay:** with message delay above the base timeout, a
  decision is still reached once the round timeout exceeds the delay.
- **Evidence:** a conflicting vote is persisted and can be checked by a third
  validator.
