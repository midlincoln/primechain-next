# Ring records (B1, draft for review)

Status: **draft, 2026-11-25. Agreed: the five changes in section 6.
Other details and all numbers marked * are proposed.** Exact records for opening
and mining rings and for reading balances. Quadratic rings (certificate type 1)
come first. Rules: `rings-design.md`. Numbers marked * are starting values.

## Overview

| Record | Where | Written by |
|---|---|---|
| `RING_COMMIT` | main block (transaction) | discoverer, with deposit |
| `RING_REVEAL` | rings file (in a ring batch) | discoverer |
| Ring record | state (registry) | protocol, at opening |
| `RING_WORK` | message only (journal, D15) | ring miner |
| Ring batch, ring block | rings file | consensus |
| `RING_PAYEE` | main block (transaction) | current payee |
| Payout, claim | **not written: read from the rows** | — |

## 1. Names

- **Ring ID** = hash(type ‖ degree ‖ identity) (R4). Used in signed messages.
- **Ring number** = 1, 2, 3, … in order of opening. Used inside blocks.
- **Asset** = (ring number, q, i): the i-th prime with norm q above q, ordered by
  root. Main-chain prime p is (0, p, 0). In quadratic rings i is 0 for the smaller
  root (and for a ramified prime), and 1 for the larger. This replaces
  hash(ring ID, q, r): it is shorter, readable, and leads straight to the row.

## 2. Opening a ring

`RING_COMMIT` (transaction): version, network, discoverer, commitment, deposit
(asset, amount), nonce, optional key, signature.
commitment = hash(tag ‖ network ‖ discoverer ‖ salt ‖ hash(certificate)).
Accepted only while gate 1 is open. The deposit is held by nobody.

`RING_REVEAL` (ring batch): version, commit reference (height, index), salt,
certificate type, version, bytes. No signature: only the discoverer knows the
salt. Checked against an unexpired commit first, so unmatched reveals cost
nothing. Then eligibility |D| ≤ F² (R3), the PID certificate, and size ≤ 256 KB*.

**When a ring opens (agreed, R5):** when it is revealed and no earlier commit is still
sealed; otherwise as soon as each earlier commit has been revealed or has expired, at
most 10 blocks after its own commit. The earliest revealed commit for a ring gets it.
Example: A commits at block 100, B commits the same ring at 101 and reveals at 102, A
reveals at 105. B waits; at 105 the ring opens for A, and B gets the deposit back.
Proposed: blocks under a pause of all rings do not count toward the 10.

The deposit is returned for every valid reveal, including a loser's, and otherwise
goes to the fee pool.

**Ring record:** number, ring ID, type, certificate hash, founder, payee history
(block, address), reward schedule (R6), unit (section 3), opening block, frontier,
tip hash, pause state. `RING_PAYEE`: ring, new payee, nonce, signature of the
current payee.

## 3. Mining

`RING_WORK` (message): version, network, ring ID, miner, optional key, start prime,
1 to 64* **consecutive complete segments** (R7), signature, at most 256 KB*.

- One signature covers several segments, because an ML-DSA signature (3.3 KB) is
  far larger than a typical segment. **Each segment still competes and is accepted
  on its own,** like the tail of a range (D4). R7 is otherwise unchanged.
- **Segment:** last prime, row types (2 bits each, R9), and for each paid row
  (c₀, c₁).
- **Checks per row:** the next main-chain prime, q ≤ F; type by the Kronecker
  symbol; N = ±q (+q for D < 0); the root r = −c₀/c₁ mod q is the smaller (or
  double) root. So r is not stored. c₁ is invertible mod q, otherwise q² would
  divide N. The end must be where R7 ends the segment.
- **No padding.** For D > 0, multiplying a generator by a unit makes it as long as
  one likes. The certificate therefore contains a unit ε ≠ ±1, and a generator is
  valid only if neither multiplying nor dividing by ε makes its encoding shorter.
  ε need not be fundamental. For D < 0 the units are few and all have norm 1.

**Ring batch** (third kind of decision): version, network, open height h, hash of
main block h − 1, ring blocks and reveals; at most 256 KB*. Allowed when the previous
decision was not a ring batch. A proposer with a valid closing prime proposes it first;
otherwise it alternates between ranges and ring batches.

**Ring block** (one per ring per batch): ring number, previous ring block hash, start
prime, then groups (miner, hash of the signed `RING_WORK`, its accepted segments).

**`extension_root`:** Merkle root over the ring records (with tip hashes), ordered by
number, plus the open commits and reveals. Ring money counts from the main block that
first includes it.

