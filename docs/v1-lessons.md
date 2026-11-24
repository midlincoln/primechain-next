# Lessons from running v1

Status: running list, started 2026-11-24. Each entry pairs a problem seen on
the live testnet with how PrimeChain Next is designed to avoid it. Sources:
the v1 commit history, the baseline measurements
(`v1-baseline-measurements.md`), and the founder's operating experience.
Add new problems as they come up.

| # | v1 problem | Evidence | Next's answer |
|---|---|---|---|
| 1 | **State rebuilt per request:** 1.3–2.5 s of CPU per query, constant ~83% CPU, memory churn, OOM kills | measurements; OOM on val1 (2026-11-21) and val2 (2026-11-10) | one in-memory state with incremental updates; snapshot plus tail replay only at startup (D19) |
| 2 | **Governance votes tied to a chain position**, which expire as the chain moves, so maintenance mode was needed to collect votes | founder; `vote_previous_record_hash` in `ValidatorEpochTransitionV1` | position-independent votes, tallied on chain one by one, effective after notice (A3-2) |
| 3 | **Composite lottery:** extra rounds, no-winner rounds, a stuck frontier | "Disable composite lottery from stuck frontier" (2026-09-27) | no lottery; the highest end wins (D4) |
| 4 | **Commit-phase races, reveal retries, phase timeouts,** and miners waiting for "race winners" | many July commits (commit phase, reveal retries, round-numbered recovery) | no per-integer commit–reveal; range batches settled by the proposer plus vote, with no timestamps (D4). Commit–reveal is used only for ring registrations (R5) |
| 5 | **Permanent signing locks** that stall the chain while one validator is down (G2) | `round-change-audit.md`, model test | two-phase rounds, locking only on a quorum, with justified unlock (A3) |
| 6 | **Rewards and distributions needed ops scripts** and careful replay fixes | "Stabilize validator reward distribution replay", "Allow historical validator reward pool replay"; nohup distribution runs | automatic per-epoch distributions computed by the protocol and checked in replay (D17) |
| 7 | **Peer sync fragility:** head-of-line blocking, stalled socket writes, rollback-unsafe downloads, temporary replay pressure, quarantined peers | August–September commits (sync timeouts, rollback-safe downloads, concurrency limits) | A3: certified blocks only, fetched by hash and verified before applying; bounded per-peer queues and timeouts from the start |
| 8 | **Mempool interfering with chain sync** | "Isolate mempool from peer chain sync", "Avoid empty mempool replay pressure" | transfers are validated cheaply on arrival and applied only when a block closes (A2.2); the mempool never touches chain state |
| 9 | **Connection limits hit by validators** | "Raise/Bypass peer connection limits for validators" | A3: validators use dedicated authenticated peer links, separate from public client traffic |
| 10 | **Fragile caches and sidecar files:** corrupt sidecars, cache recovery commands | "Recover corrupt sync server sidecar caches", "Add chain recovery command" | checksummed files that refuse to open when corrupt, with no silent recreation (D19); one store, no sidecars |
| 11 | **Large records:** ~20–25 KB per integer | v2 paper; measurements | one block per prime, keys stored once, miner signatures in the journal: ~650 bytes per block (D15) |
| 12 | **One identity both signs and receives rewards** | v1 distribution code | two keys per validator: hot consensus key, cold payout key (D10) |

## To add

Further problems from the founder's experience (networking, mining
clients, operations, upgrades, …).
