#!/usr/bin/env python3
"""How much money do rings issue under options A-D (docs/monetary-layer.md)? Exploratory model.

Main chain: one block per prime, 1 unit of R = one main reward (1,000,000 micro-units).
Rings: the 351 certified quadratic PIDs with |D| <= 2209, real splitting (Kronecker symbols).
A paid row issues R * rule; 50% miner, 35% owners of the prime, 15% founder.
Ring miners have a limited number of rows per block and take the best-paid rows first."""
import math, os, random, sys
from collections import defaultdict
HERE = os.path.dirname(os.path.abspath(__file__))
N = 10000                                     # main blocks
STEP = 10
def sieve(n):
    s = bytearray([1]) * (n + 1); s[0] = s[1] = 0
    for i in range(2, int(n**.5) + 1):
        if s[i]: s[i*i::i] = bytearray(len(s[i*i::i]))
    return [i for i in range(n + 1) if s[i]]
P = sieve(110000)[:N]
def kron(a, n):                               # Jacobi symbol, n odd
    a %= n; t = 1
    while a:
        while a % 2 == 0:
            a //= 2
            if n % 8 in (3, 5): t = -t
        a, n = n, a
        if a % 4 == 3 and n % 4 == 3: t = -t
        a %= n
    return t if n == 1 else 0
DS = sorted((int(l.split()[1][2:]) for l in open(os.path.join(HERE, '..', 'extensions', 'quadratic_vectors.txt'))
             if l.startswith('FIELD')), key=lambda d: (abs(d), d))
PAID = []                                     # paid[j][i] = 1 if prime number i splits or ramifies in ring j
for D in DS:
    t = {}; a = bytearray(N); m = abs(D)
    for i, p in enumerate(P):
        if p == 2: a[i] = D % 2 == 0 or D % 8 == 1; continue
        r = p % m
        k = (r, p % 4) if m % 4 else r
        if k not in t: t[k] = kron(D, p) != -1
        a[i] = t[k]
    PAID.append(a)

WSUM = [0.0]
for _j in range(1, 400): WSUM.append(WSUM[-1] + _j**-.5)

class Rule:
    """w(j, i, n, q): share of R for ring number j (1, 2, ...), its n-th paid row, prime index i."""
    budget = None; cohort = None
    def __init__(s, name, w, **k): s.name = name; s.w = w; s.__dict__.update(k)
