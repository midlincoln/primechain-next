#!/usr/bin/env python3
"""Toy ceremony on small CSIDH-type primes: many participants make a supersingular curve over F_p
whose secret (the class-group element leading to it from y^2 = x^3 + x) nobody knows.

Each participant applies a secret walk (an exponent vector over the small ideals l_i) to the last
accepted curve and proves, in zero knowledge, that the new curve came from the old one:
a sigma protocol with rejection sampling (as in SeaSign; no class-group computation needed), made
non-interactive with a hash (Fiat-Shamir). The "chain" accepts contributions in order, rejects stale
and forged ones, and records only curves and proofs. Small primes give NO security; this models the
procedure and measures how sizes and times grow. Run: python3 ceremony.py"""
import hashlib, random, sys, time

def is_prime(n):
    if n < 2: return False
    for q in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % q == 0: return n == q
    d, s = n - 1, 0
    while d % 2 == 0: d //= 2; s += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        x = pow(a, d, n)
        if x in (1, n - 1): continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1: break
        else: return False
    return True

def csidh_prime(n):
    """p = 4 * f * l_1 * ... * l_n - 1 for the first n odd primes, smallest cofactor f."""
    ls, k = [], 3
    while len(ls) < n:
        if is_prime(k): ls.append(k)
        k += 2
    prod = 4
    for l in ls: prod *= l
    f = 1
    while not is_prime(prod * f - 1): f += 1
    return prod * f - 1, tuple(ls)

