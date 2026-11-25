"""Checker for a ring of integers of any degree, given its multiplication table. Integers only.

T[i][j] = coordinates of e_i e_j on the basis e_0 = 1, e_1, ..., e_{n-1}."""
from math import factorial
def mul(T, x, y):
    n = len(T); r = [0]*n
    for i, a in enumerate(x):
        if a:
            for j, b in enumerate(y):
                if b:
                    t = T[i][j]; ab = a*b
                    for k in range(n): r[k] += ab*t[k]
    return r
def unit_vec(n, i): return [int(k == i) for k in range(n)]
def matrix(T, x): return [mul(T, x, unit_vec(len(T), j)) for j in range(len(T))]   # rows: x*e_j
def det(M):
    M = [r[:] for r in M]; n = len(M); s = pv = 1
    for i in range(n-1):
        if M[i][i] == 0:
            for j in range(i+1, n):
                if M[j][i]: M[i], M[j] = M[j], M[i]; s = -s; break
            else: return 0
        for j in range(i+1, n):
            for l in range(i+1, n): M[j][l] = (M[j][l]*M[i][i] - M[j][i]*M[i][l])//pv
        pv = M[i][i]
    return s*M[-1][-1]
def norm(T, x): return det(matrix(T, x))
def is_ring(T):
    n = len(T); e = [unit_vec(n, i) for i in range(n)]
    return (all(T[0][i] == e[i] == T[i][0] for i in range(n)) and all(T[i][j] == T[j][i] for i in range(n) for j in range(n))
            and all(mul(T, T[i][j], e[k]) == mul(T, e[i], T[j][k]) for i in range(n) for j in range(n) for k in range(n)))
def discriminant(T):
    n = len(T); tr = [sum(T[i][j][j] for j in range(n)) for i in range(n)]
    return det([[sum(c*t for c, t in zip(T[i][j], tr)) for j in range(n)] for i in range(n)])
def factor(m):
    m = abs(m); f = {}; p = 2
    while p*p <= m:
        while m % p == 0: f[p] = f.get(p, 0) + 1; m //= p
        p += 1
    if m > 1: f[m] = f.get(m, 0) + 1
    return f
# ---- linear algebra mod p
def rref(rows, p):
    R = []; rows = [[x % p for x in r] for r in rows]; n = len(rows[0]) if rows else 0; c = 0
    while c < n and rows:
        k = next((i for i, r in enumerate(rows) if r[c]), None)
        if k is not None:
            r = rows.pop(k); v = pow(r[c], -1, p); r = [x*v % p for x in r]
            rows = [[(x - s[c]*y) % p for x, y in zip(s, r)] if s[c] else s for s in rows]
            R = [[(x - s[c]*y) % p for x, y in zip(s, r)] if s[c] else s for s in R]; R.append(r)
        c += 1
    return R
def kernel(M, p):                              # row vectors v with v M = 0
    m = len(M); n = len(M[0]); A = rref([list(M[i]) + unit_vec(m, i) for i in range(m)], p)
    return [r[n:] for r in A if not any(r[:n])]
def power(T, x, e, p):
    r = unit_vec(len(T), 0); x = [v % p for v in x]
    while e:
        if e & 1: r = [v % p for v in mul(T, r, x)]
        x = [v % p for v in mul(T, x, x)]; e >>= 1
    return r
def frobenius(T, p): return [power(T, unit_vec(len(T), i), p, p) for i in range(len(T))]
def matmul(A, B, p): return [[sum(a*b for a, b in zip(r, c)) % p for c in zip(*B)] for r in A]
def radical(T, p, F=None):                     # kernel of x -> x^(p^k), p^k >= n
    n = len(T); F = F or frobenius(T, p); G = F; q = p
    while q < n: G = matmul(G, F, p); q *= p
    return kernel(G, p)
