#!/usr/bin/env python3
"""Rule A4 issuance row by row, with the backlog share (B3.5 reference for the engine).

docs/protocol-parameters.md "Ring issuance (starting rule A4)": the entry of a ring for main-chain
prime q issues, for each of its primes of norm q, v * e; primes mined before the ring opened pay
the backlog share (parameter 19). Each payment is split by R11: ring miner 50%, holders of the
main-chain prime 35% (at the end of the block before the row), founder 15% (rounded down each,
remainders never issued). Rings, miners and works are simulated on a small chain; the segments
follow R7 (rows types by the Kronecker symbol, a segment ends at the first inert row after a paid
one, at the 32nd paid row, or empty after 1,024 inert rows). Competing works are resolved as the
engine's proposer does (D4 analog): at the ring's next prime, the work reaching furthest wins,
ties by arrival; its segments from there on are accepted, then the next position.

Python, independent of the engine. Writes the vectors read by next/tests/ring_mine_tests.cpp:

  python3 a4_rows.py > a4_vectors.txt

Records (one per line):
  PARAM name value                 v, backlog_bp, upstream_bp, founder_bp, blocks, main_reward
  CERT D factors gens unit         certificate fields as in quadratic_vectors_v2.txt FIELD lines
  RING k D commit discoverer asset ring k: committed in block `commit` (deposit and fee in main
                                   prime `asset`), revealed in commit + 1, opens there
  GEN D q c0 c1                    the reduced generator for the smaller (or double) root above q
  BLOCK h ranger finder            main block h: one range by `ranger`, the prime by `finder`
  WORK h k miner start n           a signed RING_WORK arriving for open height h: n segments from q = start
  ACCEPT h k miner start n         the batch at open height h accepts n segments of ring k from start
  FRONTIER k q                     ring k's last row at the end
  ISSUED k rows paid backlog total rows, paid rows, ring primes paying the backlog share, units issued
  DUE name amount                  everything an address is due from all ring primes
  FINAL lines digest total         sha3-256 over sorted lines "k q i name amount\\n" of every nonzero award
"""
import hashlib, os, random, sys

V, BACKLOG, UP, FO, MAIN, DEPOSIT, FEE = 100000, 1000, 3500, 1500, 1000000, 100000, 1
PAID_ROWS, INERT_ROWS = 32, 1024
BLOCKS = 220
SEED = 35

def sieve(n):
    s = bytearray([1]) * (n + 1); s[0] = s[1] = 0
    for i in range(2, int(n ** .5) + 1):
        if s[i]: s[i*i::i] = bytearray(len(s[i*i::i]))
    return [i for i in range(n + 1) if s[i]]
P = sieve(20000)                      # P[h - 1] is the prime of main block h (2 and 3 are blocks 1, 2)
HEIGHT = {p: i + 1 for i, p in enumerate(P)}

