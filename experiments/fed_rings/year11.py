# Year 11 with k rings accepted per year from year 1 (exploratory, 2026-11-26).
# Unit: one year of main-chain issuance = 1. Constant speed, no halvings.
# A row for main prime q pays the holders of q's units. Primes mined after the ring opened pay
# the full A3 share (goes to whoever holds the fresh prime, mostly its miner); primes mined before
# pay the backlog share (10%) when the ring opens = income to people already holding primes.
# Rule A3: one budget b per prime shared by all rings. Rule C: budget = c per accepted ring.
def run(k, rule, b=1.0, c=0.01, back=0.10, Y=11):
    main = ring = 0.0; N = 0; W = 0.0
    for y in range(1, Y + 1):
        main0, tot0 = main, main + ring
        bl = 0.0
        for _ in range(k):
            N += 1; w = N ** -0.5; W += w
            B = b if rule == 'A3' else c * N
            bl += back * B * main0 * w / W          # backlog on all older primes (avg one prime of norm q per row)
        Bnow = (b if N else 0) if rule == 'A3' else c * N
        new = 1 + Bnow + bl                          # main + rows of fresh primes + backlog
        main += 1; ring += Bnow + bl
    return dict(growth=new / tot0, main_growth=1 / main0, yield_=bl / main0, ring_share=ring / (main + ring),
                rings=N, budget=Bnow, supply=main + ring)
print(f"{'rule':<26}{'rings':>6}{'supply growth':>15}{'main-prime yield':>18}{'ring money share':>18}{'supply':>8}")
for rule, lab in (('A3', 'A3, budget = main reward'), ('C', 'per-ring 1% each')):
    for k in (0, 10, 20):
        r = run(k, rule)
        print(f"{lab:<26}{r['rings']:>6}{100*r['growth']:>14.1f}%{100*r['yield_']:>17.2f}%{100*r['ring_share']:>17.0f}%{r['supply']:>8.1f}")
