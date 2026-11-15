# Three local validator processes

This opt-in test connects `ContinuousChain` to the experimental validator
message interface and runs three independent fork/exec worker processes. Each
worker owns a different chain directory and signing identity, opens its own TCP
listener on an ephemeral `127.0.0.1` port, and serializes requests through one
chain coordinator. No old-chain or remote validator service is contacted.

## Message integration

Range votes, range acceptance, and closing-block votes retain their existing
request types. A continuous-chain handler adds:

- `ChainStatus`: version-tagged status with policy limit, finalized integer
  frontier, accepted/open frontier, prime-block count, block tip, range tip, and
  readiness. Both frontiers are needed while a prime block is open.
- `InstallBlock`: validate and seal the certified closing block, install it,
  initialize/activate the next interval, and return `Installed`. A matching
  certified retry returns `AlreadyInstalled` without touching current locks.

`Rejected` and `RecoveryRequired` remain distinct. An uncertain publication does
not justify submitting conflicting work. Chain status is observational, not a
signed consensus certificate; callers must check readiness.

The existing single-interval handler still supports `Status` and `SealBlock`.
It rejects chain-level status/installation requests. Conversely, the continuous
handler rejects the old interval-only operations: a `Sealed` response must not
be mistaken for a completed chain transition. Existing operation numbers are
preserved; new operations/replies extend the experimental enum space.

## Process harness

The parent generates fresh test keys and a certified genesis. After exec, each
worker receives only its own private key, the public validator set, genesis,
policy, directory, and create/reopen instruction through a private inherited
Unix-domain control socket. Private keys are not put in command-line arguments,
TCP messages, logs, or artifact files. They remain disposable test identities,
not live wallets. Startup reports the child's PID and assigned loopback port.

The test coordinator submits proposals, verifies actual worker votes, assembles
the quorum certificate, and delivers it to validators. Both A+B and A+C quorums
are exercised so all three workers sign. This coordinator is test machinery,
not automatic peer discovery, leader selection, or autonomous validator gossip.

Workers accept bounded length-framed messages with socket timeouts. Truncated
and oversized frames close only that connection. This simple single-threaded
listener is not hardened for hostile/public clients or slow-request attacks.

Fault injection, reply dropping, and shutdown are commands on the private control
socket only; they are not exposed through validator TCP requests. The harness
uses SIGKILL to test restart locks and exits a worker after an injected durable
transition failure to test lost acknowledgements. Graceful shutdown is used at
the end, with explicit wait/reaping; error cleanup terminates only the harness's
own children. Parent-death signals prevent workers remaining alive if the test
parent is terminated. Ports are released on process exit.

## Scenarios

The mining loop advances from genesis 2 through prime 101 (26 prime blocks),
using a two-member range policy and fresh process-generated votes. It verifies:

- Three different PIDs, listeners, identities, and state directories.
- Invalid member rejection, range-size enforcement, and rejection of one-vote
  certificates; duplicate accepted ranges remain idempotent.
- A range vote lock survives killing/restarting A; a conflicting provider is
  refused while the original proposal remains votable.
- A closing vote lock survives killing/restarting B; another prime provider
  cannot replace that locked proposal.
- C is offline while A and B advance from prime 7 through 17. On restart C
  first refuses closing blocks whose range receipts it lacks. The coordinator
  supplies the retained certified ranges and blocks in order; C then reaches
  the same tip. This is explicit coordinator-driven catch-up, not peer sync.
- A exits after publishing a new block checkpoint but before activating its next
  interval. Restart completes installation before serving requests; retry returns
  `AlreadyInstalled`.
- Acknowledgements are lost after a durable range acceptance and after a completed
  block installation. Retrying recovers the prior result without new rewards.
- Malformed framing/requests and ambiguous old operation types are rejected.
- All workers restart from disk at the end and report the same state. Independent
  replay of each worker's compact blocks reproduces every tested balance, supply,
  composite attribution, and proof index; Pratt construction still works.

The nine detailed transition failure points remain covered separately by
`continuous_prime_chain_recovery`; the process test exercises one such point
through the actual chain message handler. These tests do not simulate disk
rollback or power-loss behavior of all filesystems.

## Run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPRIMECHAIN_BUILD_EXPERIMENTS=ON
cmake --build build -j 2 --target primechain-local-validator-tests
ctest --test-dir build --output-on-failure -R '^local_validator_process_recovery$'
build/primechain-local-validator-tests build/prime-block-experiments
```

The `--worker` entry point is internal to this harness, not a deployment command.
It requires the inherited private control socket and cannot select a public
address or fixed port. Generated chain artifacts are in a fresh private directory
under the requested artifact root; no persistent private-key files are created.

## Still not ready for deployment

Automatic quorum collection/propagation, peer catch-up, round changes and timeout
recovery, network authentication, public-endpoint hardening, and unique network
identity/signature domains remain unresolved. Fixed-validator single-round
assumptions are unchanged; passing these tests is not a Byzantine safety or
liveness proof. Journals are retained, not pruned. Transactions, governance,
larger integers, and policy voting are not added by this milestone.

The planned remote ports 9339-9341 are not used, bound, or opened by these tests.
Deployment remains governed by `docs/dual-chain-deployment.md` and separate
operator approval, with the old chain kept running independently.
