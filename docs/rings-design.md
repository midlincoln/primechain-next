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

## Proposed (awaiting agreement)

- **R1, anchoring:** `extension_root` = Merkle root over the registry tip and
  every field-chain tip. Ring messages are collected between blocks and
  finalized with the closing-prime decision, like transactions (D6).
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
