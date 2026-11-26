#!/usr/bin/env python3
"""A: how does a miner find generators in a cubic (or any degree) PID?

Lattice method: the prime P of norm q is a lattice; LLL (under the complex embeddings) gives
short elements of P with norm q*m, m small. If m = +-1 it is a generator; otherwise divide by
generators of the primes dividing m, taken from rows already on the chain (the chain is the
miner's factor base). Then shorten with the units. Checked with degree.py."""
import cmath, math, random, sys, time, itertools
from fractions import Fraction
from degree import *
R = random.Random(5)
def embeddings(T):
    """sigma(e_k) for each complex embedding, numerically (simultaneous eigenvectors)."""
    n = len(T)
    for v in itertools.product(range(-2, 3), repeat=n-1):
        a = [0] + list(v); c = charpoly_matrix(matrix(T, a))
        roots = poly_roots(c)
        if min((abs(x - y) for x, y in itertools.combinations(roots, 2)), default=1) > 1e-6: break
    A = matrix(T, a)                           # rows: a*e_j; for a hom s, s(a)s(e_j) = sum A[j][k] s(e_k)
    out = []
    for lam in roots:
        M = [[A[j][k] - (lam if j == k else 0) for k in range(n)] for j in range(n)]
        s = null_vector(M)                     # s = (s(e_0), ..., s(e_{n-1})), normalised s(e_0) = 1
        out.append([x/s[0] for x in s])
    return out
def poly_roots(c):                             # Durand-Kerner, c low to high monic
    n = len(c) - 1; z = [complex(.4, .9)**k for k in range(n)]
    for _ in range(500):
        z = [zi - (sum(c[k]*zi**k for k in range(n+1)))/math.prod((zi - zj) for j, zj in enumerate(z) if j != i) for i, zi in enumerate(z)]
    return z
def null_vector(M):
    n = len(M); M = [r[:] for r in M]; piv = []
    for c in range(n):
        r = max(range(len(piv), n), key=lambda i: abs(M[i][c]), default=None)
        if r is None or abs(M[r][c]) < 1e-9: continue
        M[len(piv)], M[r] = M[r], M[len(piv)]; p = M[len(piv)]
        for i in range(n):
            if i != len(piv): f = M[i][c]/p[c]; M[i] = [x - f*y for x, y in zip(M[i], p)]
        piv.append(c)
    free = [c for c in range(n) if c not in piv][0]; s = [0j]*n; s[free] = 1
    for i, c in enumerate(piv): s[c] = -M[i][free]/M[i][c]
    return s
def lll(B, gram, delta=.99):
    B = [b[:] for b in B]; n = len(B)
    def ip(x, y): return gram(x, y)
    k = 1
    while k < n:
        Bs = []; mu = [[0]*n for _ in range(n)]
        for i in range(n):
            v = [complex(x) for x in B[i]]
            for j in range(i):
                mu[i][j] = ip(B[i], Bs[j])/ip(Bs[j], Bs[j]); v = [a - mu[i][j]*b for a, b in zip(v, Bs[j])]
            Bs.append(v)
        for j in range(k-1, -1, -1):
            r = round(mu[k][j].real)
            if r: B[k] = [a - r*b for a, b in zip(B[k], B[j])]; mu = None; break
        if mu is None: continue
        if ip(Bs[k], Bs[k]).real >= (delta - abs(mu[k][k-1])**2)*ip(Bs[k-1], Bs[k-1]).real: k += 1
        else: B[k], B[k-1] = B[k-1], B[k]; k = max(k-1, 1)
    return B