R_ = [
 Rule('flat', lambda s, j, n, i: 1.0),
 Rule('A  budget 1 per block', lambda s, j, n, i: 1.0, budget=1.0),
 Rule('A2 budget per prime, shared', None, perprime=True, back=0.1),
 Rule('A3 as A2, older rings more', None, perprime=True, back=0.1, weight=True),
 Rule('B  cohort of 50: 5000', None, cohort=(50, 5000.0, 1/5000)),
 Rule('C1 geometric, total 2000', lambda s, j, n, i: math.exp(-n/2000)),
 Rule('C2 hyperbolic, half at 1000', lambda s, j, n, i: 1/(1 + n/1000)),
 Rule('C3 halving every 1000', lambda s, j, n, i: .5**(n//1000)),
 Rule('D1 1/j', lambda s, j, n, i: 1/j),
 Rule('D2 1/sqrt(j)', lambda s, j, n, i: j**-.5),
 Rule('D3 1/(j(j+1))', lambda s, j, n, i: 1/(j*(j+1))),
 Rule('C1+A', lambda s, j, n, i: math.exp(-n/2000), budget=1.0),
 Rule('D1+C2', lambda s, j, n, i: 1/(1 + n/1000)/j),
]
def run(rule, opens, cap, owner_of=None):
    """opens: list of (block, ring index); cap: rows per block over all rings."""
    opens = sorted(opens); no = 0; front = []; num = []; paidn = []; tot = []; born = []
    issue = [0.0]*(N//STEP); up = [0.0]*N; upt = defaultdict(float); rowpay = defaultdict(list)
    cohort = {}; nopen = [0]*N; back = 0.0; lastup = 0.0; firstyear = []
    b0 = 0
    for st in range(N//STEP):
        b = (st + 1)*STEP
        while no < len(opens) and opens[no][0] < b:
            front.append(0); num.append(opens[no][1]); paidn.append(0); tot.append(0.0); born.append(opens[no][0]); no += 1
        for i in range(b - STEP, b): nopen[i] = len(num)
        left = cap*STEP
        def share(x, q):                      # of the budget of one prime; older rings count more in A3
            K = nopen[q] if q >= born[x] else len(num); W = getattr(rule, 'weight', 0)
            f = (x + 1)**-.5/WSUM[K] if W else 1.0/K
            return min(1.0, 2*f)*(1.0 if q >= born[x] else rule.back)
        def nextw(x):
            j = x + 1
            if rule.cohort:
                g = x//rule.cohort[0]; return cohort.get(g, rule.cohort[1])*rule.cohort[2]
            if getattr(rule, 'perprime', 0):
                i = min(front[x], N-1); return share(x, i)
            return rule.w(rule, j, paidn[x], 0)
        order = sorted(range(len(num)), key=lambda x: -nextw(x))
        lim = b - 1; pend = []
        for x in order:
            if left <= 0: break
            a = PAID[num[x]]; i = front[x]; e = min(lim, i + left); left -= e - i
            for q in range(i, e):
                if a[q]:
                    if rule.cohort:
                        g = x//rule.cohort[0]; c = cohort.get(g, rule.cohort[1]); w = c*rule.cohort[2]; cohort[g] = c - w
                    elif getattr(rule, 'perprime', 0):
                        # ~half the open rings pay for a prime, so 2/K of the budget each, at most 1
                        w = share(x, q)
                    else: w = rule.w(rule, x + 1, paidn[x], q)
                    paidn[x] += 1; pend.append((x, q, w))
            front[x] = e
        if rule.budget is not None and pend:
            s = sum(w for _, _, w in pend); f = min(1.0, rule.budget*STEP/s)
        else: f = 1.0
        for x, q, w in pend:
            w *= f; issue[st] += w; tot[x] += w; up[q] += .35*w
            if q < born[x] - 1: back += w
        if pend: rowpay[st] = sum(w for _, _, w in pend)*f/len(pend)
        if b > .9*N: lastup += .35*sum(w for _, _, w in pend)*f
    return dict(issue=issue, tot=tot, up=up, born=born, rowpay=rowpay, back=back, lastup=lastup, behind=sum(N - 1 - f for f in front), rows=sum(front))
def pct(v, p): v = sorted(v); return v[min(len(v)-1, int(p*len(v)))] if v else 0
def scen():
    R = random.Random(7)
    few = [(int(i*N/12), i) for i in range(12)]
    many = [(int(i*N/351), i) for i in range(351)]
    early = [(0, i) for i in range(351)]
    late = few + [(8000, i) for i in range(12, 312)]
    return [('12 rings, spread out', few), ('351 rings, spread out', many), ('351 rings at the start', early), ('12 rings, then 300 at block 8000', late)]
def table(caps, pick=None):
    print('%d certified rings, %d blocks; amounts in main rewards (R). "x main" = ring money / main money' % (len(DS), N))
    for cap, cname in caps:
        for name, op in [x for k, x in enumerate(scen()) if pick is None or k in pick]:
            print('\n%s | %s' % (name, cname))
            print('  %-30s %7s %9s %8s %22s %9s %20s %8s %7s' % ('rule', 'x main', 'peak/100b', 'end/blk', 'founder first/med/last', 'row pay', 'owner p10/med/p90', 'yield%', 'behind'))
            for r in R_:
                if ONLY and r.name.split()[0] not in ONLY: continue
                o = run(r, op, cap); iss = o['issue']; T = sum(iss)
                w = 100//STEP; pk = max(sum(iss[k:k+w]) for k in range(len(iss)-w+1))/100
                f = [.15*t for t in o['tot']]; end = sum(iss[-50:])/(50*STEP)
                u = o['up'][2:N//2]; lp = [v for k, v in o['rowpay'].items() if k > .9*N/STEP]
                print('  %-30s %7.2f %9.1f %8.2f %7.0f/%6.1f/%6.1f %9.4f %6.2f/%5.2f/%5.2f %8.2f %6.0f%%' % (r.name, T/N, pk, end,
                    f[0], pct(f, .5), f[-1], sum(lp)/len(lp) if lp else 0, pct(u, .1), pct(u, .5), pct(u, .9),
                    100*o['lastup']/N/(.1*N/1000), 100*o['behind']/(o['behind'] + o['rows'])))
def attack():
    print('\nOne user opens n rings at block 5000 and mines only them; 40 honest rings opened before. Fast mining.')
    base = [(i*100, i) for i in range(40)]
    print('  %-30s %s' % ('rule', 'share of all money for n = 1 / 10 / 100 / 300 | honest founders\' later income with 300 (100% = none)'))
    for r in R_:
        if ONLY and r.name.split()[0] not in ONLY: continue
        o0 = run(r, base, 2000); out = []
        for n in (1, 10, 100, 300):
            o = run(r, base + [(5000, 40 + i) for i in range(n)], 2000)
            gain = .65*sum(o['tot'][40:]); out.append(100*gain/(N + sum(o['tot'])))
        # honest income after block 5000
        a = run_cut(r, base, 5000); later0 = sum(o0['tot']) - a; later1 = sum(o['tot'][:40]) - a
        print('  %-30s %6.2f%% %6.2f%% %6.2f%% %6.2f%%  | %4.0f%%' % (r.name, *out, 100*later1/later0))
def run_cut(rule, opens, cut):
    global N
    keep = N
    try: N = cut; return sum(run(rule, opens, 2000)['tot'])
    finally: N = keep
if __name__ == '__main__':
    FAST, SLOW = (2000, 'fast ring mining (2000 rows per block)'), (40, 'slow ring mining (40 rows per block)')
    a = sys.argv[1:]; ONLY = set(a[1].split(',')) if len(a) > 1 else None
    if a[0] == 'fast': table([FAST])
    if a[0] == 'slow': table([SLOW], (1, 3))
    if a[0] == 'attack': attack()
