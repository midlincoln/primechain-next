# MVP: the invited testnet that shows the design (agreed 2026-11-27)

**Goal (the founder, 2026-11-27):** go back to building; the MVP should show most of what
has been designed. New ideas should concentrate on **productive work** the chain can
host (ceremonies and the like), not on more finance.

The MVP is the invited testnet (A4 + B4 of `next-plan.md`), widened to include the
monetary layer. Test money only. Everything below follows the agreed docs; nothing here
changes a rule.

## What the MVP shows

| Area | In the MVP | Later |
|---|---|---|
| **Main chain** | one block per prime, composite ranges, Pratt-checked primes, three validators on 9339–9341, replay, catch-up | more validators (V2 checking nodes, V3) |
| **Parameters and votes** | the full parameter table, votes with notice | ranges from a model |
| **Rings** | quadratic rings: commit and reveal, certificates, mining, awards computed from rows, rule A4 issuance, ring liens | cubic and higher degrees as upgrades |
| **Transfers** | transfers of any asset, fees | — |
| **Notes** | issue, endorse, pay, lien; discount, fixed and floater schedules; note series; delivery versus payment; overdue computed | conditional notes (insurance) |
| **Banks** | registration, role addresses, reserve accounts at the Fed, deposit totals, every withdrawal on chain, proof of liabilities, withdrawal claims, freeze and wind-down, prudential measures computed and shown | enforcement steps tuned; deposit insurance |
| **The Fed** | votes on rings accepted, ring prime value, reserve and capital ratios, interest on reserves, discount rate; discount window from reserves (option A) | Fed creating units (option B) |
| **Cash** | RSA notes with test keys, refresh, freeze, on a small test limit | STARK version; key ceremony |
| **Clients (reference only)** | miner client (main chain and rings), wallet, reference bank setup, explorer with the Fed's view | each bank's own software |
| **Productive work** | the toy ceremony recorded on chain | ceremonies at real sizes (B5), canary keys, insurance on breaks |

## Order of work

Three tracks; each step built by a worker, reviewed, shown to the founder, then pushed.

1. **Engine, rings (B3.3–B3.7, `b3-plan.md`):** assets and awards; opening rings; mining
   rings; ring batches in consensus; measurement. Issuance follows rule A4 (a fixed value
   per ring prime), which removes the weights of A3 from B3.5.
2. **Engine, network (A3.2, plan to draft):** the three processes talking over TCP, then the
   three servers next to v1; catch-up from block files.
3. **Engine, money (new, M1–M5, plans to draft one at a time):**
   - M1 notes (issue, endorse, pay, lien, schedules, series);
   - M2 banks (registry, roles, reserve accounts, deposit totals, proofs, claims, freeze);
   - M3 prudential measures and the Fed's view (computed every block);
   - M4 discount window and Fed parameters;
   - M5 cash with test RSA keys (refresh, freeze, limits).
4. **Clients:** the programs below; then the invited testnet.

Tracks 1 and 3 touch the same state, so they alternate rather than run at once; track 2
is independent. Heavy builds stay one at a time on this host.

## Programs (agreed 2026-11-28)

Separate programs for separate jobs, all on one shared library (formats, checks,
signatures), so none can disagree with the validators on what is valid. Everything except
the validator is a reference: anyone may write their own.

| Program | Who runs it | What it does | Comes with |
|---|---|---|---|
| **validator** | validator operators | node, catch-up, status, store tools (subcommands, F11) | A3.2 |
| **miner** | anyone | main-chain and ring mining | A3.2 (N5), B3.5 |
| **wallet** | anyone | keys, transfers, balances; later holding and paying notes, holding cash | A3.2, then M1, M5 |
| **bank** (reference setup) | a bank | role addresses, the customer ledger (private, never on chain), proofs of liabilities, answering withdrawal claims, buying and redeeming cash, the book of notes and its prudential figures | M2–M5 |
| **ceremony** | ceremony participants | join, contribute one step, publish it with its proof, check the others; later canary keys; small enough to run on a clean, offline machine | toy ceremony on chain |
| **explorer** | anyone, read only | chain, rings, assets, and the Fed's view (bank measures, reserves, overdue notes) | M3 |

## Productive work (the frontier)

Ideas for useful work the chain can host, to be collected and judged separately from the
MVP: ceremonies for public parameters and canary keys (`ceremonies.md`), class groups and
isogeny graphs (B5), searches for mathematical objects with checkable answers. Each idea
gets a short sheet: what is computed, how the chain checks it, who pays, what it is for.
