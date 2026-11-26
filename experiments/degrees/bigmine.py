#!/usr/bin/env python3
"""Mining cost for large primes q and larger discriminants. python3 bigmine.py file [digits...]"""
import sys, time, random
from mine import *
from collections import defaultdict
def defaultdict_(): return defaultdict(int)
R = random.Random(3)
def big_prime(d):
    while True:
        q = R.randrange(10**(d-1), 10**d) | 1
        if all(pow(a, q-1, q) == 1 for a in (2, 3, 5, 7, 11, 13)): return q
fn = sys.argv[1]; digits = [int(x) for x in sys.argv[2:]] or [3, 6, 9, 12, 15]
for D, T, gens, units in load(fn):
    Mi = Miner(T, units)
    for p, gs in gens.items():
        for g in gs:
            if prime_degree(T, g, p) == 1: Mi.base.setdefault(p, []).append((label(T, g, p), g))
    for q in primes(200):                       # earlier rows of the chain as factor base
        Mi.base[q] = [(l, g) for l in Mi.homs(q) for g in [Mi.mine(q, l, defaultdict_())] if g]
    out = []; over = []
    for d in digits:
        st = defaultdict_(); t = time.time(); n = 0; bits = 0
        for _ in range(4):
            q = big_prime(d); ls = Mi.homs(q)
            for l in ls:
                g = Mi.mine(q, l, st)
                if g:
                    assert abs(norm(T, g)) == q and label(T, g, q) == l; n += 1; bits = max(bits, max(abs(a).bit_length() for a in g))
                    over.append(max(abs(a).bit_length() for a in g) - (q.bit_length()/3 + sum(max(abs(x).bit_length() for x in u + v) for u, v in zip(units, Mi.inv))))
        out.append('%d digits: %d found, %d failed, %.0f ms per prime, %d bits' % (d, n, st['failed'], 1e3*(time.time() - t)/max(1, n + st['failed']), bits))
    cap = lambda q: q.bit_length()/3 + sum(max(abs(x).bit_length() for x in u + v) for u, v in zip(units, Mi.inv))
    print('D=%d units %d, largest unit coefficient %d bits | %s' % (D, len(units), max([max(abs(a).bit_length() for a in u) for u in units] or [0]), ' | '.join(out)), flush=True)
