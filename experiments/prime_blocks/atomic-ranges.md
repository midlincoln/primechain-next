# Atomic composite ranges: policy, voting, and recovery

This supersedes the earlier plan to batch transport while retaining per-integer
votes. A range is one atomic consensus subject. Each member is checked, but
validators vote on the whole range, not separately on its integers.

This is experimental code under `experiments/prime_blocks`, with an isolated
message handler and loopback TCP test harness. It is not connected to the
inherited server or activated in production. No deployed validator is changed.

## Range-size policy

`RangePolicy::max_members` is the consensus limit on composites in one submitted
range, provisionally 256. Supply it to `RangeJournal` at construction; there is no
runtime setter. It is not a limit on the total composites in a prime block:
miners can submit several smaller ranges. A range cannot cross a prime.

The signed range includes `max_range_members`; validators require an exact match
to their interval policy, in addition to checking the member count. Quorum
certificates do not override this check. The journal configuration hash also
binds the limit, so reopening with a different policy fails closed. The variable
is independent of `kMaxRangeMembers`, the parser/resource ceiling (currently 256).
Zero and values above that ceiling are rejected before creating a journal.

Future economic-policy voting can determine this value, but voting and activation
are not implemented. The intended activation boundary is the next prime block,
never the middle of an accepted range or open interval. That requires a shared,
chain-committed policy event and deterministic replay rules; changing a local
validator setting is not governance. The current fixture supplies trusted policy
externally and does not add policy events to the compact block format.

Experimental range/journal tags are now `PCNXRNG2` / `PCNXRJ02`. Previous generated
v1 journals are intentionally rejected, not silently upgraded or reset. The
prime-block encoding is unchanged.

## Proposal and receipt

A proposal contains the preceding finalized block hash, preceding accepted range
hash (zero for the first range in the block), inclusive start/end, range policy, provider
address, and one divisor/cofactor pair per integer. The integer for each pair is
implied by its position. The provider signs this complete canonical payload once.

Validators check bounds, exact member count, every product and nontrivial divisor,
the miner signature, and the frontier/parent linkage. They persist a voting lock
before returning a vote for the range's content hash. Range hashes use their own
`PCNXRNG2` domain tag. Authentication bytes are not part of that identity, so
resigning identical claims or assembling another valid quorum does not create
new work or a new reward.

The fixture uses the inherited two-of-three quorum signature verifier with a
fixed validator set and round one. A validator must not sign another range at
the same position after restart. A valid quorum certificate may resolve a
different minority local vote; accepting that certificate is not signing a
second proposal. General round changes, leader selection, and Byzantine-network
liveness are not implemented or claimed by this experiment.

On a valid quorum certificate, the whole range is durably published before an
acceptance receipt is returned. An accepted range must start at the next
unaccepted integer. A missing/invalid proof, prime inside the range, gap, wrong
parent, or conflicting overlap rejects the whole proposal. A singleton composite
is a valid range. An identical certified range returns `AlreadyAccepted`, including
after restart, with no additional state transition or reward shares.

Only a successful range acceptance advances the open frontier. Merely receiving
a proposal, returning one vote, or buffering its proofs does not advance it.

## Durability boundaries

Each validator has its own journal and identity binding. A separate `flock` file
excludes a second writer. The checksum-protected journal includes its finalized
anchor/configuration, full accepted receipts, and any outstanding vote lock.
Loading validates signatures and contiguous linkage again. A missing journal is
not silently initialized; corruption fails closed rather than discarding locks.

For this bounded prototype, each update atomically replaces the complete journal
using the repository's temporary-file/fsync/rename/directory-fsync helper. This
is simple for recovery testing but is not a scalable append-log design.

Any publication error blocks further voting/acceptance in that instance until it
is reopened. `RecoveryRequired` means the disk may contain the old or new state;
it is not a claim that the range was definitely rejected. Tests inject failure
before publication and acknowledgement loss after durable publication. After
reopening, a retry either accepts the whole range or returns its existing receipt.
These tests do not simulate every filesystem, actual power loss, or disk rollback.

