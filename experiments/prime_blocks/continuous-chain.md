# Continuous prime-block installation and recovery

This opt-in, local experiment extends `RangeJournal` with a durable chain
coordinator, `ContinuousChain`. It is not wired into the inherited validator
server, does not deploy anything, and does not modify the live chain.

## Storage and activation

Each validator has its own private directory, created explicitly for this
experiment, and a directory-wide single-writer lock. Its files are:

```text
writer.lock
checkpoint.dat
block-0.dat                    certified genesis
block-1.dat                    next compact prime block
...
interval-1.journal             interval after genesis
interval-1.journal.writer-lock
interval-1.journal.block       sealed closing block, when present
interval-2.journal             next interval
...
```

Block filenames use zero-based prime-block height. Journal filenames use the
number of installed blocks at their anchor. They do not use integer positions.
Each compact block retains all composite divisor evidence and attribution, the
closing prime proof/authentication, and block-level validator votes. Intermediate
range signatures remain in the journals, not the compact block files.

The checksum-protected `PCNXCH01` checkpoint binds the trusted genesis content
hash, validator set, local validator identity, and range policy. It stores the
installed block count, tip hash, and an interval state: preparing or active.
The configuration is externally supplied and fixed for this experiment, not
chain-governed. Reopening with another identity or policy fails closed.

## One transition

1. Validate the closing block against accepted work and its quorum certificate.
   Atomically seal it through `RangeJournal`. This is the durable commit intent.
2. Publish the compact block at its height, using temporary-file fsync, rename,
   and directory fsync. Never overwrite a conflicting or corrupt existing block.
3. Publish a checkpoint with the new block count/tip and **preparing** interval.
4. Open the next interval's journal, creating it only if the preparing checkpoint
   authorizes that creation. Preserve any journal already present.
5. Publish an **active** checkpoint. Only then allow new votes and submissions.

The coordinator owns the journal and exposes voting/acceptance/installation
methods; callers cannot get a mutable journal reference and bypass activation.
It is single-threaded. Failed persistence or an interrupted transition disables
further operations until a new coordinator instance reopens the directory.

`install` distinguishes `Installed`, `AlreadyInstalled`, `Rejected`, and
`RecoveryRequired`. Retrying any previously installed matching certified block
does not pay rewards again, rotate the current journal, or discard its locks.
`RecoveryRequired` means publication may already have happened; it is not proof
of rejection. Status/history accessors may expose the last in-memory view after
failure, so callers must check `ready()` before treating it as usable state.

## Restart

Startup replays every committed block from genesis, verifies the checkpoint tip,
and rebuilds balances and the composite proof index. Balances are derived state;
there is no independently incremented reward file that could be applied twice.

If the active journal contains a sealed block but the checkpoint still names its
parent, startup completes installation. A block already published at the next
height must match the sealed intent and pass verification before reuse. If the
checkpoint advanced but next-journal initialization was interrupted, the
preparing state allows recovery to finish that initialization and activation.

**A missing active journal is an error.** It is never silently recreated: it may
have held a vote lock or accepted work. Missing/corrupt checkpoints, committed
blocks, and journals also fail closed. `open(create=true)` refuses an existing
directory rather than resetting it. An interruption during initial directory
creation before a valid checkpoint exists requires operator diagnosis; startup
does not reconstruct a checkpoint by guessing from files. No votes are enabled
during initialization.

The tests inject interruption after sealing, before/after block publication,
before/after the preparing checkpoint, before/after journal opening, and
before/after activation. Each of these nine cases also runs in a child process
that exits without destructors after the interrupted operation. Reopening must
recover the block, keep exact rewards, preserve subsequent vote locks, and allow
the following block to complete. This exercises process interruption and lost
acknowledgements, not actual power loss, disk rollback, or every filesystem.

## Run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPRIMECHAIN_BUILD_EXPERIMENTS=ON
cmake --build build -j 2 --target primechain-continuous-chain-tests
ctest --test-dir build --output-on-failure -R '^continuous_prime_chain_recovery$'
build/primechain-continuous-chain-tests build/prime-block-experiments
```

The fixture uses three independent validator directories, fresh in-memory keys,
and a test coordinator in one process for the normal mining loop. It runs from
genesis 2 through prime 101: 26 blocks, including the empty interval 2 to 3,
two-member/singleton ranges, multiple providers, and repeated restarts. It also
independently replays compact block files without consulting range journals.

## Remaining boundaries

- No journal pruning: receipts, vote locks, and sealed copies are retained. Total
  deployment storage is therefore larger than compact block bytes alone.
- Full replay and state copies favor clear recovery tests over scalability. The
  existing integer/payload/open-interval bounds still apply, with at most 10,000
  installed blocks in this experiment. This is not a production storage engine.
- The continuous coordinator now has a message-handler overload and an isolated
  [three-process localhost test](local-validators.md). The harness collects votes
  and delivers retained receipts explicitly. Autonomous validator networking,
  peer catch-up, timeout/round-change recovery, and public-endpoint hardening
  remain ahead; this is not a production daemon.
- Fixed-validator, single-round certificate assumptions are unchanged. This
  milestone is not a new Byzantine consensus proof or liveness guarantee.
- Transactions, admission/removal, participation accounting, larger integers,
  policy activation, and the optional issuance multiplier remain separate work.
  Evidence-layer expansion is paused.
- A trusted genesis/configuration anchor is not a complete new-network identity
  implementation. Explicit network IDs/signature domains and the separate ports,
  services, and resource budgets in `docs/dual-chain-deployment.md` remain launch
  prerequisites. No server deployment is implied by these passing tests.
