# Degree sheets: adding rings of a new degree (2026-11-25)

**Agreed:** every degree answers the same list of questions (its degree sheet). A degree
is added by a validator vote plus a software upgrade on each validator, with activation
after notice (R4, R2, D12). No smart-contract machine is needed.
**Proposed:** the safety steps and the general sheet below.

## Three layers

| Layer | Contents | Changed by |
|---|---|---|
| Common rules | PID required; only primes of norm q pay, e/n each (R12); computed awards (R13); opening (R5) | protocol upgrade |
| Degree sheet | the answers below, and its checking code | vote, plus upgrade, with notice |
| Parameters | sizes, limits, rewards (`protocol-parameters.md`) | vote with notice |

## The sheet

| # | Question | Degree 2 | Degree 3 |
|---|---|---|---|
| 1 | Unique name | D | open |
| 2 | Proof of the full ring of integers | D fundamental, Z[ω] | open |
| 3 | Discriminant, signature, Minkowski bound | √D/2 or (2/π)√abs(D) | 2√D/9 or 8√abs(D)/(9π) |
| 4 | Eligibility | abs(D) ≤ F² | open |
| 5 | Galois, automorphisms, subfields | Galois, none | usually not Galois, none |
| 6 | Generator form and norm check | c₀ + c₁ω | three integers; open |
| 7 | PID certificate | done | open |
| 8 | Units and padding | one unit (D > 0) | one or two units; open |
| 9 | Splitting types | 3 | 5: PQR; P·Q (norms q, q²); inert; P²Q; P³ |
| 10 | Labels of primes above q | smaller, larger root | open |
| 11 | Which primes pay | norm q, e/2 each | norm q, e/3 each |
| 12 | Row, segment, generators per row | 1 | up to 3 |
| 13 | Smaller rings below, awards | main chain only | main chain only |
| 14 | Limits, checking cost, test vectors | done | open |

Degree 2 is specified in `extension-certificates.md` and `ring-records.md`.

## Activation (proposed)

1. The proposal names the sheet, its test vectors and the exact code commit (by hash).
2. Validators vote (A3-2). A passed vote alone does not start anything.
3. Each operator upgrades, **one validator at a time**. The new code stays off.
4. The validator runs the test vectors. Its "ready" message (as for new validators, A3-3)
   carries the hash of the results. Equal results are required from all active
   validators, or the degree does not activate.
5. At the activation height the degree opens. Before it, blocks with such rings are
   invalid.

Reasons and consequences:

- **The same result everywhere:** exact integers only, no floating point, bounded work.
- **A validator without the code stops** at the first block with such rings, since ring
  money is in `state_root`. With three validators and quorum 2, one laggard leaves no
  spare. Hence "all ready", not only a quorum. (If one validator refuses forever, it has
  to be removed by vote first.)
- **Old rules stay in the code,** by height, so that a replay gives the same chain.
- **A faulty checker:** emergency pause of the degree (R2, per ring today; per degree to be
  added). Units already issued are not taken back.
- Miners, wallets and explorers must upgrade too.
- Trial on a test network first.

## General sheet (proposed)

One checker for any degree: polynomial, integral basis with proof of maximality,
generators by coefficients, norm by determinant. Then a degree could be enabled by a vote
on limits, without new code. Special sheets (quadratic, cubic) stay shorter and faster.

**Duplicates:** a new ring is compared only with open rings of the same degree and
discriminant. If some prime q below a limit splits differently in the two, they are
different. Otherwise it is refused as a duplicate. No ring can enter twice. Up to degree 6
different fields always differ at some prime (Perlis; to be checked); from degree 7 a few
pairs look alike, and the second of them is refused.
