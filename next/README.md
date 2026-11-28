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
| `parameters.hpp/.cpp` | D20 parameter table: ids, bounds, history, digest |
| `engine.hpp/.cpp` | In-memory engine: genesis {2, 3} (D7); range validation and competition (D1, D2, D4); Pratt-checked closing prime (D5); per-composite rewards (D8); block sealing; full replay; governance tally (A3-2); hooks to propose, check and apply consensus values |
| `registry.hpp/.cpp` | B3.4 ring registry: ring records and open commits, with their encodings |
| `engine_rings.cpp` | B3.4 engine part: RING_COMMIT, RING_REVEAL, deposits, gate 1, kappa, the opening rule, RING_PAYEE |
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

## B3.1: parameter table (D20)

`parameters.hpp/.cpp`: every number is a protocol parameter (38, ids 1–38; 37 and 38 added in B3.4), with placeholder
hard bounds in code and a history, so `parameterAt(id, h)` gives the value for any block.
Votes are one kind, `ParamChange` (id, old value, new value), with notice; a change applies
from its effective height and never retroactively. Headers carry the digest of the table in
force. Details: `docs/main-chain-design.md`, "B3.1 implementation notes".

Test: `ctest -R next_parameters` (every parameter voted and in force exactly after the notice;
refusals at submission, inclusion and activation; reward, finder share, fee and epoch changing
only from block H; replay and restart keep the table and its history).

**Not yet built or run (2026-11-26, low memory on the host):** the consensus changes (proposer
skip, timeouts and retention from the table) and the adapted `next_consensus_governance` test;
also `next_generated_chain`, `next_consensus_chain` and the ASan builds.

## B3.2: ring mathematics (quadratic rings)

`rings.hpp/.cpp`: pure functions, no engine state yet (registry and batches come in B3.4/B3.5).

- Certificate type 1: D, factorization of |D|, generators (q, c0, c1) for every non-inert
  q <= M_K, and for D > 0 a unit eps != +-1 (norm +-1, need not be fundamental). Checks:
  eligibility |D| <= F^t, size limits (CertificateBytes, CertificateIntegerBytes),
  fundamental D, Minkowski coverage, norm, smaller root, no padding.
- **Roots are not stored:** r = -c0/c1 mod q.
- **No padding (R10):** a generator is valid only if neither g*eps nor g/eps has a smaller
  height c0^2 + c1^2. The height is strictly convex along g*eps^k, so this local test picks
  the minimum (two choices only in a tie). Byte length was not used: it has plateaus, so a
  few factors of eps could pass unnoticed.
- Segments (R7, R9): ring ID, start prime, row types (2 bits each, zero padding bits), one
  (c0, c1) per paid row. The checker recomputes every row type, checks each generator and
  that the segment ends exactly where R7 says (SegmentPaidRows, SegmentInertRows), and
  returns the new assets (ring, q, i, e).
