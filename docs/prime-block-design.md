# Prime-ended blocks: first implementation milestone

Status: design direction and offline experiment, not an activated protocol.
Baseline: inherited live-chain source at `f7138dd`. All implementation work is
in PrimeChain Next; the old repository, wallets, services, and chain data are
not migration inputs to this experiment.

## Scope

Keep integer mining and its cooperative reward model. One finalized block ends
at a prime and contains all composite classifications after the preceding prime.
Each accepted range credits its submitting miner for every member. Multiple
miners can contribute separate ranges to the same block. Reward shares remain
per composite, not per range or per wallet.

The revised agreement replaces per-integer voting with one quorum decision per
contiguous composite range. Every member must have valid evidence; acceptance
is all-or-nothing. Keep validator admission/removal, reserve and fee rules,
transactions, and reward proportions unless a specific adaptation is reviewed.
Integer position, range position, validator round, and prime-block height become
distinct concepts. No polynomial mining, general evidence registry, inflation
multiplier, or snapshot/swap mechanism is implemented in this milestone.

## Permanent contents

The eventual block must commit to:

- A format/network identifier, prime-block height, and previous block identity.
- The previous closing prime and the new closing prime with its certificate.
- Every intervening composite's divisor evidence and credited address.
- Transactions and protocol events, with their original integer positions and
  ordering preserved wherever those positions affect validation or activation.
- The information needed to reconstruct reward and validator participation state.
- A block-level quorum certificate covering the entire content and attribution.

For a block following prime 7 and ending at 11, entries 8, 9, and 10 must each
have valid composite proofs. The prime proof for 11 plus complete coverage shows
that no earlier prime was skipped. Reject missing, duplicate, unordered, invalid,
or out-of-interval entries. Genesis at 2 has no composite entries. The prime 3
also closes an empty composite interval; preserve the existing empty-pool rule.

One miner signature and one quorum certificate authenticate each range. There
are no individual composite signatures or votes in this revised proposal. Range
signatures are journal data, not permanent block payloads. Their removal does not erase divisor evidence
or attribution. Later readers can verify the arithmetic and the block quorum's
attestation, but cannot audit discarded individual signatures. A digest alone
would not provide replayable divisor evidence or recover those signatures.

Transaction signatures and independently required governance signatures are not
automatically removed by this proposal.

## Open interval and checkpoint safety

Each validator needs durable accepted classifications, attribution, and signing
locks while the block is open. A RAM-only buffer is insufficient. The finalized
prime frontier and accepted/open integer frontier must be separately reported.

Before pruning intermediate signatures, the protocol must define how a block
certificate is agreed, persisted, synchronized, and recovered after interruption.
A compact block signature is not a retroactive reinterpretation of old votes:
validators must sign a commitment covering the new complete interval. Persist
the block/certificate and recovery checkpoint before retiring its journal entries.
Retain locks needed to prevent conflicting votes across restarts.

Unresolved before production network integration:

- The offline prototype uses a durable quorum-certified range as the receipt.
  Define receipt publication/availability and multi-round recovery for the network.
- How does a recovering validator learn and validate the still-open interval?
- Can transactions be considered final before closure, and under which retained
  certificate? Do not silently change current finality semantics.
- Which validator set signs the closing block if membership changes inside it?
- How are intermediate participation credits preserved without retaining all
  votes? Existing fee/reward recipient calculations depend on participation.
- What byte, integer-size, and verification-work bounds prevent an oversized
  interval from blocking progress? Do not truncate proofs or skip composites to
  fit a cap.

## Replay, indexing, and rewards

Range size is a shared policy variable, provisionally 256 members in the
experiment. The parser safety ceiling is a separate resource bound. All
validators must agree on the active policy; it cannot change during an open
interval. Future economic-policy votes should activate at a prime-block boundary
and be replayable from committed history. That activation machinery is not yet
implemented. Range limits do not cap the total composite count of a prime block.

Replay validates the block linkage and coverage, mathematical proofs, certificate,
and ordered state transitions. It must produce the same balances, supply per
prime asset, nonces, validator state, policies, reserves, and distribution
participants as the corresponding accepted integer sequence.

Keep an integer-to-proof lookup independent of record storage. A rebuildable
index can locate the containing block and entry. A closing prime's `p - 1`
factorization often needs evidence from the open interval itself, so proof
construction reads validated pending entries as well as finalized history.

The current fixed-validator reward rule is 1,000,000 micro-units per prime:
450,000 to the prime discoverer, 450,000 divided equally per composite entry,
and 100,000 to the validator reward pool. Integer division remainder goes to
the prime miner; an empty interval gives the prime miner both discovery shares.
Genesis is a special case. Preserve repeated provider entries, rather than
deduplicating addresses and accidentally rewarding per wallet instead of per
composite. Issuance reduction is a separate economic proposal.

## Existing implementation touchpoints

- `include/primechain/protocol/records.hpp`, `src/protocol/records.cpp`: format,
  hashes, signatures, transactions, and embedded governance events.
- `src/node/sequential_node.cpp`: per-integer validation, rewards, snapshots,
  participation tracking, and `frontier == height + 2` assumptions.
- `src/storage/record_store.cpp`: integer-indexed records and contiguous ranges.
- `src/storage/finalization_store.cpp`, phase/round-change stores: durable locks
  and recovery; these cannot simply be dropped when introducing blocks.
- `src/node/sync_server.cpp`, `src/tools/sync_download.cpp`: per-integer sync,
  submission routing, proof indexes, and activation coordinates.
- `src/miner/frontier_main.cpp`, `src/tools/client.cpp`: open frontier, batching,
  proof lookup, confirmations, and derived indexes.
- `src/math/number_theory.cpp`: proof construction and verification. The current
  verifier trial-divides prime factors rather than looking up their certificates.

## Staged delivery

1. Establish inherited test baselines and implement an opt-in offline fixture.
2. Agree on event ordering, attribution receipts, checkpoint safety, and bounds.
3. Choose larger integer types and a canonical bounded encoding; audit modular
   arithmetic, parsing, hashes, and proof verification independently of the fixture.
4. Implement durable open intervals and prime-block finalization with recovery tests.
5. Add atomic range transport and range-level voting, with exact retry and overlap
   rules. No partial acceptance and no per-composite voting.
6. Run full protocol regression and failure testing, then profile coordination.

The prototype in `experiments/prime_blocks` intentionally uses distinct
magic tags and remains outside inherited node/network paths. It tests arithmetic,
fixed-validator rewards, signature compaction, proof replay, configurable range
limits, and single-round range/closing vote locks. An isolated message interface
is exercised over localhost TCP, with a test coordinator rather than automatic
peer orchestration. See `experiments/prime_blocks/atomic-ranges.md` for its
recovery and checkpoint boundaries. The coordinator documented in
`experiments/prime_blocks/continuous-chain.md` now installs successive compact
blocks and opens new intervals with crash-recovery tests. Journals are retained.
`experiments/prime_blocks/local-validators.md` connects the coordinator to the
message handler in three localhost worker processes; the test coordinator still
drives quorum collection and catch-up. The follow-up
`experiments/prime_blocks/peer-coordination.md` adds autonomous quorum collection
and durable sender-driven delivery retries inside those workers. It retains
fixed-set, single-round assumptions; conflicting proposals can stall, and general
peer-pull sync and authenticated transport are not implemented. This is not a live-chain converter,
production daemon, or complete network consensus implementation.