def maximal_at(T, p):                          # Pohst-Zassenhaus: multipliers of the radical are only O
    n = len(T); rad = radical(T, p)
    if not rad: return True
    # I = rad + pO; x/p in (I:I) iff x*i in pI for all i in I
    basis = rref(rad, p); piv = [next(k for k, v in enumerate(r) if v) for r in basis]
    I = [r[:] for r in basis] + [[p*v for v in unit_vec(n, k)] for k in range(n) if k not in piv]
    order = [next(k for k, v in enumerate(r) if v) for r in I]
    def coords(v):                             # coordinates of v in the lattice I (triangular up to order)
        v = v[:]; out = [0]*n
        for k, r in enumerate(I):              # rows of rad first, pivots 1
            c = v[order[k]]
            if r[order[k]] == 1 and k < len(basis): out[k] = c; v = [a - c*b for a, b in zip(v, r)]
        for k, r in enumerate(I):
            if k >= len(basis): assert v[order[k]] % p == 0; out[k] = v[order[k]]//p
        return out
    M = []
    for i in range(n):
        row = []
        for r in I: row += [x % p for x in coords(mul(T, unit_vec(n, i), r))]
        M.append(row)
    return not kernel(M, p)
def maximal(T, D): return all(maximal_at(T, p) for p, e in factor(D).items() if e >= 2)
# ---- primes above p
def degrees(T, p):
    """Residue degrees of the primes above p (with multiplicity 1 each), by dimensions of ker(F^k - 1) on A/rad."""
    n = len(T); F = frobenius(T, p); rad = rref(radical(T, p, F), p); m = n - len(rad)
    piv = [next(k for k, v in enumerate(r) if v) for r in rad]; free = [k for k in range(n) if k not in piv]
    def red(v):
        v = v[:]
        for r, k in zip(rad, piv):
            if v[k]: c = v[k]; v = [(a - c*b) % p for a, b in zip(v, r)]
        return [v[k] for k in free]
    G = [red(F[k]) for k in free]; P = [unit_vec(m, i) for i in range(m)]; dim = []
    for k in range(1, m+1):
        P = matmul(P, G, p); dim.append(len(kernel([[(x - (i == j)) % p for j, x in enumerate(r)] for i, r in enumerate(P)], p)))
    # dim[k-1] = sum over primes of gcd(k, f)
    cnt = {}; from math import gcd
    import itertools
    def parts(m, mx):
        if m == 0: yield []; return
        for f in range(min(m, mx), 0, -1):
            for r in parts(m - f, f): yield [f] + r
    for fs in parts(m, m):
        if all(sum(gcd(k, f) for f in fs) == dim[k-1] for k in range(1, m+1)): return sorted(fs)
def ideal(T, g, p): return tuple(map(tuple, sorted(rref(matrix(T, g) , p), reverse=True)))     # gO + pO as a subspace mod p
def prime_degree(T, g, p):
    """f if gO is a prime ideal above p with norm p^f, else 0."""
    n = len(T); N = abs(norm(T, g)); f = 0
    while N % p == 0: N //= p; f += 1
    if N != 1 or f == 0: return 0
    sub = [list(r) for r in ideal(T, g, p)]
    if n - len(sub) != f: return 0             # p must lie in gO
    piv = [next(k for k, v in enumerate(r) if v) for r in sub]; free = [k for k in range(n) if k not in piv]
    def red(v):
        v = v[:]
        for r, k in zip(sub, piv):
            if v[k]: c = v[k]; v = [(a - c*b) % p for a, b in zip(v, r)]
        return [v[k] for k in free]
    G = [red(power(T, unit_vec(n, k), p, p)) for k in free]
    if len(rref(G, p)) != f: return 0          # nilpotents
    return f if len(kernel([[(x - (i == j)) % p for j, x in enumerate(r)] for i, r in enumerate(G)], p)) == 1 else 0
def label(T, g, q):
    """The map O -> F_q with kernel gO (norm q): the values of the basis elements mod q."""
    n = len(T); sub = [list(r) for r in ideal(T, g, q)]
    assert len(sub) == n - 1
    v = kernel([list(c) for c in zip(*sub)], q); assert len(v) == 1 and v[0][0]
    c = pow(v[0][0], -1, q); return tuple(x*c % q for x in v[0])