class Field:
    def __init__(s, p, ls): s.p, s.ls = p, ls
    def inv(s, x): return pow(x, s.p - 2, s.p)
    def sq(s, x): return x % s.p == 0 or pow(x, (s.p - 1) // 2, s.p) == 1
    def sqrt(s, x): return pow(x, (s.p + 1) // 4, s.p)
    # x-only Montgomery arithmetic on y^2 = x^3 + A x^2 + x, projective (X : Z)
    def xdbl(s, P, A):
        X, Z = P; p = s.p; a24 = (A + 2) * s.inv(4) % p
        t1 = (X + Z) ** 2 % p; t2 = (X - Z) ** 2 % p; t3 = t1 - t2
        return (t1 * t2 % p, t3 * (t2 + a24 * t3) % p)
    def xadd(s, P, Q, D):
        p = s.p; u = (P[0] - P[1]) * (Q[0] + Q[1]) % p; v = (P[0] + P[1]) * (Q[0] - Q[1]) % p
        return (D[1] * (u + v) ** 2 % p, D[0] * (u - v) ** 2 % p)
    def xmul(s, n, P, A):
        if n == 0: return (1, 0)
        R0, R1 = P, s.xdbl(P, A)
        for b in bin(n)[3:]:
            if b == '1': R0, R1 = s.xadd(R0, R1, P), s.xdbl(R1, A)
            else: R0, R1 = s.xdbl(R0, A), s.xadd(R0, R1, P)
        return R0
    def step(s, A, l, rng):
        """Action of the ideal (l, pi - 1): the l-isogeny whose kernel is F_p-rational."""
        p = s.p
        while True:
            x = rng.randrange(1, p)
            if not s.sq((x * x * x + A * x * x + x) % p): continue
            K = s.xmul((p + 1) // l, (x, 1), A)
            if K[1] % p: break
        xs = []
        for m in range(1, (l - 1) // 2 + 1):            # x-coordinates of K, 2K, ..., ((l-1)/2) K
            T = s.xmul(m, K, A); xs.append(T[0] * s.inv(T[1]) % p)
        sm = sum(xs) % p; t = sum(s.inv(v) for v in xs) % p; pr = 1
        for v in xs: pr = pr * v % p
        return (A + 6 * t - 6 * sm) * pr * pr % p
    def act(s, e, A, rng):
        """Apply the exponent vector e: positive entries on the curve, negative on its twist (A -> -A)."""
        p = s.p
        for l, k in zip(s.ls, e):
            for _ in range(abs(k)):
                A = s.step(A, l, rng) if k > 0 else (-s.step(-A % p, l, rng)) % p
        return A
    def supersingular(s, A, rng, tries=8):
        """Validator check: (p+1) P = 0 for random points (a probabilistic stand-in for CSIDH's exact test)."""
        p = s.p
        for _ in range(tries):
            x = rng.randrange(1, p)
            if s.xmul(p + 1, (x, 1), A)[1] % p: return False
        return True

M = 1            # secret exponents in [-M, M]
def prove(F, E0, E1, secret, k, rng, B):
    """k rounds; commitment r in [-B, B]^n; for challenge 1 reveal z = r - s, accepted only if |z_i| <= B - M."""
    rounds = []
    while len(rounds) < k:
        r = [rng.randint(-B, B) for _ in F.ls]
        rounds.append((r, F.act(r, E0, rng)))
    commits = [c for _, c in rounds]
    h = hashlib.sha3_256(repr((F.p, E0, E1, commits)).encode()).digest()
    bits = [(h[i // 8] >> (i % 8)) & 1 for i in range(k)]
    resp = []
    for (r, c), b in zip(rounds, bits):
        if b == 0: resp.append(r)
        else:
            z = [ri - si for ri, si in zip(r, secret)]
            if max(map(abs, z)) > B - M: return None   # abort: the response would leak information; retry
            resp.append(z)
    return commits, resp
def verify(F, E0, E1, proof, k, B, rng):
    commits, resp = proof
    if len(commits) != k or len(resp) != k: return False
    h = hashlib.sha3_256(repr((F.p, E0, E1, commits)).encode()).digest()
    bits = [(h[i // 8] >> (i % 8)) & 1 for i in range(k)]
    for c, z, b in zip(commits, resp, bits):
        bound = B if b == 0 else B - M
        if len(z) != len(F.ls) or max(map(abs, z)) > bound: return False
        if F.act(z, E0 if b == 0 else E1, rng) != c: return False
    return True
def size(F, proof):
    pb = (F.p.bit_length() + 7) // 8
    return len(proof[0]) * pb + sum(len(z) for z in proof[1])      # one byte per exponent

class Chain:
    """Contributions in order; each must build on the latest curve and carry a valid proof."""
    def __init__(s, F, k, B, rng):
        s.F, s.k, s.B, s.rng = F, k, B, rng; s.curves = [0]; s.log = []; s.refused = []
    def submit(s, who, prev, new, proof):
        if prev != s.curves[-1]: s.refused.append((who, 'stale: not built on the latest curve')); return False
        if not s.F.supersingular(new, s.rng): s.refused.append((who, 'not a supersingular curve')); return False
        if not verify(s.F, prev, new, proof, s.k, s.B, s.rng): s.refused.append((who, 'proof does not verify')); return False
        s.curves.append(new); s.log.append((who, prev, new, size(s.F, proof))); return True

def contribute(F, chain, who, rng, cheat=None):
    E0 = chain.curves[-1]
    for _ in range(400):
        secret = [rng.randint(-M, M) for _ in F.ls]
        E1 = F.act(secret, E0, rng)
        if cheat == 'random curve':           # a curve not reached from E0 (here: from the start curve)
            E1 = F.act(secret, 0, rng)
        pf = prove(F, E0, E1, secret, chain.k, rng, chain.B)
        if pf: break
    if cheat == 'stale': E0 = chain.curves[-2]
    return chain.submit(who, E0, E1, pf), secret

def brute(F, target, rng, limit):
    """What a small prime allows: search the curves reachable from y^2 = x^3 + x by breadth-first walk."""
    seen = {0: ()}; frontier = [0]; t = time.time()
    while frontier and len(seen) < limit:
        nxt = []
        for A in frontier:
            for i, l in enumerate(F.ls):
                for sgn in (1, -1):
                    e = [0] * len(F.ls); e[i] = sgn
                    B2 = F.act(e, A, rng)
                    if B2 not in seen:
                        seen[B2] = 1; nxt.append(B2)
                        if B2 == target: return len(seen), time.time() - t
        frontier = nxt
    return None, time.time() - t

if __name__ == '__main__':
    rng = random.Random(7)
    k = 16                                   # Fiat-Shamir rounds (real use: 128 or more)
    for n in (3, 5):          # 8 small ideals takes minutes per contribution in Python
        p, ls = csidh_prime(n); F = Field(p, ls); B = 3 * len(ls)   # wide commitment box keeps aborts rare
        chain = Chain(F, k, B, rng); t = time.time(); secrets = []
        for who in ('alice', 'bob', 'carol', 'dave'):
            ok, sec = contribute(F, chain, who, rng); secrets.append(sec)
        honest = time.time() - t
        contribute(F, chain, 'mallory (random curve)', rng, cheat='random curve')
        contribute(F, chain, 'erin (stale)', rng, cheat='stale')
        # stale proof replayed on the current curve
        last = chain.log[-1]
        tv = time.time(); ok = verify(F, last[1], last[2], prove(F, last[1], last[2], secrets[-1], k, rng, B) or ([], []), k, B, rng); tv = time.time() - tv
        found, bt = brute(F, chain.curves[-1], rng, 200000) if p.bit_length() <= 40 else (None, 0)
        print('p of %d bits (%d small ideals): 4 contributions accepted in %.1f s; proof %d bytes; one proof checked in %.2f s; refused: %s'
              % (p.bit_length(), n, honest, chain.log[0][3], tv, '; '.join(r for _, r in chain.refused)))
        print('    final curve A = %d; ' % chain.curves[-1] + ('found by searching %d curves in %.1f s (small primes are not secure)' % (found, bt) if found else
              'not found by a search of up to 200,000 curves' if p.bit_length() <= 40 else 'too large to search in this model'), flush=True)
