# Autonomous local peer coordination

This opt-in prototype lets a miner submit to one validator. That validator
collects peer votes, assembles a certificate, applies it locally, and delivers it
to the other two validators. The parent test no longer collects votes or drives
catch-up. Everything runs on ephemeral loopback ports, with disposable keys and
separate private directories. No inherited live-node path or remote service is
changed.

## Submission and delivery

- `SubmitRange` carries a miner-signed composite range with no validator votes.
  The whole range is checked and voted atomically; individual composites do not
  receive separate votes.
- `SubmitBlock` carries the closing prime block with no validator votes. Its
  composite contents must match the accepted open interval.
- `Pending` acknowledges a durable local vote lock and persisted submission
  intent, not a quorum or mining reward. Repeating the submission after completion
  returns `AlreadyAccepted` or `AlreadyInstalled`, with the certified payload.
- `VoteRange` and `VoteBlock` use the existing lock rules. Returned votes must
  match the requested subject, configured peer identity, key, round, and signature.
- `DeliverRange` and `DeliverBlock` require a valid fixed-two-of-three certificate.
  Delivery retries are idempotent. Historical ranges can be acknowledged through
  retained journal receipts even after their prime block closes.

Only a validator with a persisted miner submission intent starts collecting votes.
Receiving a peer vote request alone does not make that validator another proposer.
There is no leader election. A miner can choose a different ingress validator for
later work, but contradictory proposals can still stall the single-round protocol.

The worker's main thread serializes all chain and outbox mutations. At most one
outbound exchange per peer runs asynchronously, so waiting on a peer does not
block inbound voting. The loopback transport uses socket timeouts and retries;
it alternates queued deliveries with outstanding vote requests. Failed exchanges
have a short backoff. This is not a throughput benchmark or a public server.

## Durability

`peer-outbox.dat` contains a versioned, checksummed snapshot bound to genesis,
validator identities, local identity, and range policy. It retains submission
intent, locally assembled certificates, their application state, and a delivery
cursor for each peer. Publication uses the existing fsync/rename helper.

The order is:

1. Persist the core vote lock, then submission intent, before replying `Pending`.
2. Collect enough valid peer votes and persist the assembled certificate as
   unapplied, before changing the accepted interval or installing a block.
3. Apply the certificate, persist its applied state, and clear submission intent.
4. Deliver applied events in order to each peer and persist acknowledgements.

Restart recovers unapplied certificates idempotently and resumes durable intent
and unacknowledged deliveries. A crash between the core lock and intent writes
can leave only a vote lock: no `Pending` was promised, and the miner must retry.
Uncertain storage failures stop coordinator service pending recovery. A missing,
corrupt, or mismatched outbox is not silently recreated on reopen.

The queue retains up to 256 certificates and its snapshot is bounded to 16 MiB.
At the count limit, new submissions are rejected; a byte-limit or persistence
failure fails closed. Certificates and journals are not pruned, even after all
peers acknowledge them. Snapshot rewrites and historical receipt lookup favor
simple recovery over performance: receipt lookup may replay an entire prefix.
Total storage therefore exceeds the compact finalized-block size measurement.

## Tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPRIMECHAIN_BUILD_EXPERIMENTS=ON
cmake --build build -j 2 --target primechain-peer-coordination-tests
ctest --test-dir build --output-on-failure -R '^autonomous_peer_coordination$'
```

The test runs three independent fork/exec validators from genesis 2 through 101
(26 prime blocks) with a two-member range limit. The parent sends miner proposals
and polls status; workers assemble all new certificates themselves. Coverage:

- Range policy and invalid-member rejection, malformed payloads, and verification
  of actual worker-produced certificates.
- Wrong peer identity responses cannot contribute to quorum.
- No quorum with two peers down; after killing/restarting the ingress, persisted
  intent completes when peers return, without another miner submission.
- C is offline from prime 7 through 17; sender queues catch it up automatically.
- Lost miner acknowledgements and peer delivery acknowledgements are retried.
- Worker exit before certificate publication, after publication, and after local
  application; restart resumes without asking the miner to resubmit.
- Rotating ingress, historical duplicate range delivery, and rejection of
  uncertified delivery even when its subject was previously accepted.
- All workers restart; independent disk replay agrees on balances, supply,
  composite attribution, and proof lookup, including Pratt construction.
- Corrupt/missing outboxes refuse startup. A separate contention fixture creates
  three conflicting prime proposals; retries and restart do not erase locks or
  produce a quorum.

Fault controls and endpoint configuration use a private inherited control socket,
not public validator operations. Test children are shut down and reaped. See
[the original process harness](local-validators.md) for key and directory handling.

## Remaining boundaries

Catch-up is **sender-driven retry**, not general peer-pull synchronization. A
returning peer depends on the relevant sender and its retained outbox becoming
available. Permanently lost senders, restored/rolled-back peer disks, discovery,
queue compaction, and authenticated delivery acknowledgements remain unresolved.
Status and acknowledgements are unsigned; the test assumes trusted localhost
endpoint configuration. Signed votes are checked, but this transport is not
safe to expose publicly.

The validator set stays fixed at three and all voting stays in round one.
The [proposal/recovery design](../../docs/proposal-recovery-design.md) and separate
`prime_block_conflict_boundaries` test demonstrate why two-of-three signatures
do not tolerate one equivocating validator. The current certificates can fork
under that fault assumption; preserved honest-node locks alone do not prevent it.
Conflicting locks can stall indefinitely. There is no timeout-based lock reset,
round change, new Byzantine safety proof, or general liveness guarantee here.
The listener also remains vulnerable to slow clients within its socket timeout.

Larger integers, transactions, governance/admission/removal, issuance multipliers,
policy voting, network identity, and production integration remain separate work.
This adds no polynomial evidence and does not authorize a deployment. The planned
9339-9341 ports and old-chain services are untouched.