def is_hom(T, l, q): return l[0] == 1 and all((l[i]*l[j] - sum(c*x for c, x in zip(T[i][j], l))) % q == 0 for i in range(len(T)) for j in range(len(T)))
def ram_index(T, g, q):                        # e: dimension of O/q localized at the prime (g)
    n = len(T); P = [list(r) for r in ideal(T, g, q)]; cur = P
    for _ in range(n):                         # powers of the ideal stabilize
        nxt = rref([mul(T, a, b) for a in cur for b in P], q) if cur else []
        if len(nxt) == len(cur): break
        cur = nxt
    return n - len(cur)
def minkowski(n, r2, D, N):                    # is N <= (n!/n^n)(4/pi)^r2 sqrt|D| ?  (rational lower bound of pi: safe side)
    a, b = 333, 106                            # 333/106 < pi, so the bound is enlarged
    return (N * n**n)**2 * a**(2*r2) <= factorial(n)**2 * (4*b)**(2*r2) * abs(D)
def primes(m):
    s = bytearray([1])*(m+1); s[:2] = b'\0\0'[:min(2, m+1)]
    for i in range(2, int(m**.5)+1):
        if s[i]: s[i*i::i] = bytearray(len(s[i*i::i]))
    return [i for i in range(m+1) if s[i]]
# ---- signature and irreducibility
def signature(T):
    """(r1, r2): the trace form has r1 + r2 positive and r2 negative eigenvalues (Descartes; all roots real)."""
    n = len(T); tr = [sum(T[i][j][j] for j in range(n)) for i in range(n)]
    A = [[sum(c*t for c, t in zip(T[i][j], tr)) for j in range(n)] for i in range(n)]
    c = [x for x in charpoly_matrix(A) if x]; pos = sum((a > 0) != (b > 0) for a, b in zip(c, c[1:]))
    return pos - (n - pos), n - pos
def charpoly_matrix(A):                        # low to high, monic (Faddeev-LeVerrier)
    n = len(A); M = [[0]*n for _ in range(n)]; c = [0]*n + [1]
    for k in range(1, n+1):
        M = [[sum(A[i][l]*M[l][j] for l in range(n)) + (c[n-k+1] if i == j else 0) for j in range(n)] for i in range(n)]
        t = sum(sum(A[i][l]*M[l][i] for l in range(n)) for i in range(n)); c[n-k] = -t//k
    return c
def pmod(a, f, p):
    a = [x % p for x in a]; m = len(f) - 1
    while len(a) > m:
        c = a.pop()
        if c:
            for k in range(m): a[len(a)-m+k] = (a[len(a)-m+k] - c*f[k]) % p
    while a and a[-1] == 0: a.pop()
    return a
def pmul(a, b, f, p):
    r = [0]*(len(a)+len(b))
    for i, x in enumerate(a):
        for j, y in enumerate(b): r[i+j] += x*y
    return pmod(r, f, p) if f else [x % p for x in r]
def monic(a, p): v = pow(a[-1], -1, p); return [x*v % p for x in a]
def pgcd(a, b, p):
    while b: b = monic(b, p); a, b = b, pmod(a, b, p)
    return monic(a, p) if a else a
def pdiv(a, b, p):
    a = a[:]; q = [0]*(len(a)-len(b)+1)
    for k in range(len(q)-1, -1, -1):
        q[k] = a[k+len(b)-1] % p
        for l, y in enumerate(b): a[k+l] -= q[k]*y
    return q
def ppow(a, e, f, p):
    r = [1]
    while e:
        if e & 1: r = pmul(r, a, f, p)
        a = pmul(a, a, f, p); e >>= 1
    return r
