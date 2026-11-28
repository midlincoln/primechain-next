# B3 work plan: quadratic rings in the engine (2026-11-25)

Rules: `rings-design.md` (R1–R13), `ring-records.md`, `protocol-parameters.md` (D20).
**Agreed to start 2026-11-26** (founder: "do a, b and c"); the step list is the plan, the
small rules below are still proposals.
Quadratic rings only (certificate type 1). Engine code in `next/`; no networking yet
(A3.2). Each step is built by one worker, reviewed, shown to the founder, then pushed.
No chain is live, so formats may still change freely.

| Step | What | Done when |
|---|---|---|
| **B3.1 Parameter table** (started 2026-11-26) | all engine numbers in one table in the state, with history; one kind of vote (parameter, old → new, height); replaces the fee-only vote | existing tests pass; every parameter can be voted, takes effect after notice, is never retroactive; history survives restart and replay |
| **B3.2 Ring mathematics** | the certificate verifier moved into `next/`, plus the unit check, the size limit and a segment checker (R7, R9) | all 351 certificates and 2,424 entries pass; every corrupted variant is rejected; padded generators are rejected; segment ends match the measured runs |
| **B3.3 Assets and awards** | asset (ring, q, i); ownership history; computed awards (R13) | the Alice and Ovanes cases, refused spending, restart and replay of `spend_test.py`, with the "pay everyone at once" ledger alongside, equal to the last unit |
| **B3.4 Opening rings** | commit, reveal, deposit, gate 1, payee, registry | races (A commits first, B reveals first), expiry, copied reveals, loser's refund, certificates too large |
| **B3.5 Mining rings** | signed work, batches, rings file with disk index, issuance (rule A4, with backlog), `extension_root` | wrong rows, gaps, wrong ends, wrong miner credit and wrong roots are rejected; issuance matches the simulation; replay from the two files is bit-identical |
| **B3.6 Consensus** (built 2026-11-28, for review) | ring batch as third kind of decision | a prime closes with a missing, late or invalid batch; crash, partition, Byzantine and fuzz tests with rings; pausing one ring or all |
| **B3.7 Measurement** | chain to 20,000 with many rings | memory, bytes per row, time per batch, look-up time; main chain no slower than agreed |

Order: B3.1 and B3.2 are independent; then B3.3, B3.4, B3.5, B3.6, B3.7.

## To settle inside the steps (proposed, not agreed)

1. **Backlog rule (proposed, built in B3.5):** a prime is older than a ring if its
   block came before the ring's opening block; it pays parameter 19 of v (rule A4).
   It depends on chain data only, so there is no race.
2. **Counting rings:** paused rings count as open (proposed), so a pause does not move
   money to the others.
3. **Integer weights:** not needed under rule A4 (they belonged to rule A3).
4. **Batch frequency:** at most one between two main decisions (R1); after a ring batch
   the proposer turns to main work first (proposed, built in B3.6). Also built as proposals in
   B3.6: still one batch per main block (B3.5); a ring batch leads only in round 0 of an
   instance, so a failed batch costs at most one timeout; in a batch, rows first, then reveals,
   then openings; the proposer puts reveals in first; an emergency pause is a quorum vote that
   names the hash of a fault report, takes effect from the next block and can only pause
   (`next/README.md`, B3.6).
5. **Main-chain balances** also get a history, so one rule serves all assets.

## Not in B3

Degrees above 2 and nested rings, networking, a miner client (A4), cash, banks, the
Merkle `state_root` (separate proposal), ranges for parameters (later model).

## Server

One build or test at a time, -j 1, under the memory cap. Full engine builds may need a
val1 pause; each is asked for separately.