## 4. Payouts without payout records (agreed)

The row says which primes were produced, by whom, in which ring and in which main block
h. The registry gives the founder's payee at h and the schedule. So **what an address
is due from a new prime X can be computed and need not be written**:

  due(X, a) = miner share, if a mined the row
            + founder share, if a was the payee at h
            + for each prime L below X (R11):
              ⌊ amount(L) · owned(L, a, end of block h − 1) / issued(L) ⌋

  **owned(X, a, t) = due(X, a) + net transfers of X by a up to t**

- **A claim is no longer a transaction.** The earlier R13 said that unclaimed units
  earn, and that claims apply automatically before spending. Then a claim changes
  nothing, so it needs no record, fee or marker. The miner and founder are "credited at once"
  in the same sense. A transfer checks owned ≥ amount.
- **State per address and asset:** only for assets it has sent or received: a list
  (block, net). A new entry replaces the last one unless X paid something between
  them, so histories grow with payouts that matter, not with transfers.
- issued(L) is the nominal amount, so rounding remainders and units held by nobody
  are never issued (R13).
- **Genesis primes 2 and 3:** their share goes to the validators active at h, equally,
  remainder to the first by payout address. This replaces the epoch distribution for
  these rows (R11, D17).
- Main-chain primes keep explicit balances.

Tested (`experiments/nested_rings/lookup.py`): an abstract three-level tower with
random owners and transfers, 5 runs, about 20,000 comparisons each: equal to paying
everyone at once. Three deliberate mistakes are all caught.

`spend_test.py` adds real rows (seven quadratic rings and the degree 2–4–8 tower),
fees, epoch payouts, refused spending, restarts and replay. Three nodes that look
awards up and one ledger that pays everyone at once decided every transfer alike in
5 runs (8,476 accepted, 11,384 refused; 105,341 balances equal). Four deliberate
mistakes are caught. Rounding loss is under 0.0004%. With 351 rings to 10⁶ (27.6
million rows, a synthetic 220 MB index on disk) one look-up takes 0.5 ms (median),
3.9 ms (99%). Signatures, consensus and the engine are not covered.

- **Sale in the block of the split:** the award goes to the seller, who held the prime
  at the end of the block before. Wallets should show this.
- **Awards file:** a node may keep a list "block, ring, prime, owner, amount",
  so that addresses can be browsed as in v1. It is rebuilt from the chain and is not
  part of any block. Writing owners into blocks was considered: one split of a prime
  spread over 166,666 addresses would take about 6.7 MB, in every ring.

## 5. Sizes

One generator in a real quadratic PID (both numbers, reduced by the unit), from the
largest regulator R among 40 PIDs near each D: at most about R/5.5 bytes plus the
size of √q. Average about half. From 10⁵ on only R was computed (PARI, GRH-based).

| D near | largest R | largest generator | whole certificate |
|---|---|---|---|
| 10³ | 55 | 12 B (measured) | 60 B (measured) |
| 10⁴ | 182 | 34 B (measured) | 203 B (measured) |
| 10⁶ | 2,133 | 0.4 KB | ~3–10 KB (estimate) |
| 10⁸ | 20,460 | 3.7 KB | ~0.2–0.6 MB (estimate) |
| 10¹⁰ | 192,604 | 35 KB | ~15–45 MB (estimate) |

- **From F ≈ 10⁴ the certificate size limits ring openings before |D| ≤ F² does.** A
  compact form of generators (known mathematics: products of small elements) would be
  a later certificate version. Bach's bound would shorten the list, not the generators.
- A ring of small discriminant takes roughly 10 bytes per paid row, about 0.5 MB up to
  10⁶ (estimate, 78,498 rows).
- The index of rows must live on disk, not in memory: it grows with rings × rows.

## 6. Changes to earlier decisions

1. R5/R2: a ring opens once no earlier commit is sealed (**agreed 2026-11-25**).
2. R13: no payout records and no claim transactions; balances are computed
   (**agreed 2026-11-25**), with an optional awards file.
3. R7: one signed message may carry up to 64* complete segments (**agreed**).
4. Certificates: asset name (ring number, q, i); a unit for D > 0; at most 256 KB*
   (**agreed**).
5. R11/D17: the validators' share from 2 and 3 goes to those active at h (**agreed**).

## Open

Deposit amount; the quota κ; ring issuance per entry (simulation); limits marked *;
segments above degree 2, and the other proposals in `rings-design.md`.
