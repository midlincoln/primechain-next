# Offline prime-block experiment

This is an opt-in, bounded experiment, not a new consensus implementation.
It does not read or alter the old chain and cannot be served by the node.

The follow-up [atomic-range experiment](atomic-ranges.md) adds one vote per range,
durable receipts, conflict locks, and restart tests. The comparison generator
below still builds legacy per-integer records as its storage/reward baseline.

## Run

From the PrimeChain Next repository:

```sh
bash scripts/check-repo-purpose.sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPRIMECHAIN_BUILD_EXPERIMENTS=ON
cmake --build build -j 2
ctest --test-dir build --output-on-failure -R '^prime_block_experiment_'
build/primechain-prime-block-experiment 100 build/prime-block-experiments
```

Every invocation creates a fresh private `run-XXXXXX` directory under the given
artifact root. It prints the exact path. To replay the compact file in another
process, use that path:

```sh
build/primechain-prime-block-experiment --replay build/prime-block-experiments/run-XXXXXX
```

`validators.txt` supplies the fixture's externally trusted validator identities.
It is test configuration, not a production trust-discovery mechanism. Generated
private keys exist only in process memory and are not written to disk.

## What it checks

- Generate a legacy per-integer chain accepted by the inherited `SequentialNode`,
  using fresh ML-DSA-65 miner identities and two votes from three validators.
- Generate prime-ended blocks containing the same proofs and credited addresses.
  Sign their complete new contents; never reuse legacy votes for changed bytes.
- Retain the closing prime's miner authentication and block quorum signatures,
  but no per-composite signatures in the compact encoding.
- Decode and replay the compact file from scratch, rebuild the composite index,
  and reconstruct Pratt proofs for every finalized prime.
- Compare all fixture accounts and per-prime supply against a full legacy replay,
  including repeated providers, empty intervals, and rounding (the 150 fixture).
- Reject invalid linkage, omitted/duplicate/reordered composites, wrong divisors,
  bad prime proofs, attribution tampering, unauthorized/duplicate/insufficient
  signers, bad signatures, duplicate blocks, malformed counts, and truncated data.

The tests cover limits 2, 3, 100, and 150. At limit 100 the last finalized prime
is 97: 25 prime blocks contain 71 composite entries. Integers 98, 99, and 100
remain pending, not silently inserted into the block ending at 97. Their old
signed records contribute only to the separately reported pending-inclusive size.

## Experimental encoding

All integers use the inherited uint64 type and fixed little-endian 8-byte words.
Strings/blobs have an 8-byte length prefix. The block payload contains:

```text
magic = PCNXPB01
height, previous_hash[32], previous_prime
prime, witness, provider_address, packed_prime_authentication
factor_count, (factor_prime, exponent)[]
composite_count, (integer, divisor, cofactor, provider_address)[]
vote_count, (address, public_key, target_hash[32], round, signature)[]
```

Each disk frame prefixes the payload with its byte length. The signed content
hash excludes the vote count and votes and includes the distinct magic tag.
Parent links use that content hash, not a hash that changes with the quorum
subset. The experiment supports only fixed three-validator, round-one finality.

Composite integers/cofactors are stored explicitly for clarity. Address tables,
implicit integer positions, and derived cofactors are possible later compactions,
not assumptions behind the measured results.

## Measurement boundaries

`legacy_finalized_prefix_bytes` and `compact_finalized_bytes` both cover the same
closed prefix through the last prime, including each format's framing. Validator
configuration bytes are reported separately for the compact fixture. Indexes,
snapshots, journals, and filesystem overhead are excluded. Both use real signatures,
not padded estimates. The synthetic fixture has no transaction/governance traffic;
its savings are not a forecast for a live network.

Elapsed time includes fixture generation, signatures, negative tests, and two
replays. It is not a consensus throughput or mining-difficulty benchmark.

### First measurement (2026-11-14)

Release build, limit 100, two ML-DSA-65 validator votes per record/block, and
signed miner submissions:

| Measurement | Result |
| --- | ---: |
| Finalized integer prefix | 2 through 97 |
| Legacy records in that prefix | 96 |
| Prime blocks | 25 |
| Embedded composites | 71 |
| Pending composites outside finalized prefix | 3 |
| Legacy finalized prefix, bytes | 1,569,388 |
| Compact finalized file, bytes | 405,286 |
| External compact fixture validator configuration, bytes | 141 |
| Finalized-file reduction | 74.18% |
| Negative checks in this run | 95 |

Reward comparison, proof reconstruction, and full disk replay passed. A separate
`--replay` invocation also passed. Genesis is included in both totals; the compact
fixture explicitly signs its genesis with the fixture quorum, whereas the inherited
genesis has its existing bootstrap encoding. These are measured fixture sizes,
not the earlier estimate assuming three validator signatures on every record.

Inherited baseline checks passed: 16 offline tests (consensus, record format,
registry, factorization, Pratt proofs, chain generation, store operations, node
replay, finalization/sidecar recovery, round changes, signatures, and epoch
rotation), plus local quorum mining, reserve lock, and validator admission.
This is 19 selected inherited tests, not the entire 125-test inherited suite.
Four experiment CTest cases passed in the release build and again under
AddressSanitizer/UndefinedBehaviorSanitizer, with no reported sanitizer errors.

## Deliberately not implemented

- Larger integer arithmetic: input frontier is capped at 100,000, proof factors
  are bounded, and payloads are capped at 16 MiB. These are experiment limits only.
- Transactions, governance, changing validator sets, fees/reserves, and participation
  history in the new format. No converter exists that could silently drop them.
- In this comparison generator: open-block journals and range voting. A separate
  prototype now exercises those aspects, configurable range limits, closing vote
  locks, and an isolated message interface over localhost TCP; see `atomic-ranges.md`.
- Production checkpointing/pruning, policy voting, live-server range submissions,
  migration, issuance multipliers, or polynomial evidence.

A separate [continuous-chain experiment](continuous-chain.md) now exercises
local multi-block installation, interval rotation, full replay, and interrupted
transition recovery. It retains journals and is not connected to a live server.
The [local validator process test](local-validators.md) connects the coordinator
to TCP messages in three independent local workers with explicit test-driven
quorum collection and catch-up.

The existing client/node APIs remain unchanged. See `docs/prime-block-design.md`
for the decisions needed before this becomes a network protocol.
