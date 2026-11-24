# PrimeChain Next engine

The core engine for PrimeChain Next, built from `docs/main-chain-design.md`
(decisions D1–D15). This tree is separate from the inherited v1 code and from
`experiments/`.

## Modules

| File | Contents |
|---|---|
| `numbers.hpp/.cpp` | `Integer` (GMP, arbitrary precision) and `Amount` (unsigned 128-bit, checked add/sub/mul) — D14 |
| `codec.hpp/.cpp` | Canonical strict codec: minimal LEB128 varints, minimal length-prefixed integers with a size bound, length-prefixed bytes and text; decoders reject truncation, non-minimal forms, oversize values and trailing bytes |
| `merkle.hpp/.cpp` | SHA3-256 Merkle tree, RFC 6962 shape (no leaf duplication); leaf/node/empty domain bytes 0x00/0x01/0x02 |
| `model.hpp/.cpp` | Range and prime submissions with domain-separated signing payloads, accepted ranges, block header (D9) and block; versioned encodings (D12) |
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

## Deferred

- **A2.2:** transactions, fees, reserves, validator registration with consensus
  and payout keys (D10), and disk persistence with a block index.
- **A3:** two-phase consensus, block certificates, networking.
- **B3:** `extension_root` (currently zero) and the rings layer.
- An incremental state Merkle tree. `state_root` currently hashes one cached
  digest per asset.
