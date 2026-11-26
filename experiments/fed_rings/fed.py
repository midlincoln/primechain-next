# Money supply vs rings accepted by the Fed (exploratory, 2026-11-26).
# Unit: main-chain issuance per year = 1 (constant speed, no halvings).
# A3 (current rule): all rings together share one budget b per main prime -> ring money per year = b,
#   whatever the ring count; opening a ring also pays a one-off backlog: 10% of its share of all older primes.
# PR (alternative "per-ring" rule): each accepted ring earns c per year (c = its budget as a share of main issuance).
import sys
Y = 20
def a3(rings_per_year, b=1.0, back=0.10):
    S, out, N, W = 0.0, [], 0, 0.0
    for y in range(1, Y+1):
        add = 1 + b
        for _ in range(rings_per_year):
            N += 1; w = N ** -0.5; W += w
            add += back * b * (y - 1) * w / W      # backlog of the new ring over older years
        g = add / S if S else None; S += add; out.append((y, N, S, g))
    return out
def pr(rings_per_year, c):
    S, out, N = 0.0, [], 0
    for y in range(1, Y+1):
        N += rings_per_year; add = 1 + c * N
        g = add / S if S else None; S += add; out.append((y, N, S, g))
    return out
def need10(c):          # rings the Fed must have open each year to hold 10% growth, per-ring rule
    S, out = 0.0, []
    for y in range(1, Y+1):
        need = 0.10 * S if S else 1
        N = max(0, -(-(need - 1) // c)) if need > 1 else 0
        add = 1 + c * N; S += add; out.append((y, int(N), S, add / (S - add) if S - add else None))
    return out
f = lambda g: '  -  ' if g is None else f'{100*g:5.1f}%'
for k in (1, 10, 100):
    print(f'A3, b=1, {k} rings/yr:', ' '.join(f(g) for y, n, s, g in a3(k) if y in (2, 5, 11, 20)), f' supply y20={a3(k)[-1][2]:.1f}')
print('A3, b=0 (no rings):', ' '.join(f(g) for y, n, s, g in a3(0, 0)), )
for k, c in ((1, .01), (10, .01), (100, .01)):
    r = pr(k, c); print(f'PR c=1%, {k} rings/yr:', ' '.join(f(g) for y, n, s, g in r if y in (2, 5, 11, 20)), f' supply y20={r[-1][2]:.1f}')
print('PR c=1%, rings needed for 10%:', [(y, n) for y, n, s, g in need10(.01)])
print('supply:', [round(s, 1) for y, n, s, g in need10(.01)])
