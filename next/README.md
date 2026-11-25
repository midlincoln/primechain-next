# PrimeChain Next engine

The core engine for PrimeChain Next, built from `docs/main-chain-design.md`
(decisions D1–D19). This tree is separate from the inherited v1 code and from
`experiments/`.

## Modules

| File | Contents |
|---|---|
| `numbers.hpp/.cpp` | `Integer` (GMP, arbitrary precision) and `Amount` (unsigned 128-bit, checked add/sub/mul) — D14 |
| `codec.hpp/.cpp` | Canonical strict codec: minimal LEB128 varints, minimal length-prefixed integers with a size bound, length-prefixed bytes and text; decoders reject truncation, non-minimal forms, oversize values and trailing bytes |
| `merkle.hpp/.cpp` | SHA3-256 Merkle tree, RFC 6962 shape (no leaf duplication); leaf/node/empty domain bytes 0x00/0x01/0x02 |
| `model.hpp/.cpp` | Range and prime submissions with domain-separated signing payloads, accepted ranges, block header (D9) and block; versioned encodings (D12) |
| `store.hpp/.cpp` | D19 persistence: append-only block file with checksummed frames, derived height index, atomic state snapshots |
| `engine.hpp/.cpp` | In-memory engine: genesis {2, 3} (D7); range validation and competition (D1, D2, D4); Pratt-checked closing prime (D5); per-composite rewards (D8); block sealing; full replay; governance tally (A3-2); hooks to propose, check and apply consensus values |
| `consensus.hpp/.cpp` | A3.1 consensus core: per-instance (h, k) two-phase rounds, certificates, retention, proposer skip, equivocation evidence, durable state for restart |

## What A2.1 covers

- One authoritative in-memory state, updated incrementally and never
  reloaded per request.
- Range rules (D4): the highest end wins, ties go to the earliest arrival in this node's queue (the round proposer's choice; the vote makes it final),
  queued batches continue with their tail, and stale batches are dropped.
  Losers receive nothing.
- Closing prime: the gap must be covered; the Pratt witness is checked
  against the factorization of p − 1, derived from the divisors already
  recorded on chain.
- Rewards: `reward(p)`, currently 1,000,000 micro-units per prime, split 50% to
  the finder and 50% per composite. Each prime is its
  own asset.
- Replay from genesis re-verifies every block and must match bit for bit.
- Miner keys are registered once (`new_keys` in the block where a miner is
  first accepted). Miner signatures are journal data; blocks keep the
  evidence, the attribution and the hash of each full signed submission (D15).
  A chain to 19,997 takes 1.30 MB, 576 bytes per block.
- Header (D9) includes the reserved `extension_root`, `privacy_root` and
  `finance_root`.

Run the tests with `ctest -R '^next_'` (codec, engine rules, and a generated
chain to 20,000 with ML-DSA signatures followed by a replay).

## A2.2 (D16–D19)

- **Transfers (D16):** inputs and outputs per asset (strictly increasing
  input assets), several outputs per transfer (bulk send, up to
  `Policy::max_outputs`), a fee of at least `Policy::min_fee_micro_units`
  into the validator fee pool, and a per-sender nonce that is exactly the
  previous one plus 1 (first nonce 1). The key is optional once registered.
  **User transfers keep their signatures in blocks** (replay must re-verify
  balances), unlike miner submissions (D15). `transactions_root` is the
  Merkle root over the encoded transfers.
- **Automatic distributions (D17):** when the block being sealed has a height
  that is a multiple of `Policy::epoch_length`, the fee pool (per asset) is
  paid out to the validators' payout addresses in equal shares. The
  remainder goes one micro-unit each to the first validators in
  payout-address order. With no validators configured, the pool keeps
  accumulating. There is no validator share of block rewards (D8).
- **Validator records (D18):** `Genesis::validators`. Each record binds a
  consensus key to a payout address and is signed by both keys; invalid
  bindings are refused. The validator set's digest is in every header
  (`validator_epoch_digest`) and in the state root.
- **Persistence (D19):** `Engine::open(dir, policy, genesis)` loads the latest
  snapshot and replays only the blocks after it. Every sealed block is
  appended and fsynced (fail-stop on error); every `Policy::snapshot_interval`
  blocks the full state is snapshotted atomically. An incomplete final frame
  (an interrupted append) is truncated away. A damaged complete frame, a
  damaged snapshot, or a policy or validator-set mismatch refuses to open.

### Choices made in A2.2 that the design left open (for review)

1. **Mempool:** submission checks form, signature, caps, the fee minimum, and
   that the nonce is ahead of the sender's last one. Balances and exact
   nonce order are checked at inclusion; transfers failing there are
   dropped, not kept for later.
2. **Order within a block:** transfers (in submission order), then mining
   rewards (D8), then the epoch distribution (D17). Transfers cannot spend
   the same block's rewards.
3. **Asset existence:** an asset must be a recorded prime (2 and 3 exist
   but hold no units).
4. **Distribution order and remainder:** validators sorted by payout address
   (ties by key hash); remainder one unit each to the first ones; no
   validators means no distribution.
5. **Defaults:** `min_fee` 1, `max_outputs` 1000, `epoch_length` 100,
   `snapshot_interval` 1000. The policy digest covers the first three
   (`snapshot_interval` is local).
