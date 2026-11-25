#!/usr/bin/env python3
"""Can an award be spent if nothing was written for its owner? (docs/ring-records.md, section 4)

Three independent nodes keep ring rows on disk and compute balances; a fourth ledger pays every
owner at once, the old way. All must accept and reject the same transfers and agree on every
balance. Ring rows are real (norms and roots are checked). Signatures are not part of this test."""
import bisect, hashlib, json, os, random, sqlite3, sys, tempfile
from collections import defaultdict
HERE = os.path.dirname(os.path.abspath(__file__))
M, POOL, VALS, FEE, EPOCH = 10**6, '<pool>', ['val1', 'val2', 'val3'], 1, 100
def sieve(n):
    s = bytearray([1]) * (n + 1); s[0] = s[1] = 0
    for i in range(2, int(n**.5) + 1):
        if s[i]: s[i*i::i] = bytearray(len(s[i*i::i]))
    return [i for i in range(n + 1) if s[i]]
PR = sieve(8000)
# ---- rings: number -> name; ROOTS[(ring, q)] = sorted roots of the paid primes; LOWER[ring] = ring below
NAMES, ROOTS, LOWER, RAM, checked = {}, {}, {}, set(), 0
num = {}
for line in open(os.path.join(HERE, '..', 'extensions', 'quadratic_vectors.txt')):
    if not line.startswith('ENTRY'): continue
    e = dict(x.split('=') for x in line.split()[1:])
    D, p = int(e['D']), int(e['P'])
    if D == 8: continue                       # Q(sqrt 2) comes with the tower below
    k = num.setdefault(D, len(num) + 1); NAMES[k] = 'D=%d' % D; LOWER[k] = 0
    if e['TYPE'] == 'inert': ROOTS[(k, p)] = []; continue
    c0, c1 = map(int, e['GEN'].split(':')); tr = D % 4
    n = c0*c0 + c0*c1 + c1*c1*(1 - D)//4 if tr else c0*c0 - D//4*c1*c1
    r = -c0 * pow(c1, -1, p) % p
    assert abs(n) == p and r == int(e['R']); checked += 1
    other = (tr - r) % p
    ROOTS[(k, p)] = [r] if e['TYPE'] == 'ramified' else [r, other]
    assert e['TYPE'] == 'ramified' or r < other
    if e['TYPE'] == 'ramified': RAM.add((k, p))
T = len(num); tower = {}
for line in open(os.path.join(HERE, 'tower_generators.txt')):
    k, q, _ = line.split(' ', 2); tower[(int(k), int(q))] = 1
for lev in (1, 2, 3):
    k = T + lev; NAMES[k] = 'tower degree %d' % 2**lev; LOWER[k] = T + lev - 1 if lev > 1 else 0
    for q in PR:
        if (lev, q) not in tower: ROOTS[(k, q)] = []; continue
        if q == 2: ROOTS[(k, q)] = [0]; RAM.add((k, q)); continue
        low = ROOTS[(k - 1, q)] if lev > 1 else [0]
        rs = [x for x in range(q) if (x*x - 2) % q in low] if q < 2000 else []
        assert q >= 2000 or len(rs) == 2**lev
        ROOTS[(k, q)] = rs
R17 = num[17]
def issued(X):                                # nominal amount of one prime
    if X[0] == 0: return M
    return M if (X[0], X[1]) in RAM else M // len(ROOTS[(X[0], X[1])])
def below(X):
    k, q, i = X
    if LOWER[k] == 0: return (0, q, 0)
    if q == 2: return (LOWER[k], 2, 0)
    return (LOWER[k], q, ROOTS[(LOWER[k], q)].index((ROOTS[(k, q)][i]**2 - 2) % q))