def factor_mod(f, p, seed=1):                  # squarefree monic f, p odd: distinct degree, then Cantor-Zassenhaus
    import random
    R = random.Random(seed); out = []; x = [0, 1]; h = x; d = 0; f = f[:]
    while len(f) > 1:
        d += 1
        if 2*d > len(f) - 1: out.append(f); break
        h = ppow(pmod(h, f, p), p, f, p); g = pgcd(f, pmod([a - b for a, b in zip(h + [0]*2, x + [0]*len(h))], f, p), p)
        todo = [g] if len(g) > 1 else []
        if todo: f = pdiv(f, g, p)
        while todo:
            g = todo.pop()
            if len(g) - 1 == d: out.append(g); continue
            t = ppow([R.randrange(p) for _ in range(len(g)-1)], (p**d - 1)//2, g, p); t = t + [0]*(1 - len(t)); t[0] -= 1
            u = pgcd(g, pmod(t, g, p), p)
            if 1 < len(u) < len(g): todo += [u, pdiv(g, u, p)]
            else: todo.append(g)
    return out
def irreducible(c):
    """Monic integer polynomial, low to high: factor modulo one prime above twice the bound on factor coefficients."""
    import itertools
    n = len(c) - 1; B = 2**n * (1 + int(sum(x*x for x in c)**.5) + 1)     # Mignotte
    p = 2*B + 1
    def isp(m): return m % 2 and all(pow(a, m-1, m) == 1 for a in (2, 3, 5, 7))
    while True:
        p += 2
        if not isp(p): continue
        g = pgcd([x % p for x in c], pmod([k*x for k, x in enumerate(c)][1:], [x % p for x in c], p), p)
        if len(g) == 1: break
        if not any(k for k in g[:-1]) and p > 10**9: return False
        if p > 2*B + 4000: return False        # not squarefree
    fs = factor_mod([x % p for x in c], p)
    for k in range(1, len(fs)//2 + 1):
        for S in itertools.combinations(fs, k):
            g = [1]
            for h in S: g = pmul(g, h, None, p)
            while g and g[-1] == 0: g.pop()
            g = [x - p if x > p//2 else x for x in g]
            a = c[:]; m = len(g) - 1; bad = False      # exact division
            for t in range(len(a)-1-m, -1, -1):
                q = a[t+m]
                for l, y in enumerate(g): a[t+l] -= q*y
            if not any(a): return False
    return True
def is_field(T):
    n = len(T); import itertools
    for v in itertools.product(range(4), repeat=n-1):  # an element that generates the algebra
        x = [0] + list(v); c = charpoly_matrix(matrix(T, x))
        if n == 1: return True
        p = 2**61 - 1
        if len(pgcd([a % p for a in c], pmod([k*a for k, a in enumerate(c)][1:], [a % p for a in c], p), p)) == 1: return irreducible(c)
    return False
def check_field(T, D, r2, gens):
    """PID certificate: gens = {p: [generator, ...]} for the primes of norm <= Minkowski bound. Returns (ok, reason)."""
    n = len(T)
    if not is_ring(T): return False, 'not a ring'
    if discriminant(T) != D: return False, 'discriminant'
    if signature(T) != (n - 2*r2, r2): return False, 'signature'
    if not is_field(T): return False, 'not a field'
    if not maximal(T, D): return False, 'not maximal'
    M = 1
    while minkowski(n, r2, D, M+1): M += 1
    need = set()
    for p in primes(M):
        want = sorted(f for f in degrees(T, p) if p**f <= M and f < n)
        have = []; seen = set()
        for g in gens.get(p, []):
            f = prime_degree(T, g, p); I = ideal(T, g, p)
            if f == 0: return False, 'generator not prime'
            if I in seen: return False, 'repeated ideal'
            seen.add(I); have.append(f)
        if sorted(have) != want: return False, 'primes missing or extra at %d: %s %s' % (p, want, have)
        need.add(p)
    if set(gens) - need: return False, 'extra primes'
    return True, M
def check_row(T, q, gens):
    """One mined row: all primes of norm q, each with a generator; returns [(label, e)] sorted by label."""
    want = degrees(T, q).count(1); out = []
    if len(gens) != want: raise ValueError('wrong number of primes')
    for g in gens:
        if abs(norm(T, g)) != q: raise ValueError('norm')
        l = label(T, g, q)
        if not is_hom(T, l, q): raise ValueError('label')
        out.append((l, ram_index(T, g, q)))
    if len({l for l, _ in out}) != len(out): raise ValueError('repeated prime')
    return sorted(out)