def kronecker(d, n):                  # (d/n) for a prime n
    if n == 2:
        if d % 2 == 0: return 0
        return 1 if d % 8 in (1, 7) else -1
    r = pow(d % n, (n - 1) // 2, n)
    return 0 if d % n == 0 else (1 if r == 1 else -1)

def row_type(d, q):                   # 0 inert, 1 split, 2 ramified (R9; the 2-bit codes on the wire)
    k = kronecker(d, q)
    return 2 if k == 0 else (1 if k > 0 else 0)

def norm(d, c0, c1):
    if d % 4 == 1: return c0*c0 + c0*c1 + c1*c1*((1 - d)//4)
    return c0*c0 - (d//4)*c1*c1
def mul(d, a, b):
    cc = a[1]*b[1]
    if d % 4 == 1: return (a[0]*b[0] + cc*((d - 1)//4), a[0]*b[1] + a[1]*b[0] + cc)
    return (a[0]*b[0] + cc*(d//4), a[0]*b[1] + a[1]*b[0])
def conj(d, a): return (a[0] + a[1], -a[1]) if d % 4 == 1 else (a[0], -a[1])
def height(a): return a[0]*a[0] + a[1]*a[1]
def roots(d, q):
    t = 1 if d % 4 == 1 else 0
    return sorted(x for x in range(q) if (x*x - t*x + ((1 - d)//4 if t else -(d//4))) % q == 0)
def root_of(q, g):
    if g[1] % q == 0: return None
    return (-g[0] * pow(g[1], -1, q)) % q

def isqrt(n):
    if n < 0: return None
    r = int(n ** .5)
    while r*r > n: r -= 1
    while (r + 1)*(r + 1) <= n: r += 1
    return r if r*r == n else None

def generator(d, q, unit):
    """A generator of the prime above q with the smaller (or double) root, reduced by the unit (R10)."""
    small = roots(d, q)[0]
    targets = [q] if d < 0 else [q, -q]
    c1 = 1
    while True:
        for n in targets:
            for s1 in (c1, -c1):
                if d % 4 == 1:
                    disc = isqrt(d*s1*s1 + 4*n)
                    cands = [] if disc is None else [((-s1 + sg*disc)//2) for sg in (1, -1) if (-s1 + sg*disc) % 2 == 0]
                else:
                    r = isqrt(n + (d//4)*s1*s1)
                    cands = [] if r is None else [r, -r]
                for c0 in cands:
                    g = (c0, s1)
                    assert norm(d, *g) == n
                    r = root_of(q, g)
                    if r is None: continue
                    if r != small: g = conj(d, g)
                    assert root_of(q, g) == small
                    if unit:
                        nu = norm(d, *unit); inv = conj(d, unit); inv = (inv[0]*nu, inv[1]*nu)
                        while True:
                            a, b = mul(d, g, unit), mul(d, g, inv)
                            if height(a) < height(g): g = a
                            elif height(b) < height(g): g = b
                            else: break
                    return g
        c1 += 1
        if d < 0 and c1*c1*(-d) > 4*q + 4: raise RuntimeError('no generator: %d %d' % (d, q))

def segment_end(d, start, frontier):
    """Index (into P) of the last row of the segment starting at P[start], or None if it is not
    complete at or below P[frontier]."""
    paid, k = 0, start
    while True:
        if k > frontier: return None
        t = row_type(d, P[k])
        if t != 0:
            paid += 1
            if paid == PAID_ROWS: return k
        elif paid > 0 or k - start + 1 == INERT_ROWS: return k
        k += 1

# ---- the certificates (experiments/extensions/quadratic_vectors_v2.txt)
CERTS = {}
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'extensions', 'quadratic_vectors_v2.txt')):
    if not line.startswith('FIELD'): continue
    f = dict(x.split('=', 1) for x in line.split()[1:])
    CERTS[int(f['D'])] = (f['FACTORS'], f['GENS'], f.get('UNIT', ''))

def unit_of(d):
    u = CERTS[d][2]
    return tuple(map(int, u.split(':'))) if u else None

MINERS = ['A', 'B', 'C']               # main chain: ranges and primes
RING_MINERS = ['A', 'D', 'E', 'F']     # D, E, F hold no main-chain units
# (D, commit block, discoverer): two rings in one block, others opening later (more backlog).
RINGS = [(-4, 5, 'A'), (-7, 5, 'B'), (5, 12, 'C'), (-3, 30, 'A'), (13, 60, 'B'), (17, 100, 'C'),
         (-8, 140, 'A'), (21, 175, 'B')]

def main():
    R = random.Random(SEED)
    out = []
    w = out.append
    for name, val in (('v', V), ('backlog_bp', BACKLOG), ('upstream_bp', UP), ('founder_bp', FO),
                      ('blocks', BLOCKS), ('main_reward', MAIN)):
        w('PARAM %s %d' % (name, val))
    for d, _, _ in RINGS:
        f, g, u = CERTS[d]
        w('CERT %d %s %s %s' % (d, f, g or '-', u or '-'))
    hold = {}                                       # (q, address) -> [(block, delta)]
    def move(q, a, x, b): hold.setdefault((q, a), []).append((b, x))
    def owned_at(q, a, t): return sum(x for b, x in hold.get((q, a), []) if b <= t)
    paid_to = {}                                    # block -> the miners paid in it
    for h in range(3, BLOCKS + 1):
        ranger, finder = MINERS[h % 3], MINERS[(h + 1) % 3]
        w('BLOCK %d %s %s' % (h, ranger, finder))
        p, f = P[h - 1], P[h - 2]
        c = p - f - 1
        share = (MAIN // 2) // c
        move(p, ranger, share * c, h); move(p, finder, MAIN - share * c, h)
        paid_to[h] = (ranger, finder)
    rings = {}                                      # k -> dict
    for k, (d, cb, who) in enumerate(RINGS, start=1):
        # the deposit asset: the prime of the last block before the commit in which `who` was paid
        asset = P[max(b for b in range(3, cb) if who in paid_to[b]) - 1]
        assert owned_at(asset, who, cb - 1) >= DEPOSIT + FEE
        w('RING %d %d %d %s %d' % (k, d, cb, who, asset))
        move(asset, who, -(DEPOSIT + FEE), cb); move(asset, who, DEPOSIT, cb + 1)
        rings[k] = dict(d=d, opened=cb + 1, founder=who, next=0, unit=unit_of(d))
    gens = {}
    pending = []                                    # [arrival, k, miner, start index, ends]
    accepted = []
    due = {}                                        # (k, q, i, address) -> amount
    stats = {k: [0, 0, 0, 0] for k in rings}        # rows, paid, backlog primes, issued
    arrival = 0
    for h in range(3, BLOCKS + 1):
        frontier = h - 2                            # index of the chain frontier at the end of h - 1
        # works arriving for open height h
        for k, r in rings.items():
            if r['opened'] >= h: continue
            for _ in range(R.choice((0, 0, 1, 1, 2))):
                # most works start at the ring's next prime; some start further on (a gap until
                # the earlier segments arrive), some repeat what another miner sent
                ends, pos = [], r['next']
                if R.random() < .15 and pending:
                    same = [x for x in pending if x[1] == k]
                    if same:
                        base = R.choice(same)
                        arrival += 1
                        pending.append([arrival, k, R.choice([m for m in RING_MINERS if m != base[2]]), base[3], list(base[4])])
                        w('WORK %d %d %s %d %d' % (h, k, pending[-1][2], P[base[3]], len(base[4])))
                        continue
                skip = R.random() < .15
                n = R.randint(1, 4)
                start = pos
                for j in range(n + (1 if skip else 0)):
                    e = segment_end(r['d'], pos, frontier)
                    if e is None: break
                    if skip and j == 0: start = e + 1
                    else: ends.append(e)
                    pos = e + 1
                if not ends: continue
                arrival += 1
                pending.append([arrival, k, R.choice(RING_MINERS), start, ends])
                w('WORK %d %d %s %d %d' % (h, k, pending[-1][2], P[start], len(ends)))
        # the batch at open height h: per ring in number order, greedy from the ring's next prime
        for k, r in sorted(rings.items()):
            if r['opened'] >= h: continue
            while True:
                pos = r['next']
                best = None
                for x in pending:
                    if x[1] != k: continue
                    starts = [x[3]] + [e + 1 for e in x[4][:-1]]
                    if pos not in starts: continue
                    j = starts.index(pos)
                    key = (-x[4][-1], x[0])
                    if best is None or key < best[0]: best = (key, x, j)
                if best is None: break
                _, x, j = best
                segs = x[4][j:]
                w('ACCEPT %d %d %s %d %d' % (h, k, x[2], P[pos], len(segs)))
                for e in segs:
                    for idx in range(pos, e + 1):
                        q = P[idx]; t = row_type(r['d'], q)
                        stats[k][0] += 1
                        if t == 0: continue
                        stats[k][1] += 1
                        if (r['d'], q) not in gens: gens[(r['d'], q)] = generator(r['d'], q, r['unit'])
                        for i, e_ in ((0, 2),) if t == 2 else ((0, 1), (1, 1)):
                            issued = e_ * V
                            if HEIGHT[q] < r['opened']:          # older than the ring: the backlog share
                                issued = issued * BACKLOG // 10000; stats[k][2] += 1
                            stats[k][3] += issued
                            up, fo = issued * UP // 10000, issued * FO // 10000
                            def pay(a, amt):
                                if amt: due[(k, q, i, a)] = due.get((k, q, i, a), 0) + amt
                            pay(x[2], issued - up - fo)
                            pay(r['founder'], fo)
                            if q > 3:                            # 2 and 3: no holders; no validators here
                                for a in MINERS:
                                    held = owned_at(q, a, h - 1)
                                    if held: pay(a, up * held // MAIN)
                    pos = e + 1
                r['next'] = pos
            # works now fully covered are stale
            pending = [x for x in pending if not (x[1] == k and x[4][-1] < r['next'])]
    # Q(sqrt 97) is not mined here; its generators (coefficients above 255 from q = 43) serve the
    # engine test of the generator size limit (parameter 65).
    f, g, u = CERTS[97]
    w('CERT 97 %s %s %s' % (f, g, u))
    for q in P[:80]:
        if row_type(97, q): gens[(97, q)] = generator(97, q, unit_of(97))
    # Generators a little beyond the end of the run, so the engine tests can mine further.
    for r in rings.values():
        for q in P[:BLOCKS + 60]:
            if row_type(r['d'], q) and (r['d'], q) not in gens: gens[(r['d'], q)] = generator(r['d'], q, r['unit'])
    for (d, q), g in sorted(gens.items()):
        w('GEN %d %d %d %d' % (d, q, g[0], g[1]))
    total = 0
    for k, r in sorted(rings.items()):
        w('FRONTIER %d %d' % (k, P[r['next'] - 1] if r['next'] else 0))
        w('ISSUED %d %d %d %d %d' % (k, *stats[k]))
        total += stats[k][3]
    per = {}
    for (k, q, i, a), v in due.items(): per[a] = per.get(a, 0) + v
    for a in sorted(per): w('DUE %s %d' % (a, per[a]))
    lines = sorted('%d %d %d %s %d\n' % (k, q, i, a, v) for (k, q, i, a), v in due.items() if v)
    w('FINAL %d %s %d' % (len(lines), hashlib.sha3_256(''.join(lines).encode()).hexdigest(), total))
    sys.stdout.write('\n'.join(out) + '\n')

if __name__ == '__main__':
    main()
