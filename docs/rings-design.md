# Rings layer design (B1)

Status: in progress. Decisions are recorded as they are agreed. Plan:
`next-plan.md`, track B1. Certificates: `extension-certificates.md`.

## Structure (agreed earlier in discussion)

- **Ring registry:** records ring openings (certificates and discoverers),
  using commit–reveal.
- **Field chains:** one per opened ring. Each records, for every main-chain
  prime in order, how that prime factors in the ring (with generators).
- **Discoverer:** earns a share of the ring's own issuance. The main chain's
  reward split (D8) is unchanged.
- **Ring assets:** transferred inside their own field chain (D6).

### Protocol messages (versioned, D12)

1. `RING_COMMIT`: discoverer address, commitment = hash(certificate ‖ address
   ‖ salt), refundable fee, signature.
2. `RING_REVEAL`: the commitment it answers, the salt, and the certificate
   (field identity D, claimed eligibility prime F, factorization of |D|,
   generators). Validators check, in order:
   1. it matches an earlier finalized commitment by the same discoverer;
   2. claimed F ≤ current chain frontier;
   3. the eligibility rule holds for F;
   4. the certificate verifies;
   5. no earlier commitment for the same D has been revealed (earliest wins).
3. `FIELD_ENTRY`: ring ID, main-chain prime q (the ring's next one, in order),
   decomposition type, generators, provider, signature.

## Decided

### R2. Validator gates, with advance notice (2026-11-23)

Two policy gates, voted by validator quorum, following the institutional
model in which validators provide settlement and policy, as regional
Reserve Banks do:

- **Gate 1, new rings (global):** open or closed. While closed, `RING_COMMIT`
  is rejected on arrival, and no fee is taken.
- **Gate 2, mining per ring:** a registered ring accepts `FIELD_ENTRY`
  submissions only after validators allow mining on it. The discoverer share
  accrues only while mining is allowed.
- **Notice:** gate changes take effect only after a notice period of N blocks
  following the finalized vote, so miners can predict the rules (Nash
  credibility). N is a policy constant to be set.
- **Exception:** an **emergency pause** of a single ring takes effect
  immediately, and only if its verifier is shown to be faulty. It is recorded
  on chain.
- Commitments finalized while gate 1 was open may still be revealed within
  the reveal window after it closes.
- Gate states are part of the policy digest in every block header (D9).

### R7. Ordering by norm (2026-11-23)

- Within each ring, field-chain entries are ordered by **increasing norm** of
  the prime ideals they record. The norm is unaffected by units and works the
  same way for real and imaginary fields. For example, (3 + √17)/2 has |N| = 2,
  and 2 + i has N = 5.
- **Invariant:** no ring may record a norm greater than the main-chain
  frontier F. F is always the maximum.
- A global order across all trees is **not** part of consensus. It is a
  derived index that anyone can compute, if one is ever needed.

### R8. Conjugates: one entry per split prime (2026-11-23)

When q splits into π and π̄, one entry is recorded with **one canonical
generator**; the conjugate is derived from it. There is one reward per split
q, since finding one generator finds both. The canonical choice of which
conjugate (for example b > 0 in Q(i), so 2 + i rather than 2 − i) is fixed
together with canonical generators modulo units.

### R9. Only split and ramified primes are recorded (2026-11-23)

- Inert primes produce no new prime element and anyone can determine them
  from D (one Kronecker symbol), so they are **not recorded and not paid**.
  The main chain already paid for their primality.
- Validators check that every prime skipped between two consecutive entries
  is inert, so no split prime can be omitted silently.

### R10. Units and associates (2026-11-23)

- A recorded generator must have norm ±q for a prime q > 1. Units (norm ±1)
  can never pass.
- **At most one entry per rational prime q per ring,** so the same prime ideal
  cannot be recorded twice under different associates (π and π·ε).
- Special treatment of units (for example recording fundamental units as
  evidence) would be a new versioned entry type.

## Proposed (awaiting agreement)

- **R1, structure (revised, DAG):** each ring has its own chain. Every
  ring block links to its previous ring block and to the main-chain block it
  depends on (the one that finalized the prime it records). Ring blocks are
  finalized by validators in their own decisions, batched across rings and
  independent of main-chain blocks. The main header's `extension_root` is an
  optional snapshot of the latest finalized ring tips, for light clients.
- **R3, eligibility:** Minkowski bound M_K ≤ F (for quadratic fields:
  D ≤ 4F² if real; 4|D| ≤ π²F² if imaginary, with a rational enclosure of
  π²). The gates control the pace.
- **D4 amendment (main chain):** ties between batches with the same end are
  broken by content hash, for example the lowest hash of (submission, block
  height), instead of arrival order, so settlement never depends on network
  position.

## Open

- Reveal window length; commit fee amount and refund rule.
- Discoverer share (y%), and whether it is permanent or time-limited.
- Ring-asset issuance per field-chain entry.
- Canonical field identity for degree ≥ 3; canonical generators modulo units
  (needed before ring assets exist).
