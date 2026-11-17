# Proposal selection and conflict recovery

Status: decision draft and executable counterexamples, 2026-11-17.
Not an activated protocol or an implemented recovery mechanism. Existing
three-validator workers and their durable locks are unchanged. No old-chain
files, validator services, or production keys are touched.

## What the current experiment proves

The three-process test demonstrates delivery, durable intent, and restart
recovery for work that can gather a fixed-two-of-three certificate. It does not
demonstrate recovery when different validators have already signed different
subjects at the same position. The first signature currently locks the entire
range or closing block until acceptance of a certificate resolves it.

New `prime_block_conflict_boundaries` tests exercise the actual journals,
ML-DSA signatures, and replay verifier:

- Three distinct overlapping-range/provider proposals split all three locks;
  reopening the journals preserves the stall.
- Three prime providers similarly split closing-block votes.
- An honest minority can accept a valid majority range certificate without
  signing another candidate or losing the winner's attribution.
- One deliberately equivocating signer B signs X with A and Y with C. Both
  prime certificates pass the current verifier and produce different replayed
  rewards; overlapping range certificates likewise disagree on attribution.
  Honest A and C follow their own journal rules throughout.
- Repeated signatures from one identity do not count as two validators, and a
  corrupted signature is rejected.

The same test enumerates all exact-size quorum pairs for sets of up to eight
members. A separate, explicitly abstract trace illustrates why deleting a lock
on timeout is unsafe: A may already have decided X from A+B while B and C later
sign Y. The test does not add such a reset to the real journals or pretend that
the current round-one verifier accepts a round-two certificate.

A passing boundary test confirms these *expected limitations*. It is not a
passing Byzantine-safety test. No claim is made about a historical live-chain
fork; the counterexamples use fresh disposable keys and synthetic data only.

## Decision required: fault model

For equal-weight validators, two size-q signer sets among n identities overlap
in at least `max(0, 2*q - n)` identities. If up to f identities can sign both
values, require `2*q - n > f` for an honest overlap. To progress when those f
identities withhold votes, also require `q <= n - f`.

| Configuration | Honest overlap despite one equivocator | Progress with one absent signer |
| --- | --- | --- |
| 2 of 3 | No | Yes |
| 3 of 3 | Yes | No |
| 3 of 4 | Yes | Yes |

These are necessary quorum conditions, not a full cross-round safety or liveness
proof. Four identities alone do not implement BFT. Four processes under one
administrator, host, or signing-key store do not create four independent fault
domains, either. Admission and voting weights must eventually preserve the
chosen assumptions; adding identities cannot be a free way to acquire votes.

Recommendation: design for one Byzantine validator with at least four
equal-weight validators and a reviewed BFT protocol. If the initial experiment
instead assumes three trusted validators, use an explicitly crash-fault-tolerant
protocol, and do not describe that configuration as tolerating one dishonest
validator. No extra server is provisioned by this document.

Reference families, not a commitment to a new dependency:

- [Tendermint's protocol paper, Algorithm 1](https://arxiv.org/pdf/1807.04938)
  uses a designated proposer, prevotes, precommits, and evidence-aware locking
  across rounds under a less-than-one-third Byzantine voting-power assumption.
  A local timeout advances the procedure; it does not itself authorize forgetting
  a possible decision. Its termination assumptions include eventual timely
  communication and dissemination of consensus messages.
- [Raft, extended paper](https://raft.github.io/raft.pdf) provides a leader-based
  replicated log for non-Byzantine failures. Leader election and log-recovery
  rules matter; copying its timeout alone is not a consensus implementation.

Before choosing an engine, evaluate its application interface, cryptographic
support, recovery behavior, and storage costs. In particular, replacing ML-DSA
with a dependency's default signature algorithm would be a separate decision.

## Proposed PrimeChain application rules

The following requirements apply whichever fault model is chosen. They are not
an alternative consensus algorithm.

1. A miner sends a fully authenticated range or closing-prime proposal to any
   validator. Ingress forwards it to the designated proposer; ingress alone
   cannot request irreversible votes from arbitrary peers.
2. Only the proposer selected by the consensus protocol for this position and
   round can introduce a candidate. Its identity is checked by every validator.
   Proposer selection must not depend on a miner-controlled candidate hash.
3. The proposer first obeys the chosen protocol's recovery constraints. When
   those allow a fresh value, an honest proposer chooses the earliest locally
   queued valid submission extending the current accepted frontier. Local FIFO
   is an implementation policy, not a globally verifiable arrival order.
4. Validators check the complete proposal independently. A range must start at
   the next integer, contain only composites with valid evidence, respect the
   agreed size policy, and preserve its signed provider. No partial acceptance.
5. A certified range advances the accepted/open frontier. A certified closing
   prime installs the compact block only if it covers precisely the accepted
   evidence and attribution. The proposer does not become the credited miner.
6. Overlapping losing submissions become stale when a different range wins.
   Never silently trim or reattribute them. The miner may construct and sign a
   new remaining-tail submission against the new accepted anchor.
7. Pending acknowledgement is not acceptance. Miner APIs distinguish queued,
   certified, stale, rejected, and recovery-required results. Acknowledgement
   loss must be recoverable by subject lookup, not another reward assignment.

This removes competing proposers within one round when they follow the protocol;
it does not guarantee latency fairness or prevent proposer censorship. A timeout
can replace a stalled proposer, not prove which miner was globally first. Those
economic questions remain explicit rather than being hidden in a hash tie-break.

## State and recovery contract

Define the consensus position separately from the integer and prime-block
height. Candidate context must bind the network ID, validator-set/epoch digest,
policy digest, prime-block anchor, accepted range anchor, and next integer.
Both range acceptance and prime closure are ordered consensus decisions.

The selected protocol must specify its complete signing domain, including
position, round/term, phase, signer, and proposal digest (or explicit nil value).
A preliminary vote is not interchangeable with a final decision signature.
Existing round-one votes cannot be relabeled as the new protocol's first phase.
Use an explicit format transition; do not reuse the current certificate tag.

Persist the exact signed message and the protocol's required lock, justification,
round/term, and proposal state before sending a vote. A restart retransmits that
same logical vote. Different valid signature bytes for the same signing payload
are not evidence of equivocation; two different signed subjects in the same
position/round/phase are.

Timeout handlers may progress rounds according to the selected protocol but
must not clear safety state. A later proposer needs the protocol's verifiable
justification for its value. Missing decision evidence is not evidence that no
decision exists. Once a decision certificate is known, catch-up must recover
the complete proposal, verify it, and apply it idempotently.

Proposal availability must not depend on the original ingress remaining online.
Peers must disseminate or serve the candidate and certified open-interval data,
not only their hashes. Resource bounds, invalid-message handling, and per-peer
backpressure are required before public exposure.

## Compact storage implications

The final prime block still contains all composite evidence and credited
addresses, plus its closing proof and final decision certificate. Transient
range votes and recovery messages stay in a durable journal until a reviewed
checkpoint makes their retirement safe. No per-composite voting is introduced.

An off-the-shelf replicated log may retain a separate consensus entry for each
range. We must measure this storage as well as the compact prime file. Engine
snapshots/log compaction must preserve anti-equivocation and open-interval
recovery, not just application balances. If an engine uses a global decision
index, the prime-block checkpoint must commit to the corresponding index and
validator configuration; prime-block height alone cannot reconstruct it after
range boundaries are pruned. The exact new header is not selected yet.

Two voting phases would add temporary signatures and coordination. Benchmark
that cost; do not claim the mining-difficulty bottleneck goal has already been
met. Compact final storage and inexpensive live coordination are separate goals.

## Implementation gates

1. Choose and document the fault model and validator configuration.
2. Select a reviewed consensus engine/protocol and specify the PrimeChain adapter,
   signature domains, durable state, and compact-checkpoint contract.
3. Test protocol state transitions and crash recovery independently of mining.
4. Integrate range/prime validation without changing attribution or reward rules.
5. Run competing-miner process tests with overlapping ranges, proposal equivocation,
   partition/heal, offline proposers, delayed old-round votes, replay, and lost
   acknowledgements. Assert no two honest nodes decide different values, not
   merely that all processes eventually exit successfully.
6. Test eventual recovery after communication stabilizes and a supported quorum
   remains available. All-node split votes must recover under the new protocol;
   unlike today's boundary test, a preserved stall will not count as success.

Until these gates are met, leave the current single-round implementation intact
and do not deploy it as a contention-tolerant or Byzantine-safe testnet.

## Run the boundary tests

```sh
cmake -S . -B build -DPRIMECHAIN_BUILD_EXPERIMENTS=ON
cmake --build build -j 2 --target primechain-conflict-tests
ctest --test-dir build --output-on-failure -R '^prime_block_conflict_boundaries$'
```

The test creates a private `conflicts-XXXXXX` directory under the build artifact
root. It contacts no network, writes no private keys, and changes no live state.
