# PrimeChain Next

PrimeChain Next is the successor design to the PrimeChain testnet
([midlincoln/primechain](https://github.com/midlincoln/primechain), still running).
It is in development: the mathematics and the core engine are taking shape, the economic
design is being discussed, and nothing here is deployed.

**In one paragraph.** Miners prove that integers are prime or composite, as in v1, but
the chain is compact: **one block per prime**, closing all the composite ranges found
since the previous prime. On top of the main chain, miners can open **rings of integers
of number fields** (quadratic first) once they certify, unconditionally, that the ring
is a principal ideal domain (Minkowski bound). Each opened ring records how every
main-chain prime factors in it, and those splits issue new money to the ring's miners,
the holders of the prime, and the ring's founder. A **monetary layer** turns this into an
economy: ring bonds, banks with reserves held by the validators, and a rule-based
"algebraic Fed" whose main lever is how many new rings it accepts.

## Where things stand

| Part | Status |
|---|---|
| Main chain: one block per prime, atomic composite ranges, Pratt-checked closing primes, replay | engine built and tested (`next/`) |
| Consensus: two-phase rounds, certificates, equivocation evidence, restart | core built and tested in simulation; networking next |
| Protocol parameters: every number in one voted, versioned table | built (B3.1) |
| Rings: certificates, ring math, segments, encodings (quadratic) | built (B3.2), checked against PARI/GP vectors |
| Rings on chain: awards, opening, mining, consensus | designed; steps B3.3–B3.7 next |
| Higher degrees (cubic and up) | designed and tested in experiments; later upgrades |
| Monetary layer: issuance, lending, banks, the Fed | being designed now; changes often |
| Cash, privacy, ceremonies | directions agreed; later |
| Invited testnet next to v1 | after the steps above |

## Start here

**Design** (in `docs/`):

| Read | For |
|---|---|
| [next-plan.md](docs/next-plan.md) | the plan, the two tracks and the milestones |
| [main-chain-design.md](docs/main-chain-design.md) | main chain decisions D1–D20 and consensus (A3) |
| [extension-certificates.md](docs/extension-certificates.md) | how a ring is certified (Minkowski bound, class group) |
| [rings-design.md](docs/rings-design.md), [ring-records.md](docs/ring-records.md) | rings R1–R13: opening, mining, rewards, records |
| [degree-sheets.md](docs/degree-sheets.md) | what each new degree needs (cubic, quartic, …) |
| [protocol-parameters.md](docs/protocol-parameters.md) | every protocol number, its starting value and the rules for changing it |
| [b3-plan.md](docs/b3-plan.md) | the steps that put rings on chain |
| [monetary-layer.md](docs/monetary-layer.md) | money, issuance, lending ("rings are land"), banks, reserves, the Fed, cash |
| [validator-policy.md](docs/validator-policy.md) | who runs validators, now and later |
| [ceremonies.md](docs/ceremonies.md) | trusted-setup ceremonies hosted by the chain (planned) |

**Code:** [`next/`](next/README.md) is the engine (C++17, GMP, OpenSSL; MIT). Build with
CMake from the repository root and run `ctest -R '^next_'`. `next/README.md` lists the
modules and what each test covers. The rest of `src/`, `include/` and `tests/` is
inherited v1 code, kept for reference and for porting.

**Experiments** (`experiments/`, Python and PARI/GP, not engine code):

| Folder | What it checks |
|---|---|
| `extensions/` | certificate generator and test vectors used by the engine tests |
| `degrees/` | certificates and mining for degrees 3–7; subrings and links |
| `nested_rings/` | ring awards computed from rows; spends across a split |
| `ring_issuance/`, `fed_rings/` | issuance rules, money supply and holder yield by rings accepted |
| `cash_cycle/` | the cash note cycle |
| `ceremony/` | a toy isogeny ceremony on small primes |
| `prime_blocks/` | early size, replay and coordination experiments |

**Settled and moving.** The mathematics (certificates, ring math, test vectors) and the
main-chain engine are the settled part and the best place to review. The monetary
documents change almost daily.

## Reviewing and contributing

Reviews and corrections are welcome, especially independent checks of the certificate
rules and the test vectors (for example with your own PARI/GP or Sage). Please open an
issue or a pull request from your fork. `next/` is MIT-licensed; the license for the
documents and experiments is not chosen yet, so please ask before contributing there.

## Relation to the v1 testnet

The v1 testnet keeps running while Next is developed. Next uses separate ports
(9339–9341 instead of 8339–8341), data directories and services, and shares no chain
data, wallets or keys with v1; see
[dual-chain-deployment.md](docs/dual-chain-deployment.md). If Next becomes the public
chain, v1 holder balances are meant to carry over through a published snapshot or swap;
the details (height, eligible balances, reserves, procedure) will be published before
any migration.

Earlier background: [prime-block-design.md](docs/prime-block-design.md),
[v1-lessons.md](docs/v1-lessons.md), [v1-baseline-measurements.md](docs/v1-baseline-measurements.md),
and the [whitepaper](prime-mining-whitepaper.md).
