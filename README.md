# PrimeChain Next

PrimeChain Next is the repository for the next PrimeChain protocol experiment.

The current live testnet remains in the original repository:

```text
https://github.com/midlincoln/primechain
```

This repository starts from that codebase, but it is not the live chain. It is where we will design and test a cleaner protocol before deciding whether to launch a new chain.

## Why A New Repository

The current testnet has been useful because it exposed real protocol and operational issues:

- one-integer-per-record admission is too slow;
- validator coordination dominates arithmetic work;
- composite lottery added fragility and was removed;
- validator admission, removal, reserve lock/unlock, and reward distribution needed live fixes;
- replay and sync rules became harder to reason about after several testnet patches;
- integer representation and evidence format need to be revisited for larger future ranges.

PrimeChain Next lets us work on these changes without risking the running testnet.

## Operating Plan

We intend to keep the existing testnet alive while developing this repository in parallel.

The same physical validator servers may be reused for experiments, but PrimeChain Next services should run on separate ports and separate data directories. The old chain and the new-chain experiments must not share chain data, wallets, peer state, or service units.

Planned separation (checked against listeners on the three active hosts on
2026-11-15; Next ports are not reserved or opened yet):

```text
current testnet:
  public/status/mining: 8339
  validator peer:       8340
  record sync:          8341

PrimeChain Next experiments:
  public/status/mining: 9339
  validator peer:       9340
  record sync:          9341
```

See [dual-chain deployment notes](docs/dual-chain-deployment.md) for host checks,
firewall prerequisites, isolated paths, and pre-launch safety checks. Recheck
availability before deployment; no Next service is deployed by this plan.

## Design Goals

PrimeChain Next is focused on reducing protocol bureaucracy while preserving deterministic replay and finality.

Primary goals:

- store one finalized prime-ended block with all intervening composite evidence
  and credited miner addresses;
- accept contiguous composite ranges atomically, verifying every member but
  voting once per range, with durable range receipts and voting locks;
- limit range size through shared policy, fixed during an open prime block;
- preserve per-composite reward shares and use a block-level certificate before
  pruning intermediate range signatures;
- support larger integers with bounded canonical encodings and checked arithmetic;
- keep proofs searchable and replay deterministic;
- preserve validator admission/removal, transaction, fee, reserve, and reward-share
  behavior unless a specific adaptation is reviewed;
- measure validator coordination before optimizing it.

An issuance multiplier is a separate economic decision. Polynomial mining and
broader evidence work remain research plans, not requirements for this launch.

## Current Chain Migration Principle

If PrimeChain Next becomes a new public testnet or main successor chain, we intend to preserve current-chain holder balances through a transparent snapshot or swap process.

Details are not finalized. Before any migration, we should define and publish:

- snapshot height;
- eligible balances;
- treatment of validator reserves;
- treatment of undistributed reward pools;
- claim or swap procedure;
- deadline, if any;
- verification tools.

The goal is continuity for current miners and holders, not abandoning the existing testnet.

## Initial Work Items

Start with the [prime-block design](docs/prime-block-design.md) and the
[offline size/replay experiment](experiments/prime_blocks/README.md).
The experiment is opt-in and does not change node or network behavior.

An [atomic range/recovery prototype](experiments/prime_blocks/atomic-ranges.md)
adds a configurable range policy, durable range and closing-block vote locks,
and an isolated message interface tested over localhost TCP. A separate
[continuous-chain experiment](experiments/prime_blocks/continuous-chain.md)
installs successive prime blocks and recovers interrupted interval transitions.
A [three-process localhost test](experiments/prime_blocks/local-validators.md)
connects that coordinator to messages and exercises restart, catch-up, and lost
replies with independent workers. An [autonomous peer test](experiments/prime_blocks/peer-coordination.md)
adds miner-to-one-validator submissions, peer quorum collection, and durable
sender-driven delivery/recovery. It remains a bounded, fixed-set, single-round
localhost experiment. None changes the live node. Subsequent
milestones cover larger integer encoding, round-change recovery, production
checkpoint safety, policy voting, and full validator-network integration.
Network identity/deployment separation
and a reviewed migration/snapshot policy are required before any public launch.

The [proposal/recovery design](docs/proposal-recovery-design.md) records the next
decision: trusted crash-fault consensus or a Byzantine-tolerant configuration.
Offline conflict tests reproduce split locks and conflicting certificates with
one equivocating signer under the current two-of-three rule. These are known
boundary counterexamples, not a completed recovery implementation.

## Status

This repository is experimental. Do not use it to run the current live testnet.