Beyond the configured range policy, the prototype bounds a journal at 256
accepted ranges, an open interval at 4096 composites, and integers at 100,000.
These are test resource limits, not production parameters or permission to skip
evidence. Do not raise the range parser ceiling without auditing wire size and
verification-work limits as well.

## Closing the prime block

The closing block must contain exactly the journaled composites in order with
the same proofs and attribution. Its prime must be the next unaccepted integer.
`PrimeBlockReplay::validateProposal` checks the prime proof, miner authentication,
coverage and history without requiring a quorum or mutating replay/reward state.
`voteForBlock` also checks the exact journaled evidence and attribution, then
persists the complete closing proposal before returning a validator signature.
It refuses conflicting closing proposals, including after restart, and cannot
vote while a range vote is outstanding. A closing lock freezes further range
votes and new acceptances. Retries of the same closing subject are allowed.

`seal` additionally requires the complete block quorum certificate; proposal
validation or one vote is not finalization. A valid majority certificate may
resolve a different minority closing vote, without emitting a conflicting vote.
The journal publishes the compact block atomically
as `<journal>.block`. On restart it validates that file against the receipts and
recognizes the interval as closed even if the acknowledgement was lost.

The compact file contains composite evidence but no range or composite signatures;
it retains the prime miner authentication and block-level validator signatures.
The journal still retains all range receipts. **Automatic pruning is deliberately
not implemented.** `RangeJournal` alone stops at a sealed block. The separate
[continuous-chain coordinator](continuous-chain.md) now installs compact blocks,
advances to the next journal, and tests recovery across that transition locally.
Automatic network receipt synchronization is still absent. Production checkpoint
and pruning rules must be reviewed before deploying or deleting receipts.

For example, Alice's range 8-9 and Bob's range 10 produce three reward shares.
When prime 11 closes the block, Alice receives 300,000 micro-units and Bob receives
150,000 from the existing 450,000 composite pool. This is not a half-and-half
split between two ranges. Evidence remains available for future Pratt proofs.

## Validator message boundary

`validator_messages.hpp/.cpp` provides bounded, version-tagged binary requests:
`Status`, `VoteRange`, `AcceptRange`, `VoteBlock`, and `SealBlock` for a single
interval. A continuous-chain overload adds `ChainStatus` and `InstallBlock`,
described in [the three-process test](local-validators.md). Voting replies
return the submitted object with one validator vote; the test coordinator checks
and combines votes and sends the certificate to each journal. Status reports
the range policy, accepted frontier, range count, closed flag, and readiness.
Errors distinguish rejection from `RecoveryRequired`; a durable receipt retry
returns `AlreadyAccepted`. Unknown kinds, bad tags, extra bytes, invalid payloads,
and declared lengths above bounds are rejected.

The single-interval harness uses real TCP on ephemeral `127.0.0.1` ports with bounded frames,
socket timeouts, and deterministic cleanup. It tests three independent validator
journals in one process. The separate continuous-chain test now uses three
fork/exec workers; neither harness is a production deployment. There is no public
listener, automatic peer broadcast/quorum collection, catch-up protocol, peer
authentication, rate limiting, or multi-round coordination. The caller must
serialize access to each journal. This boundary is preparation for server
integration, not an Internet-ready service.

## Run and coverage

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPRIMECHAIN_BUILD_EXPERIMENTS=ON
cmake --build build -j 2 --target primechain-atomic-range-tests
ctest --test-dir build --output-on-failure -R '^atomic_range_recovery$'
build/primechain-atomic-range-tests build/prime-block-experiments
```

Fixtures use fresh in-memory ML-DSA-65 keys and private generated directories.
Tests exercise atomic rejection, whole-range votes, persisted conflict locks,
minority-vote resolution, duplicate receipts, overlap rejection, range linkage,
restart recovery, closure attribution, reward shares, compact replay, signature
tampering, malformed encodings, and write/acknowledgement failures.
They also exercise configurable limits, mismatched-policy replay, closing-block
vote locks, pre-vote prime authentication, and the complete range-to-prime-block
flow over localhost TCP, including lost acknowledgements and restart retries.

Transactions, validator admission/removal, participation accounting, and policy
activation at prime-block boundaries still require a reviewed network design. They remain
unchanged in the inherited production code, not implemented in this prototype.
