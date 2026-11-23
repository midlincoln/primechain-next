# PrimeChain Next: plan

Status: agreed 2026-11-23. Supersedes the earlier milestone list in
`working-plan.md` and in the README as the working plan for Next.

## What we learned first (phase 0, done)

- `round-change-audit.md`: v1's round-change path compared against PBFT; the
  G2 stall; a two-phase (Tendermint-style) design, tested in models.
- `extension-certificates.md`: certificates for quadratic fields and an
  independent GMP verifier (1,165 certificates; the nine Heegner–Stark fields).
- `v1-baseline-measurements.md`: v1 runs at 36 integers/hour. The dominant
  cost is rebuilding node state per request (1.3–2.5 s of CPU each), not
  arithmetic and not the network.

Two consequences shape the plan:

1. **The first engineering priority is an in-memory engine.**
2. **The main chain never needs big integers.** Walking the integers in order,
   the frontier cannot pass about 10¹⁵, so 64-bit integers are enough.
   Arbitrary precision (GMP) is needed only inside certificates: rings,
   curves and landmarks.

## Roles of the layers

- **Main chain:** the clock and the ledger. One block per prime. It opens
  rings through its frontier F, supplies certified primes to the layers
  above, and anchors them through `extension_root`.
- **Rings layer:** the ring registry (F-proofs with commit–reveal and
  discoverer attribution), plus one field chain per opened ring, which
  records how each main-chain prime factors in that ring.
- **Evidence and privacy:** class groups and supersingular curves, which
  later enable privacy.

## Two tracks

| Weeks | Track A: main chain | Track B: rings and evidence |
|---|---|---|
| 1 | **A1 Main-chain design:** block format (Merkle roots, `extension_root`, reserved privacy roots), segment submissions sliced at primes, evidence rule, two-phase any-quorum consensus with growing timeouts, signing domains, versioned types, reward as a function | **B1 Rings design:** registry (commit–reveal, discoverer, fees), field-chain format, anchoring. Quadratic first |
| 2–4 | **A2 Core engine:** one in-memory state with incremental updates; prime-block storage and index; segment checking with the sieve rule; deterministic slicing; replay reproducing v1 reward accounting; v1 features ported. Target < 200 MB of memory | **B2 Cubic and quartic certificates:** Dedekind–Kummer, O_K certification, canonical field identity |
| 4–6 | **A3 Consensus and network:** two-phase rounds, locks persisted before votes are sent, growing timeouts, equivocation evidence, process tests from the model scenarios; 3 local processes, then the 3 servers on 9339–9341 | **B3 Rings on chain:** registry and field chains wired into the engine through `extension_root`, using the same consensus, batched |
| 6–8 | **A4 + B4 Invited testnet:** main chain plus quadratic rings from day one; segment-miner client; proof-carrying query API; multi-day rehearsal next to v1, measured against the baseline | |
| later | Public-launch preparation: v1 balance migration and swap | **B5 Evidence expansion:** class groups, supersingular curves and isogeny graphs (p = 419 first), Diophantine searches; a web fallback with rewards. **B6 Privacy activation:** stealth addresses, then ring signatures, when the certified evidence allows |

The tracks meet only at the `extension_root` slot (fixed in A1) and at wiring
the rings into the engine (B3, after A2). The rings layer sits behind
`extension_root`, so if it has problems it can be paused without stopping the
main chain.

## Decision gates

- **A1:** evidence rule A/B/C; segment cap K; several miners per decision;
  stating "3 validators = crash-tolerant" plainly.
- **A2:** memory budget.
- **B1:** commit–reveal parameters; refundable fee; discoverer share.
- **B2/B3:** eligibility rule (Minkowski or Bach); canonical identity.
- **B5/B6:** which evidence earns rewards; security threshold for privacy
  activation.
- **Before public launch:** migration and swap of v1 balances.

## Alongside

- v3 position paper and a reply on Metzdowd, once A2–A4 have results.
- Collaborators.
- The optional landmark/ECPP track for "difficult primes".
- Economics: the issuance function and the discoverer share.

## Working rule

A clean design comes first: open decisions and worked examples, agreed in
discussion, before implementation (`feedback`: design before code).