6. **Persistence layout:** frame = 8-byte length, block, SHA3-256; one
   `snapshot.dat` (latest only); the index is derived and rebuilt when
   inconsistent; snapshots refuse to load under a different policy or
   validator set. After `open`, `blocks()` holds only the blocks since the
   snapshot; the full history is on disk.
7. **Copies of an opened engine share its store** (tests copy only in-memory
   engines).

Generated chain to 19,997 with 45 ten-output bulk transfers: 1.48 MB
(654 bytes per block), replay identical, peak memory about 30 MB.

## A3.1: consensus core (in-process)

`consensus::Node` runs one validator's consensus over its own `Engine`.
Messages are plain values handed to `receive()` and returned as `Outgoing`;
there is no networking yet (A3.2). The rules are Tendermint's Algorithm 1, as
in `experiments/prime_blocks/two_phase_model_tests.cpp`: prevote on the
round's proposal (or on a re-proposal justified by a polka at its valid
round), lock on a polka, carry the valid value forward, precommit nil on a
nil polka or timeout, decide on a precommit quorum in any round, and skip to
a higher round on n − q + 1 messages from it. q = ceil(2n/3).

- **Values:** a range batch (the proposer's highest-end pending batch, ties by
  its own queue), or the closing prime plus transfers and governance
  transactions (built by a dry run of block sealing). Every validator checks a
  value objectively against its own engine (`validateRange`,
  `checkPrimeValue`) and applies a decided one with `applyDecidedRange` /
  `applyDecidedPrime`, so engines stay bit-identical. A prime value carries
  the hash of the block header it seals; validators recompute it and reject
  a mismatch, so a prime decision's certificate certifies the block header
  (F7). Nil is a vote, never a decision.
- **Votes and proposals** sign the D10 domain: tag, network ID, validator-set
  digest, (h, k, r), phase, value hash or nil (ML-DSA with the consensus key).
- **Certificates:** {h, k, r, value hash, signer bitmap, precommit signatures
  in validator order}, verified against the set active at that instance
  (`consensus::verifyCertificate`). Retention: permanent at every K-th block
  and on both sides of a validator-set change; all certificates in a window
  of the last W blocks; decided values only in the window.
- **Proposer:** (h + k + r) mod |candidates| over the active set minus
  validators with ≥ M missed proposer turns (M = `Policy::proposer_skip_after`,
  3); nobody is skipped if everybody would be.
- **Governance (A3-2):** `GovernanceTx` votes and "ready" messages ride in
  prime decisions after the transfers and are tallied on chain; ready
  entries of proposals that lapse without passing are pruned at expiry. See
  the design notes for the exact rules.
- **Restart:** `Node::durable()` is the state to persist before sending
  (decisions of the open block, certificates, skip counters, lock, valid
  value, round, and every vote and proposal already signed, which are
  re-sent, never re-signed); `Node::restore` rebuilds from it plus the engine
  reopened from its Store.
- **Evidence:** two different validly signed votes from one signer at one
  (h, k, r, phase) give an `Evidence` record, kept in memory.

Tests: `ctest -R next_consensus_` (a simulated network with drop, delay,
reorder, partitions, crash and restart, a logical clock, and gossiped
submissions; see `next/tests/consensus_tests.cpp`).

Results (1 vCPU shared with a live validator, `nice -n 19`):

| Test | What it shows |
|---|---|
| `next_consensus_chain` | n = 3, ML-DSA votes: height 2,000 (frontier 17,389), engines and blocks identical, 8,646 certificates verified, one gossiped transfer included once; 735 s wall |
| `next_consensus_crash` | one of three down for 60 blocks: with skip (M = 3) 12 timeouts and none in the last 30 blocks, 22 s logical; without skip 160 timeouts, 97 s logical |
| `next_consensus_g2` | the G2 scenario (lost proposal, C crashes) decides in round ≥ 1 without C, for three proposer rotations |
| `next_consensus_partition` | n = 4, 20% loss: a 2\|2 split decides nothing, a 3\|1 split continues, the isolated node restarts from its Store and catches up |
| `next_consensus_byzantine4` | n = 4 with one equivocator: no conflicting decisions, evidence at honest nodes |
| `next_consensus_byzantine3` | n = 3 with one equivocator: the known fork happens; evidence extracted from the two certificates |
| `next_consensus_governance` | lapse; fee 1 → 7 after notice; add a validator with its ready message (it then signs); remove it; an add without ready never activates; a lapsed proposal's ready entry is pruned on every node; certificates verified across set changes; restart from a snapshot keeps governance state |
| `next_consensus_header` | n = 4, a Byzantine proposer forges the header hash in its prime values: the engine rejects them, they are never decided, and every certified header hash matches the sealed block |
| `next_consensus_retention` | K = 10, W = 5: certificates kept exactly at checkpoints and in the window |
| `next_consensus_fuzz` | 20 seeds, n = 3–5, loss up to 29%, delays up to 420 ms, 59 partitions, 37 crash/restarts, 3,636 instances: no conflicts, progress after healing |

Clean under AddressSanitizer and UndefinedBehaviorSanitizer.

## Deferred

- **Monetary layer:** reserves, loans, banks.
- **A3.2–A3.3:** networking, catch-up from the block file, a validator daemon.
- **B3:** `extension_root` (currently zero) and the rings layer.
- An incremental state Merkle tree. `state_root` currently hashes one cached
  digest per asset.

## License

The code in this directory (`next/`) is released under the MIT license
(`LICENSE`). Contributions to it are accepted under the same license.
