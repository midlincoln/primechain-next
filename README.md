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

Example separation:

```text
current testnet:
  public/status/mining: 8339
  validator peer:       8340
  record sync:          8341

PrimeChain Next experiments:
  public/status/mining: TBD
  validator peer:       TBD
  record sync:          TBD
```

Exact ports will be chosen before deployment.

## Design Goals

PrimeChain Next is focused on reducing protocol bureaucracy while preserving deterministic replay and finality.

Primary goals:

- support batch or range submission instead of one quorum round per integer;
- reduce validator coordination on the critical path;
- remove lottery-dependent liveness risks;
- support larger integer ranges cleanly;
- define a simpler evidence model for prime and composite verification;
- keep replay deterministic and auditable;
- simplify validator admission and removal;
- define cleaner reserve, reward, and inflation rules;
- keep bootstrap/snapshot behavior explicit from the start.

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

1. Write the range/batch record design.
2. Define large integer encoding requirements.
3. Decide whether range records are atomic or partially accepted.
4. Define reward assignment inside a range.
5. Define deterministic replay for range records.
6. Prototype local replay before validator networking changes.
7. Choose separate experimental ports and service names.
8. Build a clean migration/snapshot tool for the old chain.

## Status

This repository is experimental. Do not use it to run the current live testnet.