def solve_div(T, x, g):                        # y with g*y = x, or None
    n = len(T); M = [list(r) for r in zip(*matrix(T, g))]; d = det(M)
    y = []
    for i in range(n):
        Mi = [r[:] for r in M]
        for r in range(n): Mi[r][i] = x[r]
        v = det(Mi)
        if v % d: return None
        y.append(v//d)
    return y
class Miner:
    def __init__(s, T, units):
        s.T = T; s.n = len(T); s.emb = embeddings(T); s.units = units; s.base = {}   # p -> [(label, generator)]
        s.inv = []
        for u in units:
            M = matrix(T, u); dd = det(M); s.inv.append([det([M[k] if k != i else unit_vec(s.n, 0) for k in range(s.n)])*dd for i in range(s.n)])
    def sig(s, x): return [sum(a*e for a, e in zip(x, em)) for em in s.emb]
    def gram(s, x, y): return sum(a*b.conjugate() for a, b in zip(s.sig(x), s.sig(y)))
    def homs(s, q):                            # labels of all primes of norm q (brute force over values of e_1 for small q, else roots)
        n, T = s.n, s.T; out = []
        c = charpoly_matrix(matrix(T, unit_vec(n, 1)))
        for r in roots_mod([x % q for x in c], q):
            # determine the other basis values from e_1 -> r, when e_1 generates mod q; else fall back to search
            l = solve_hom(T, r, q)
            out += [x for x in l if x not in out]
        if len(out) < degrees(T, q).count(1):  # q divides the index of Z[e_1]: search all
            out = [l for l in itertools.product(range(q), repeat=n-1) if is_hom(T, (1,) + l, q)] if q**(n-1) < 2*10**6 else out
            out = [(1,) + tuple(l) if len(l) == n-1 else l for l in out]
        return sorted(set(tuple(l) for l in out))
    def mine(s, q, label, stats):
        n, T = s.n, s.T
        B = [[q] + [0]*(n-1)] + [[(-label[k]) % q if i == 0 else int(i == k) for i in range(n)] for k in range(1, n)]
        B = lll(B, s.gram)
        tried = 0
        for coef in sorted(itertools.product(range(-2, 3), repeat=n), key=lambda c: sum(map(abs, c))):
            if not any(coef): continue
            x = [sum(c*b[i] for c, b in zip(coef, B)) for i in range(n)]; tried += 1
            N = norm(T, x)
            if N == 0: continue
            m = abs(N)//q
            while m > 1:
                p = next(p for p in range(2, m+1) if m % p == 0)
                for l, g in s.base.get(p, []):
                    if sum(a*b for a, b in zip(x, l)) % p == 0:
                        y = solve_div(T, x, g)
                        if y: x = y; m //= p; break
                else: break
            if m == 1:
                stats['tried'] += tried; stats['divided'] += abs(N) != q
                return s.shorten(x)
        stats['failed'] += 1; return None
    def shorten(s, x):
        bits = lambda v: max(abs(a).bit_length() for a in v)
        while True:
            b = min((mul(s.T, x, u) for u in s.units + s.inv), key=bits, default=x)
            if bits(b) >= bits(x): return x
            x = b
def roots_mod(c, q):
    if q < 3000: return [r for r in range(q) if sum(a*pow(r, k, q) for k, a in enumerate(c)) % q == 0]
    fs = factor_mod(monic([x % q for x in c], q), q) if len(c) > 1 else []
    out = []
    for f in fs:
        if len(f) == 2: out.append((-f[0]*pow(f[1], -1, q)) % q)
    # repeated roots: gcd with derivative already removed by squarefree factor_mod; add them
    d = pgcd([x % q for x in c], pmod([k*x for k, x in enumerate(c)][1:], [x % q for x in c], q) if len(c) > 2 else [1], q)
    if len(d) > 1:
        for f in factor_mod(d, q) if len(d) > 2 else [d]:
            if len(f) == 2: out.append((-f[0]*pow(f[1], -1, q)) % q)
    return sorted(set(out))
def solve_hom(T, r, q):                        # homs with e_1 -> r: other values from the relations e_1*e_k
    n = len(T); res = []
    # linear system for (l_2..l_{n-1}) : r*l_k = sum T[1][k][j] l_j
    import itertools as it
    rows = []; rhs = []
    for k in range(2, n):
        row = [(T[1][k][j] - (r if j == k else 0)) % q for j in range(2, n)]
        rows.append(row); rhs.append((-(T[1][k][0] + T[1][k][1]*r)) % q)
    if n == 2: cand = [(1, r)]
    else:
        A = rref([rw + [b] for rw, b in zip(rows, rhs)], q)
        if len(A) < n-2 or any(not any(a[:-1]) and a[-1] for a in A): return []
        sol = [0]*(n-2)
        for a in A:
            k = next(i for i, v in enumerate(a[:-1]) if v); sol[k] = a[-1]
        cand = [(1, r) + tuple(sol)]
    return [c for c in cand if is_hom(T, c, q)]
def load(fn):
    for l in open(fn):
        if not l.startswith('FIELD'): continue
        l = l.replace('~', ''); head, gens, rows, units, gal = l.split(' ; '); h = head.split(' ', 5)
        yield int(h[4]), eval(h[5]), dict((a, b) for a, b in eval(gens)), eval(units)
if __name__ == '__main__':
    fn, QMAX = sys.argv[1], int(sys.argv[2]); nf = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    tot = {'rows': 0, 'tried': 0, 'divided': 0, 'failed': 0, 'time': 0., 'bits': 0}
    for k, (D, T, gens, units) in enumerate(load(fn)):
        if k >= nf: break
        Mi = Miner(T, units)
        for p, gs in gens.items():               # the certificate starts the factor base
            for g in gs:
                if prime_degree(T, g, p) == 1: Mi.base.setdefault(p, []).append((label(T, g, p), g))
        for q in primes(QMAX):
            t = time.time(); ls = Mi.homs(q); found = []
            for l in ls:
                g = Mi.mine(q, l, tot)
                if g is None: continue
                assert abs(norm(T, g)) == q and label(T, g, q) == l
                found.append((l, g)); tot['bits'] = max(tot['bits'], max(abs(a).bit_length() for a in g))
            Mi.base[q] = found
            tot['time'] += time.time() - t; tot['rows'] += 1
            if len(found) != len(ls): tot['failed'] += 0
    print('%s: %d rows to %d in %d fields: %.2f ms per row; %.1f candidates tried per prime; %d needed division by chain rows; %d not found; longest coefficient %d bits'
          % (fn, tot['rows'], QMAX, min(k+1, nf), 1e3*tot['time']/tot['rows'], tot['tried']/max(1, tot['rows']), tot['divided'], tot['failed'], tot['bits']))
