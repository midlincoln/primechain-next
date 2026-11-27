# A bank with deposits, reserves at its validator and the Fed's discount window (exploratory, 2026-11-27).
# One year, daily steps. Year-11 economy, rule A4: holder yield y = 3.5%; reserves and free funds are primes,
# so they earn y. Balance sheet (micro-units): capital E; demand deposits Dd (withdraw any time, rate rd);
# term deposits Dt (1 year, rate rt, no early withdrawal); ring bonds B (collateral at the Fed, haircut).
# Loans are funded by E, Dt and the part of Dd not kept in reserve. Fund shares carry no promise: left out.
import random
y, LOAN, PD, REC, COST = 0.035, 0.12, 0.05, 0.30, 0.01
FED, HAIR, BUF = 0.05, 0.30, 0.02

def year(rho, rt=0.03, rd=0.0, run=0.0, E=600e6, Dd=400e6, Dt=600e6, B=300e6, seed=1):
    rnd = random.Random(seed)
    liquid = rho * Dd + BUF * (E + Dd + Dt)            # reserves + a small free buffer, all held as primes
    loans = E + Dd + Dt - liquid
    net_loan = LOAN * (1 - PD) - PD * (1 - REC) - COST  # net yearly return per unit lent
    dd, fed, fed_peak, failed = Dd, 0.0, 0.0, False
    fed_max = B * (1 - HAIR)
    for day in range(365):
        liquid += (loans * net_loan + liquid * y - Dt * rt - dd * rd - fed * FED) / 365
        out = dd * rnd.gauss(0, 0.005)
        if 100 <= day < 105: out += Dd * run / 5
        dd -= out; liquid -= out
        if liquid < 0:                                   # out of money: borrow at the discount window
            take = min(fed_max - fed, -liquid); fed += take; liquid += take
            if liquid < -1: failed = True
        short = rho * dd - liquid                        # reserve rule: reserves >= rho * demand deposits
        if short > 0:
            take = min(fed_max - fed, short); fed += take; liquid += take
        if fed > 0 and liquid > rho * dd:                # repay the Fed with any surplus
            r = min(fed, liquid - rho * dd); fed -= r; liquid -= r
        fed_peak = max(fed_peak, fed)
    equity = liquid + loans - dd - Dt - fed
    return (equity - E) / E, fed_peak, failed

if __name__ == "__main__":
    print("Bank: 600M own units, 400M demand deposits (0%), 600M term deposits (3%), 300M ring bonds;")
    print("loans at 12%, 5% defaults, 30% recovered; Fed: 5%, 30% haircut on ring bonds (up to 210M).\n")
    print(f"{'reserve ratio':>14}{'lent out':>10}{'run':>6}{'return on capital':>19}{'peak Fed loan':>15}{'fails?':>8}")
    for rho in (1.0, 0.5, 0.2, 0.0):
        for run in (0.0, 0.4, 0.8):
            roe, peak, failed = year(rho, run=run)
            lent = 1600e6 - rho * 400e6 - BUF * 1600e6
            print(f"{rho:>13.0%}{lent/1e6:>9.0f}M{run:>6.0%}{100*roe:>18.1f}%{peak/1e6:>13.0f}M{'YES' if failed else 'no':>8}")
    print("\nWhat the bank can pay depositors (reserve 50%, no run):")
    for rt, rd in ((0.02, 0.0), (0.05, 0.0), (0.05, 0.02), (0.08, 0.03)):
        print(f"   term {rt:.0%}, demand {rd:.0%} -> return on capital {100*year(0.5, rt=rt, rd=rd)[0]:.1f}%")
