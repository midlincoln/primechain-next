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
| `engine.hpp/.cpp` | In-memory engine: genesis {2, 3} (D7); range validation and competition (D1, D2, D4); Pratt-checked closing prime (D5); per-composite rewards (D8); block sealing; full replay |

## What A2.1 covers

- One authoritative in-memory state, updated incrementally and never
  reloaded per request.
- Range rules (D4): the highest end wins, ties go to the earliest arrival in this node's queue (the round proposer's choice; the vote makes it final),
  queued batches continue with their tail, and stale batches are dropped.
  Losers receive nothing.
- Closing prime: the gap must be covered; the Pratt witness is checked
  against the factorization of p − 1, derived from the divisors already
  recorded on chain.
- Rewards: `reward(p)`, currently 1,000,000 micro-units per prime, split 45% to
  the finder, 45% per composite, 10% to the validator pool. Each prime is its
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
  that is a multiple of `Policy::epoch_length`, both pools (reward and fee,
  per asset) are paid out to the genesis validators' payout addresses in
  equal shares. The remainder goes one micro-unit each to the first
  validators in payout-address order. With no validators configured, pools
  keep accumulating.
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

## Deferred

- **Monetary layer:** reserves, loans, banks.
- **Validator changes:** adding or removing validators by quorum vote (A3).
- **A3:** two-phase consensus, block certificates, networking.
- **B3:** `extension_root` (currently zero) and the rings layer.
- An incremental state Merkle tree. `state_root` currently hashes one cached
  digest per asset.