def split(X):                                 # miner, founder, [(prime below, amount)], validators' part
    I = issued(X); up, fo = I*35//100, I*15//100; recs = []; S, Y = up, X
    while True:
        Y = below(Y)
        if Y[0] == 0: recs.append((Y, S)); break
        recs.append((Y, S//2)); S -= S//2
    val = 0
    if X[1] < 5: val = recs.pop()[1]           # 2 and 3 have no holders
    return I - up - fo, fo, recs, val
def part(total, n, j): return total // n + (j < total % n)
FOUNDER = lambda k: 'founder%d' % k

class Push:                                   # reference: credit everybody at once
    def __init__(s): s.bal = defaultdict(lambda: defaultdict(int)); s.nonce = defaultdict(int); s.dust = 0; s.made = 0
    def rows(s, b, rows):
        todo = []
        for k, q, miner in rows:
            for i in range(len(ROOTS[(k, q)])):
                X = (k, q, i); mi, fo, recs, val = split(X); s.made += issued(X); left = issued(X)
                out = [(miner, mi), (FOUNDER(k), fo)] + [(v, part(val, 3, j)) for j, v in enumerate(VALS) if val]
                for L, amt in recs:
                    out += [(a, amt*v//issued(L)) for a, v in s.bal[L].items() if a != POOL]
                todo += [(X, a, x) for a, x in out if x]; s.dust += left - sum(x for _, x in out)
        for X, a, x in todo: s.bal[X][a] += x
    def owned(s, X, a): return s.bal[X][a] if X in s.bal and a in s.bal[X] else 0
    def exists(s, X): return X in s.bal
    def move(s, X, a, x, b): s.bal[X][a] += x
    def pool(s): return {X: d[POOL] for X, d in s.bal.items() if d.get(POOL)}
    def main(s, b, p, shares):
        for a, x in shares.items(): s.bal[(0, p, 0)][a] += x
        s.made += sum(shares.values()); s.b = b

class Lookup:                                 # proposal: rows on disk, net transfers in state
    mut = None
    def __init__(s, path):
        s.db = sqlite3.connect(path); s.path = path
        s.db.execute('create table if not exists row(ring int, q int, h int, miner text, primary key(ring,q)) without rowid')
        s.net, s.last, s.poolb, s.nonce, s.mainp, s.b = {}, {}, {}, defaultdict(int), {}, 0
        s.entries = 0
    def row(s, k, q): return s.db.execute('select h, miner from row where ring=? and q=?', (k, q)).fetchone()
    def at(s, X, a, t):
        h = s.net.get((X, a), []); i = bisect.bisect_right(h, [t, float('inf')]); return h[i-1][1] if i else 0
    def due(s, X, a):
        if X[0] == 0: return 0                # main primes: mined amounts are in the history itself
        r = s.row(X[0], X[1])
        if not r or a == POOL: return 0
        h, miner = r; mi, fo, recs, val = split(X)
        x = (mi if a == miner else 0) + (fo if a == FOUNDER(X[0]) else 0)
        for L, amt in recs: x += amt * s.owned_at(L, a, h if s.mut == 'block h, not h-1' else h - 1) // issued(L)
        if val and a in VALS: x += part(val, 3, VALS.index(a))
        return x
    def owned_at(s, X, a, t): return s.due(X, a) + s.at(X, a, t)
    def owned(s, X, a): return s.poolb.get(X, 0) if a == POOL else s.owned_at(X, a, s.b)
    def exists(s, X):
        return X[1] in s.mainp if X[0] == 0 else bool(s.row(X[0], X[1])) and X[2] < len(ROOTS[(X[0], X[1])])
    def rows(s, b, rows):
        s.b = b
        for k, q, miner in rows:
            s.db.execute('insert into row values(?,?,?,?)', (k, q, b, miner))
            for i in range(len(ROOTS[(k, q)])):
                for L, _ in split((k, q, i))[2]: s.last[L] = b
    def move(s, X, a, x, b):
        if a == POOL: s.poolb[X] = s.poolb.get(X, 0) + x; return
        if s.mut == 'spending not recorded' and x < 0: return
        h = s.net.setdefault((X, a), []); cur = h[-1][1] if h else 0
        if h and (s.last.get(X, -1) <= h[-1][0] or s.mut == 'history overwritten'): h[-1] = [b, cur + x]
        else: h.append([b, cur + x]); s.entries += 1
    def pool(s): return {X: v for X, v in s.poolb.items() if v}
    def main(s, b, p, shares):
        s.mainp[p] = b
        for a, x in shares.items(): s.move((0, p, 0), a, x, b)
        s.db.commit()
    def state(s):
        return {'net': sorted([list(k[0]), k[1], v] for k, v in s.net.items()), 'last': sorted([list(k), v] for k, v in s.last.items()),
                'pool': sorted([list(k), v] for k, v in s.poolb.items()), 'nonce': sorted(s.nonce.items()), 'main': sorted(s.mainp.items()), 'b': s.b}
    def digest(s): return hashlib.sha3_256(json.dumps(s.state()).encode()).hexdigest()
    def restart(s):                           # snapshot to bytes, forget everything, load again
        d = json.loads(json.dumps(s.state())); s.db.close()
        n = type(s)(s.path); n.mut = s.mut; n.entries = s.entries
        n.net = {} if s.mut == 'restart loses history' else {(tuple(X), a): v for X, a, v in d['net']}
        n.last = {tuple(X): v for X, v in d['last']}; n.poolb = {tuple(X): v for X, v in d['pool']}
        n.nonce.update(d['nonce']); n.mainp = dict(d['main']); n.b = d['b']; return n

def transfer(L, tx, b):
    """tx = (sender, asset, [(receiver, amount)], fee asset, nonce). Returns None or the reason for refusal."""
    a, X, outs, F, n = tx[0], tuple(tx[1]), tx[2], tuple(tx[3]), tx[4]
    if n != L.nonce[a] + 1: return 'nonce'
    if not L.exists(X) or not L.exists(F): return 'no such prime'
    if any(x <= 0 for _, x in outs): return 'amount'
    need = defaultdict(int); need[X] += sum(x for _, x in outs); need[F] += FEE
    for Y, x in need.items():
        if L.owned(Y, a) < x: return 'not enough'
    L.nonce[a] = n
    for Y, x in need.items(): L.move(Y, a, -x, b)
    for c, x in outs: L.move(X, c, x, b)
    L.move(F, POOL, FEE, b)
    return None
def apply(L, B):                              # rows, transfers, main reward, epoch distribution
    b, rows, txs, p, shares = B; L.rows(b, rows); res = [transfer(L, t, b) for t in txs]
    L.main(b, p, shares)
    if b % EPOCH == 0:
        for X, v in sorted(L.pool().items()):
            L.move(X, POOL, -v, b)
            for j, w in enumerate(VALS):
                if part(v, 3, j): L.move(X, w, part(v, 3, j), b)
    return res

class Net:                                    # nodes A, B (restarts), C (late, replays the file), reference
    def __init__(s, d, mut=None):
        s.d = d; Lookup.mut = mut; s.A, s.B, s.ref = Lookup(d + '/a.db'), Lookup(d + '/b.db'), Push(); s.log = []
        for q in (2, 3):
            for L in (s.A, s.B): L.mainp[q] = 0
            s.ref.bal[(0, q, 0)]
    def step(s, B, restart=False):
        s.log.append(json.dumps(B)); out = [apply(L, json.loads(s.log[-1])) for L in (s.A, s.B, s.ref)]
        assert out[0] == out[1] == out[2], 'nodes decide differently: %s' % out
        if restart: s.B = s.B.restart()
        assert s.A.digest() == s.B.digest(), 'state digests differ'
        return out[0]
    def check(s):
        n = 0
        for X in list(s.ref.bal):
            for a in list(s.ref.bal[X]) + ['nobody']:
                v = s.ref.bal[X].get(a, 0); n += 1
                assert s.A.owned(X, a) == s.B.owned(X, a) == v, ('balance', X, a, v, s.A.owned(X, a))
        assert sum(sum(d.values()) for d in s.ref.bal.values()) + s.ref.dust == s.ref.made
        return n
    def replay(s):
        C = Lookup(s.d + '/c.db'); C.mainp.update({2: 0, 3: 0})
        for l in s.log: apply(C, json.loads(l))
        assert C.digest() == s.A.digest(), 'replay differs'

def block(b, rows=(), txs=(), shares=None): return [b, list(rows), list(txs), PR[b-1], shares or {'zed': M}]
def scenario(when, fee13):
    """Alice mines 13 with Bob, buys 100,000 from Bob, sells all 13 to Ovanes in block `when`; 13 splits in 500."""
    with tempfile.TemporaryDirectory() as d:
        N = Net(d); A13, x = (0, 13, 0), (R17, 13, 0); O = []
        for b in range(3, 503):
            rows, txs, sh = [], [], None
            if b == 6: sh = {'Alice': 500000, 'Bob': 500000}
            if b == 7: sh = {'Alice': M}                                     # 17: to pay fees with
            if b == 8: sh = {'Ovanes': M}                                    # 19
            if b == 300: txs = [('Bob', A13, [('Alice', 100000)], A13 if fee13 else (0, 11, 0), 1)]
            if b == 300 and not fee13: txs = [('zed', (0, 11, 0), [('Bob', 10)], (0, 11, 0), 1)] + txs
            if b == 499: rows = [(R17, 2, 'Dan')]
            if b == 500: rows = [(R17, q, 'Dan') for q in (13, 17, 19)]
            if b == when:
                have = N.ref.owned(A13, 'Alice')
                txs = [('Alice', A13, [('Ovanes', have - (1 if fee13 else 0))], A13 if fee13 else (0, 17, 0), 1)]
            if b == 500 and when == 499:
                txs = [('Alice', x, [('Carol', 1)], (0, 17, 0), 2)]           # the seller must have nothing
            if b == 501:
                got = {a: N.A.owned(x, a) for a in ('Dan', FOUNDER(R17), 'Alice', 'Bob', 'Ovanes', POOL)}
                assert got == {a: N.ref.owned(x, a) for a in got}; O.append(('after 13 split', got))
                w = 'Ovanes' if got['Ovanes'] else 'Alice'; m = got[w]; n = N.ref.nonce[w]; F = (0, 19, 0) if w == 'Ovanes' else (0, 17, 0)
                txs = [(w, x, [('Carol', issued(x))], F, n + 1),             # all 500,000 of the new prime
                       (w, x, [('Carol', m + 1)], F, n + 1),                 # one unit too many
                       (w, x, [('Carol', m)], x, n + 1),                     # everything, fee in the same prime
                       (w, x, [('Carol', m)], F, n + 1),                     # everything, fee in another prime
                       (w, x, [('Carol', m)], F, n + 2),                     # again in the same block
                       (w, x, [('Carol', m)], F, n + 1),                     # the same transfer replayed
                       ('Carol', x, [('Eve', m - 1)], x, 1)]                 # the receiver spends at once
                who = w
            if b == 502: txs = [(who, x, [('Carol', 1)], F, n + 2)]           # again, one block later
            r = N.step(block(b, rows, txs, sh), restart=b % 50 == 0 or b >= 498)
            if txs and b != 300: O.append(('block %d' % b, r))
            if b == 300: assert r[-1] is None
        N.check(); N.replay(); O.append(('end', {a: N.A.owned(x, a) for a in (who, 'Carol', 'Eve', POOL)})); return O

def rand(seed, mut=None, last=290):
    with tempfile.TemporaryDirectory() as d:
        R = random.Random(seed); N = Net(d, mut); P = ['p%d' % i for i in range(8)] + ['Alice', 'Bob', 'Ovanes']
        front = {k: 0 for k in NAMES}; acc = ref = 0; assets = []; why = defaultdict(int)
        for b in range(3, last):
            start = dict(front); rows = []
            for k in NAMES:
                for _ in range(R.randint(0, 5)):
                    lim = b - 1 if LOWER[k] == 0 else start[LOWER[k]]
                    if front[k] >= lim: break
                    q = PR[front[k]]; front[k] += 1; rows.append((k, q, R.choice(P)))
            assets += [(k, q, i) for k, q, _ in rows for i in range(len(ROOTS[(k, q)]))]
            c = sorted(R.randint(0, M) for _ in range(2)); sh = defaultdict(int)
            for x in (c[0], c[1]-c[0], M-c[1]): sh[R.choice(P)] += x
            txs = []; nn = dict(N.ref.nonce)
            for _ in range(12):
                X = R.choice(assets[-400:]) if assets and R.random() < .7 else (0, PR[R.randrange(2, b)], 0)
                hs = [a for a, v in N.ref.bal[X].items() if v and a != POOL] if X in N.ref.bal else []
                a = R.choice(hs) if hs and R.random() < .8 else R.choice(P)
                v = N.ref.owned(X, a); kind = R.random()
                x = v - 1 if kind < .3 else v if kind < .5 else v + 1 if kind < .6 else R.randint(1, max(1, v))
                n = nn.get(a, 0) + 1
                F = X if kind < .3 or R.random() < .5 else R.choice(assets) if assets else X
                to = R.choice(P + ['n%d' % R.randrange(300)]); t = (a, X, [(to, x)], F, n)
                ok = N.ref.owned(X, a) >= x + (F == X) and (F == X or N.ref.owned(F, a) >= 1) and x > 0
                txs.append(t)
                if R.random() < .15: txs.append((a, X, [(to, x)], F, n + R.randint(0, 1)))   # double spend attempt
                if ok: nn[a] = n
            res = N.step(block(b, rows, txs, dict(sh)), restart=R.random() < .2)
            for r in res: acc += r is None; ref += r is not None; why[r] += r is not None
        n = N.check(); N.replay()
        return 'equal: %d accepted, %d refused %s, %d balances compared, loss %d of %d, %d ledger entries (written payouts would be %d)' % (
            acc, ref, {k: v for k, v in why.items() if k}, n, N.ref.dust, N.ref.made, N.A.entries, sum(len(d) for d in N.ref.bal.values()))
def scale(rings=351, nprimes=78498, keep=True):
    """Look-ups with all 351 rings to 10^6: one 4-byte segment number per row, 16 bytes per segment."""
    import struct, time
    R = random.Random(1); d = tempfile.mkdtemp(dir=os.environ.get('SCALE_DIR')); f = open(d + '/rows', 'wb'); g = open(d + '/segs', 'wb'); seg = 0
    for k in range(rings):
        buf = bytearray(); sb = bytearray(); i = 0
        while i < nprimes:
            ln = min(R.choice((2, 3, 4, 5, 6)), nprimes - i); buf += struct.pack('<I', seg) * ln; sb += struct.pack('<QII', 10**6 + i, R.randrange(5000), k); seg += 1; i += ln
        f.write(buf); g.write(sb)
    f.flush(); g.flush(); os.fsync(f.fileno()); os.fsync(g.fileno())
    for h in (f, g): os.posix_fadvise(h.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
    size = os.path.getsize(d + '/rows') + os.path.getsize(d + '/segs')
    F, G = os.open(d + '/rows', os.O_RDONLY), os.open(d + '/segs', os.O_RDONLY); ts = []
    for _ in range(3000):
        t = time.perf_counter(); n, = struct.unpack('<I', os.pread(F, 4, 4*(R.randrange(rings)*nprimes + R.randrange(nprimes)))); os.pread(G, 16, 16*n); ts.append(time.perf_counter() - t)
    ts.sort(); print('%d rows, %d segments, index %.0f MB; look-up median %.3f ms, 99%% %.3f ms, worst %.1f ms' % (rings*nprimes, seg, size/1e6, 1e3*ts[1500], 1e3*ts[2970], 1e3*ts[-1]))
    os.remove(d + '/rows'); os.remove(d + '/segs'); os.rmdir(d)

if __name__ == '__main__':
    if sys.argv[1:] == ['scale']: scale(); sys.exit()
    print('%d quadratic generators checked; rings: %s' % (checked, NAMES))
    for when, fee13, text in ((499, False, 'sold one block BEFORE the split, fee in another prime'), (499, True, 'the same, 1-unit fees paid in 13'),
                              (500, False, 'sold in the SAME block as the split')):
        print('\n13', text)
        for l in scenario(when, fee13): print('  ', *l)
    print()
    for s in range(5): print('seed', s, rand(s), flush=True)
    print()
    for m in ('block h, not h-1', 'history overwritten', 'spending not recorded', 'restart loses history'):
        out = []
        for s in range(2):
            try: rand(s, m, 150); out.append('NOT CAUGHT')
            except AssertionError as e: out.append('caught (%s)' % str(e)[:32])
        print('mistake "%s":' % m, out, flush=True)
