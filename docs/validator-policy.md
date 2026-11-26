# Validator policy (2026-11-26)

## Decided

**V1. Founder-run validators, said openly.** Until independent validators join, the founder
runs all validators (three today). Public material says so: the chain tolerates one crashed
validator, not a dishonest one (D13); that needs four independent validators.

**V2. Public verification meanwhile.** Anyone may run a checking node: it verifies every
block, proof, certificate and award, and publishes evidence if a validator signs two
conflicting votes (A3-3, A3-4). The first outside participant (the community miner
kaito-zero, if he wishes) is invited to run one; he may become a validator later. A fourth
seat for him now would add no independence, since the founder would hold three of four.

**V3. Path for validators appointed by investors** (the expected source of independent
validators; a long-term goal):

1. **Admission by vote** of the current validators (A3-2, A3-3), with stated requirements:
   an operator separate from other validators, its own server and keys, minimum hardware,
   published contact, and a reserve (parameter, to be set).
2. **One validator, one vote.** Nobody controls more than one seat. When four independent
   validators exist, the founder keeps one seat.
3. **Owner and operator:** an investor may appoint a hosting operator. The consensus key
   stays with the operator; the payout (cold) key with the investor (D10). This follows the
   Fed model: the board appoints, the operator runs (`monetary-layer.md`).
4. **Operator handbook** with the testnet: setup, upgrades (including degree activation,
   `degree-sheets.md`), key care, emergency pauses, votes.

No code change is needed for V1–V3.

## Later

Penalties (for example losing the reserve for provable double signing), removal after long
absence, limits on cross-ownership, and the target of seven validators in different
countries and with different providers: decided when outside validators exist.