- Ring ID = hash(type, degree, D) (R4). Signed integers: a sign byte (never "negative
  zero") and the magnitude.

Tests (`ctest -R next_rings`, vectors in `experiments/extensions/quadratic_vectors_v2.txt`
from `gen_quadratic_vectors_v2.gp`): 351 certificates (342 real with a unit), 992 non-PIDs
refused, 6,520 corrupted certificates refused; 681 segments with 2,414 rows and 2,342 new
ring primes; 7,214 corrupted segments and 34,473 malformed encodings refused.

## B3.3: assets and awards (R13)

`assets.hpp/.cpp`: the asset ledger. An asset is (ring, q, i); main-chain prime p is (0, p, 0).
Ring awards are computed from the rows and never written:
owned(X, a) = due from the row + received − sent, where the upstream part counts what a held
at the end of block h − 1 (`ring-records.md`, section 4).

- **Ownership history** per asset and address: a list (block, net). A new entry replaces the
  last one unless the asset paid something (was below a row) after it; entries that change
  nothing are dropped. Rows of a block are recorded before anything moves in that block.
- **Main-chain balances use the same ledger** (b3-plan item 5): the Engine keeps no separate
  balance map; `Engine::assets()` exposes it, the snapshot and the per-asset state hashes come from
  it. Transfers still name main primes (`Integer`); the asset field becomes (ring, q, i) when rows
  reach the chain (B3.5). With no rows every history has one entry, so nothing grows.
- **Awards (R11, A4):** a ring prime with index e issues e·v (v = `RingPrimeValue`, parameter 14,
  100,000, fixed per ring at opening: `Engine::ringTermsAt(h)`); miner 50%, founder's payee at h
  15%, 35% upstream: holders of a ring prime keep half, the rest goes in equal parts to the primes
  below, a main-chain prime keeps all. The part reaching 2 or 3 goes to the validators active at
  h. Units in the fee pool earn nothing; rounding remainders are never issued.
- **Spending:** every debited asset must exist and be owned (awards included); refusals: unknown
  asset, amount, not enough.

Tests (`ctest -R next_assets`):

| Test | What it checks |
|---|---|
| `next_assets` | asset names and encoding; history replace/append; rows refused (unknown ring, q not on chain or too new, twice, after the block's transfers, missing or too new primes below, e = 0); awards (miner, founder, payee changes, pro-rata 35%, validators for 2 and 3 with remainder, pool earns nothing, two ring levels and two embeddings); refused spending; snapshot round trip, every truncation refused |
| `next_assets_alice` | the three Alice/Ovanes cases of `spend_test.py`, natively, under A4 (v = 100,000) and with v = 500,000: who gets the award, the seller left with nothing, 4 of 7 transfers refused in the block after the split, restart and replay, the pay-at-once ledger alongside |
| `next_assets_spend` | replays all 8 runs of `spend_test.py` (3 scenarios, 5 random seeds, 2,935 blocks, 354 restarts) from `experiments/nested_rings/spend_vectors.txt` (`export_spend_vectors.py`): 8,490 accepted and 11,404 refused transfers decided as in Python; 83,968 balances equal to Python's pay-at-once ledger (sha3 of all lines) and to a C++ one, to the last unit; rounding loss and total issue equal; quadratic row shapes checked against the Kronecker symbol |

All 20 `next_` tests pass on this branch (2026-11-28), including `next_generated_chain` (chain to
19,997, 655 bytes per block, peak 30 MB) and `next_consensus_chain` (575 s wall) with balances on
the ledger. Peak memory: compiling `assets.cpp` about 350 MB, the consensus tests about 470 MB;
`next_assets_spend` runs in 27 s at 31 MB.

Choices left open by the docs (proposed): parts to several primes below are equal with the
remainder to the first (as D17); a prime below must come from an earlier block than the row
("lower rings first"); no validators at h means the 2/3 part is not issued; a main prime with
nominal issue 0 (2 and 3) counts as having no holders; zero moves are not recorded; the ledger's
`spend` requires debits to equal credits plus fee per asset. The issuance backlog (parameter 19)
is left to B3.5: the ledger takes v·e per prime as given.

## B3.4: opening rings

Records (`model.hpp`): `RingCommit` (version, network, discoverer, commitment, deposit asset and
amount, nonce, optional key, signature), `RingReveal` (commit reference (height, index), salt,
certificate type, version, bytes; no signature), `RingPayee` (ring, new payee, nonce, signature of
the current payee). commitment = sha3("ring-commitment", network, discoverer, salt,
sha3("certificate", type, version, bytes)). Blocks carry `ring_commits`, `ring_payees` and
`ring_reveals`; commits and payee changes are leaves of `transactions_root`.

- **Commit:** accepted only while gate 1 (`RingGateOpen`) is open (refused on arrival when
  closed, nothing taken) and while fewer than kappa (`RingsPerEpoch`, new parameter 37) rings
  have opened in the current epoch. The deposit (`RingDeposit`, new parameter 38, 100,000; 0
  allowed) leaves the discoverer and is held by nobody (`AssetLedger::debit`).
- **Reveal:** first matched against an unexpired sealed commit (an unmatched reveal is refused
  at submission and makes a block invalid on replay, so it costs nothing). Then type and
  version, size (`CertificateBytes`), encoding, eligibility |D| <= F^t, the B3.2 verifier, and
  the registry. A matching but invalid reveal is included: the deposit goes to the fee pool and
  the commit is settled at once. A valid reveal is refunded in its block, also a loser's.
- **Opening (R5):** after the block's reveals, commits are walked in (block, index) order; the
  walk stops at the first sealed one. Each revealed commit before it either opens its ring (the
  earliest) or loses to an earlier one. A commit not revealed by block height + W (the reveal
  window in force at the commit) expires at the start of block height + W + 1, its deposit going
  to the fee pool; so a revealed ring opens at most W blocks after its own commit.
- **Ring record** (`RingEntry`): number, ring ID, type, version, certificate hash, D and unit,
  founder, payee history, terms (`ringTermsAt(h)`, fixed), opening block, the winning commit,
  frontier (0 = no row yet), tip (`ringGenesisHash` until B3.5 writes ring blocks), pause state
  (a field only). Opening calls `AssetLedger::openRing`, a payee change `AssetLedger::setPayee`.
- **State:** the registry is in the snapshot; `extension_root` = Merkle root over the ring
  records by number, the open commits in order, then the block's reveals. `lastRingOutcomes()`
  lists what happened to each commit in the last block (refunded, forfeited with the reason,
  expired, opened, lost); it is not state.
- **Primality of the factors of D:** exact from chain data. A factor at or below the frontier is
  on chain; one above it is at most F^2 (eligibility), so trial division by the chain's primes
  up to its square root decides. No Pratt certificates are needed.
- **Consensus:** prime values carry the ring lists (hashed in `consensus::encode(Value)`), so
  `buildPrimeContents`/`checkPrimeValue`/`applyDecidedPrime` include them.

Choices left open by the docs (proposed): one nonce sequence per address for transfers, ring
commits and payee changes; the deposit is exactly the parameter, in any main-chain prime; a commit
is revealed in a later block, never its own; the salt is 32 bytes; order in a block: transfers,
ring commits, ring payee changes, governance, then expiry, reveals, openings, then rewards;
eligibility uses the frontier at the end of the previous block; a reveal of a ring that was open
before the commit's block forfeits, one opened in or after it is a refunded loser; kappa counts
openings, so the commits already in flight can push an epoch past it; kappa starts at 100 per
epoch (the doc leaves it to be set); a payee change applies from its block, the last in a block
counts; the ring frontier starts at 0 and the tip at a genesis hash. Not built: "blocks under a
pause of all rings do not count toward the 10" (pausing comes with B3.6).

**How B3.5 and B3.6 plug in.** B3.5 reads D and the unit from `RingEntry` for `checkSegment`,
advances `frontier` and `tip` as ring blocks are accepted, and records rows in the ledger.
B3.6 moves `ring_reveals` from the main block into ring batches: the batch handler calls the
same `applyRingReveal` then `openRings` for the batch's open height (expiry stays at main
blocks), and `extension_root` takes the batch's ring blocks. Pausing sets `RingEntry::paused`
and the R5 window rule for a pause of all rings goes into `expireCommits`.

Tests (`ctest -R next_ring_open`):

| Test | What it checks |
|---|---|
| `next_ring_open_race` | the ring-records example (A commits at 100, B at 101 and reveals at 102, A at 105: ring opens for A at 105, B refunded at 102); B alone opens at once; block order within one block; expiry at 111 (deposit to the pool, B's ring opens at 101 + 10, a late reveal refused, later commits unblocked); a reveal in the last block of the window; a loser that reveals after the opening is refunded; the record fields and the ledger wiring; replay |
| `next_ring_open_rules` | copied reveals (wrong salt, wrong certificate, missing index, another's secret under one's own commit, a second reveal, a copy committed after the reveal loses and is refunded); a block with an unmatched reveal fails replay; invalid certificates (wrong generator, non-PID, garbage), too large (limit 1,024 bytes), duplicate ring: deposits to the pool with the reason, no delay for later commits; eligibility at F = 5 (-43 refused, -7 and 17 open, factors above F by trial division); deposit 0 by an address with no units; insufficient funds and a wrong deposit not included, and invalid on replay; gate 1 closed; kappa = 1 per epoch |
| `next_ring_open_payee` | RING_PAYEE only by the current payee, unknown ring, payee history in registry and ledger, the old payee refused (and invalid on replay), stale nonce, replay |
| `next_ring_open_governance` | gate 1 closed by vote: new commits refused, a commit made while open still opens after (R2) |
| `next_ring_open_persistence` | restart with snapshots every 3 blocks while commits are sealed and revealed; open, payee change and expiry after restart; second restart; replay from the block file; codec round trip; identical tip, state root, registry and ledger |

## Deferred

- **Monetary layer:** reserves, loans, banks.
- **A3.2–A3.3:** networking, catch-up from the block file, a validator daemon.
- **B3:** mining rings (B3.5), ring batches and pausing (B3.6).
- An incremental state Merkle tree. `state_root` currently hashes one cached
  digest per asset.

## License

The code in this directory (`next/`) is released under the MIT license
(`LICENSE`). Contributions to it are accepted under the same license.
