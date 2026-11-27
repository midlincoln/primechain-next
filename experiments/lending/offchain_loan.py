# An ordinary user with nothing on chain borrows micro-units from a bank (exploratory, 2026-11-27).
# No on-chain collateral: repayment is enforced off chain (identity, contract, income), like a consumer loan.
# Year-11 economy, rule A4, 10 rings/yr: holder yield y = 3.5%. All amounts in micro-units unless in $.
import random
y = 0.035          # holder yield: what the bank's units would earn if it kept them
PRICE = 0.001      # $ per micro-unit (working assumption)

def break_even(pd, rec, cost, y=y):
    # (1-pd)(1+r) + pd*rec = 1 + y + cost
    return (1 + y + cost - pd * rec) / (1 - pd) - 1

print("1. Loan rate the bank must charge to break even (1-year loan, costs 1%/yr):")
print(f"   {'defaults':>9}{'recovered':>11}{'y=3.5% (10 rings/yr)':>23}{'y=7% (20 rings/yr)':>21}")
for pd, rec in ((0.02, 0.5), (0.05, 0.3), (0.10, 0.2)):
    print(f"   {pd:>8.0%}{rec:>11.0%}{break_even(pd, rec, .01):>22.1%}{break_even(pd, rec, .01, .07):>21.1%}")

# 2. One bank for a year: equity in primes, fixed-rate deposits, optional Fed loan, 1,000 consumer loans.
def bank(rate, pd, rec, E=600e6, D=600e6, d=0.02, fed=0, fed_rate=0.05, n=1000, size=1e6,
         cost=0.01, reserve=0.10, runs=2000, seed=1):
    rnd = random.Random(seed); roes = []
    lent = n * size
    funds = E + D + fed
    assert lent <= funds * (1 - reserve), "full reserve: not enough funds"
    idle = funds - lent                                   # kept as primes, earns y
    for _ in range(runs):
        defaults = sum(rnd.random() < pd for _ in range(n))
        interest = (n - defaults) * size * rate
        loss = defaults * size * (1 - rec)
        profit = interest - loss + idle * y - D * d - fed * fed_rate - lent * cost
        roes.append(profit / E)
    roes.sort()
    return sum(roes) / runs, roes[int(.01 * runs)]

print("\n2. A bank: 600M own units, 600M deposits at 2%, 1,000 loans of 1M micro-units ($1,000 each), costs 1%:")
print(f"   {'loan rate':>9}{'defaults':>10}{'return on equity':>18}{'worst 1-in-100 year':>21}")
for rate, pd, rec in ((0.08, 0.02, 0.5), (0.12, 0.05, 0.3), (0.12, 0.10, 0.2), (0.20, 0.10, 0.2)):
    m, w = bank(rate, pd, rec)
    print(f"   {rate:>8.0%}{pd:>10.0%}{m:>17.1%}{w:>20.1%}")
m, w = bank(0.12, 0.05, 0.3, D=0, fed=600e6)
print(f"   same at 12%/5%, funded by the Fed at 5% instead of deposits: {m:.1%} (worst {w:.1%})")
m, w = bank(0.12, 0.05, 0.3, D=0, n=540)
print(f"   same at 12%/5%, own units only, 540 loans (no deposits):    {m:.1%} (worst {w:.1%})")

print("\n3. The borrower's cost in dollars (borrows 1M micro-units = $1,000 at 12%, repays 1.12M after a year):")
for ch in (-0.30, -0.15, 0.0, 0.15, 0.30):
    repay = 1.12e6 * PRICE * (1 + ch)
    print(f"   micro-unit price {ch:+.0%} over the year: repays ${repay:,.0f} -> {repay/1000-1:+.0%} in dollars")
