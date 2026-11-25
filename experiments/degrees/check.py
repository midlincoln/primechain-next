#!/usr/bin/env python3
"""Checks the PARI files with degree.py and reports what comes up. python3 check.py file ..."""
import sys, time, random
from collections import Counter, defaultdict
from degree import *
import forms
R = random.Random(1)
def size(v): return sum(1 + max(1, (abs(x).bit_length() + 8)//8) for x in v)
def load(fn):
    for l in open(fn):
        l = l.replace('~', '')
        w = l.split()
        if w[0] == 'NOPID': yield None, (w[1], int(w[2]), int(w[3])); continue
        if w[0] != 'FIELD': continue
        head, gens, rows, units, gal = l.split(' ; '); h = head.split(' ', 5)
        yield h[1], (int(h[2]), int(h[3]), int(h[4]), eval(h[5]), dict((a, b) for a, b in eval(gens)), eval(rows), eval(units), gal.strip())
def by_degrees(T):
    """Some prime (or two) whose residue degrees rule out every factorization; else None (undecided)."""
    n = len(T); poss = set(range(n+1))
    D = discriminant(T)
    for p in primes(300):
        if D % p == 0: continue
        s = {0}
        for f in degrees(T, p): s |= {a+f for a in s}
        poss &= s
        if poss == {0, n}: return p
    return None
def types(T, D, lim): return {p: tuple(degrees(T, p)) for p in primes(lim) if D % p}
def run(fn):
    ok = bad = rows = npid = undecided = caught = tried = wrong_e = 0; t0 = time.time(); tm = {'cert': 0, 'row': 0}
    paid = Counter(); blocks = 0; fields = []; mx = defaultdict(int); gal = Counter(); pergen = Counter(); split2 = 0; index = []; pad = []; slack = []
    for tag, x in load(fn):
        if tag is None: npid += 1; continue
        n, r2, D, T, gens, rws, units, G = x; gal[G] += 1
        if tag.count(',') == 3:
            f = tuple(map(int, tag.split(','))); assert forms.reduced(*f) and forms.disc(*f) == D and forms.table(*f) == T
        t = time.time(); res = check_field(T, D, r2, gens); tm['cert'] += time.time() - t
        ok += res[0]; bad += not res[0]
        if not res[0]: print('  FAILED', tag, D, res)
        if by_degrees(T) is None: undecided += 1
        for u in units: assert abs(norm(T, u)) == 1
        mx['certificate bytes'] = max(mx['certificate bytes'], sum(size(g) for v in gens.values() for g in v) + sum(size(u) for u in units) + sum(size(v) for a in T for v in a))
        for q, gs in rws:
            t = time.time(); out = check_row(T, q, [g for g, _ in gs]); tm['row'] += time.time() - t
            want = sorted((label(T, g, q), e) for g, e in gs); wrong_e += out != want; rows += 1
            paid[D % q == 0] += sum(e for _, e in out) / n; pergen[len(out)] += 1
            for g, _ in gs: mx['generator bytes'] = max(mx['generator bytes'], size(g))
            if len(gs) > q or len(gs) == n and q < n: index.append((D, q))
            blocks += 1
            # last generator from the others
            if len(gs) == n and all(e == 1 for _, e in gs): split2 += 1
            # corruptions
            def reject(g2):
                nonlocal caught, tried
                tried += 1
                try: check_row(T, q, g2)
                except (ValueError, AssertionError): caught += 1; return
                print('  NOT CAUGHT', D, q, g2)
            if gs and R.random() < .1:
                g = [list(a) for a, _ in gs]; k = R.randrange(n); g[0][k] += 1
                if abs(norm(T, g[0])) != q: reject(g)
                reject([list(a) for a, _ in gs][1:])                   # one missing
                if len(gs) > 1: reject([list(gs[0][0])]*len(gs))       # repeated
                if units: reject([mul(T, gs[0][0], units[0])] + [list(a) for a, _ in gs])   # associate as an extra
        for how in range(3):                                           # corrupted certificates
            T2 = [[list(v) for v in a] for a in T]; g2 = {p: [list(v) for v in vs] for p, vs in gens.items()}; D2 = D
            if how == 0: T2[1][1][0] += 1
            elif how == 1: D2 = D + 4
            else:
                ps = [p for p in g2 if g2[p]]
                if not ps: continue
                g2[ps[0]] = g2[ps[0]][1:]
            tried += 1; caught += not check_field(T2, D2, r2, g2)[0]
        if n >= 4 and r2 >= 2: tried += 1; caught += not check_field(T, D, r2 - 2, gens)[0]      # too small a Minkowski bound
        tried += 1; P2 = product(T); caught += check_field(P2, discriminant(P2), r2, {})[1] == 'not a field'  # Z x O is not a field
        # a proper sublattice is refused: replace the last basis element by 2 times it
        S = sub2(T)
        if S: tried += 1; caught += not check_field(S, discriminant(S), r2, gens)[0]
        for q, gs in rws:                                              # length cap from the units: every generator fits?
            cap = q.bit_length()/n + sum(max(abs(x).bit_length() for x in u + v) for u, v in zip(units, inverses(T, units)))
            for g, _ in gs: slack.append(max(abs(x).bit_length() for x in g) - cap)
        if units and R.random() < .4: pad += padding(T, units, [g for _, gs in rws[:8] for g, _ in gs])
        fields.append((D, T))
    print('%s: %d certificates ok, %d failed, %d non-PIDs listed; %d rows (%d wrong e); %d of %d corruptions caught' % (fn, ok, bad, npid, rows, wrong_e, caught, tried))
    print('  groups', dict(gal), '| irreducibility undecided by residue degrees:', undecided)
    print('  primes of norm q per row:', dict(sorted(pergen.items())), '| paid per row: %.3f (1/n = %.3f)' % (sum(paid.values())/max(1, rows), 1/n))
    print('  largest:', dict(mx), '| rows with all n primes, last one derivable:', split2, '| more primes of norm q than a polynomial allows:', len([1 for D, q in index]))
    print('  time per certificate %.1f ms, per row %.2f ms' % (1e3*tm['cert']/max(1, ok), 1e3*tm['row']/max(1, rows)))
    if slack: print('  cap bits(q)/n + sum of unit lengths: worst generator is %.1f bits above it, %d of %d above' % (max(slack), sum(x > 0 for x in slack), len(slack)))
    if pad: print('  padding: %d generators; after shortening by single units, longest/best bits: worst ratio %.2f, %d of %d at the best' % (len(pad), max(a/b for a, b in pad), sum(a == b for a, b in pad), len(pad)))
    by = defaultdict(list)
    for D, T in fields: by[D].append(T)
    w = []
    for D, Ts in by.items():
        if len(Ts) > 1:
            ty = [types(T, D, 300) for T in Ts]
            for i in range(len(Ts)):
                for j in range(i): w.append(next((p for p in ty[i] if ty[i][p] != ty[j][p]), None))
    print('  pairs with equal discriminant: %d, first prime that tells them apart: %s' % (len(w), sorted(w, key=lambda a: a or 10**9)))
def product(T):
    """Z x O on the basis 1 = (1,1), (0,1), (0,e_1), ..."""
    n = len(T); e = lambda i: unit_vec(n+1, i); S = [[None]*(n+1) for _ in range(n+1)]
    for i in range(n+1):
        S[0][i] = S[i][0] = e(i)
    for i in range(n):
        for j in range(n): S[i+1][j+1] = [0] + T[i][j]
    return S
def inverses(T, units):
    n = len(T); out = []
    for u in units:
        M = matrix(T, u); d = det(M)
        out.append([det([M[k] if k != i else unit_vec(n, 0) for k in range(n)])*d for i in range(n)])
    return out
def padding(T, units, gens):
    """Multiply by random units, then shorten by single units only; how far from the best length found?"""
    from itertools import product as pr
    bits = lambda v: sum(abs(x).bit_length() + 1 for x in v); out = []
    inv = []
    for u in units:
        n = len(T); M = matrix(T, u); d = det(M)               # inverse: solve x*u = 1 by Cramer
        sol = [det([M[k] if k != i else unit_vec(n, 0) for k in range(n)])*d for i in range(n)]
        assert mul(T, sol, u) == unit_vec(n, 0); inv.append(sol)
    steps = list(units) + inv
    for g in gens:
        res = []
        for _ in range(4):
            h = g
            for u, w in zip(units, inv):
                for _ in range(R.randrange(9)): h = mul(T, h, R.choice((u, w)))
            while True:
                b = min((mul(T, h, s) for s in steps), key=bits)
                if bits(b) >= bits(h): break
                h = b
            res.append(bits(h))
        out.append((max(res), min(res + [bits(g)])))
    return out
def sub2(T):
    n = len(T); S = [[list(v) for v in a] for a in T]
    for i in range(n):
        for j in range(n):
            v = mul(T, [x*(1 if i < n-1 else 2) for x in unit_vec(n, i)], [x*(1 if j < n-1 else 2) for x in unit_vec(n, j)])
            if v[-1] % 2: return None
            v[-1] //= 2; S[i][j] = v
    return S
for fn in sys.argv[1:]: run(fn)
