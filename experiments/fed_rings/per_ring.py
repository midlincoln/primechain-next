# Year 11 if parameter 17 is the value of EACH new ring prime (v, in main rewards), not shared by rings.
# Rows for fresh main primes pay v each (avg one prime of norm q per row); a newly opened ring also pays
# the backlog share s (param 19) on all older main primes. Each row splits 50% ring miner / 35% holders of
# the main prime (param 21) / 15% founder. Unit: one year of main issuance = 1.
def run(k, v, s=0.10, up=0.35, Y=11):
    main = ring = 0.0; N = 0
    for y in range(1, Y + 1):
        tot0, main0 = main + ring, main
        back = k * s * v * main0; N += k
        new = 1 + N * v + back
        main += 1; ring += N * v + back
    return new / tot0, up * back / main0, main + ring
print(f"{'v (value of a ring prime)':<27}{'rings/yr':>9}{'supply growth y11':>19}{'holder yield y11':>18}{'supply y11':>12}")
for v, lab in ((1, '1,000,000 (today)'), (0.1, '100,000'), (0.01, '10,000')):
    for k in (10, 20):
        g, yl, S = run(k, v)
        print(f"{lab:<27}{k:>9}{100*g:>18.1f}%{100*yl:>17.1f}%{S:>12.1f}")
