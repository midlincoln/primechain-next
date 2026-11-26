# Ceremonies (planned direction, 2026-11-26)

**Agreed direction:** the chain may host ceremonies, in which many participants jointly make
public parameters that nobody can secretly control. They run next to mining, as their own
records. **Starting rule: the sponsor pays.** Design only; building comes after the testnet
(track B5).

## Why

In a post-quantum world what becomes scarce is trust in public parameters. Some isogeny
schemes need a supersingular curve whose endomorphism ring nobody knows; no single party can
make one credibly, but a ceremony can: each participant takes a secret walk from the last
curve and publishes the new curve with a zero-knowledge proof. If one participant deletes
their walk, nobody knows the result's secret (Basso et al., Eurocrypt 2023; Mokrani–Jao
2023 for curves over F_p). The chain records the public part forever and never sees a
walk.

## Records (sketch)

| Record | Contents |
|---|---|
| Open | type and version (e.g. supersingular curve, prime size), starting parameters, number of contributions or end block, reward, sponsor |
| Contribution | previous result, new result, proof, contributor, signature |
| Close | final result and a hash of the whole history: the certified product |

- Validators check that a contribution builds on the latest result and that its proof
  holds. Contributions are sequential: one per ceremony per block; a late one is stale and
  is redone on the new result. Many ceremonies may run side by side.
- Types and versions as for ring certificates (R4); a new type is a protocol upgrade.
- Proofs are kept in their own file, with a size limit per block (a parameter). The root
  sits under `extension_root`, beside the rings, so the block format does not change.
- Rewards: the sponsor's reward is shared among accepted contributions. A protocol subsidy
  (a budget parameter) is possible later.

## Other uses

Trusted curves over F_p, public randomness, setups of other proof systems.

## Status

`experiments/ceremony/`: a toy ceremony on small primes (proofs of 80–128 bytes; forged and
stale contributions refused). Proof sizes and costs at real sizes are unknown; the first
step of B5 is to run the published proof code and measure it.
