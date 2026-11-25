#!/usr/bin/env python3
"""Toy class-group action on the supersingular curves over F_419, and blind signatures on it.

p = 419 = 4*3*5*7 - 1. Curves E_A: y^2 = x^3 + A x^2 + x. The 3-, 5- and 7-isogenies are computed
with Velu's formulas; the class group of Z[sqrt(-419)] acts on the curves. Blind signature: the
three-move scheme of CSI-Otter (Katsumata, Lai, LeGrow, Qin, 2023), 256 repetitions. 27 curves
give NO security: this shows the mathematics only. p = 419 is the CSIDH paper's own toy example."""
import hashlib, random

P = 419
ELLS = (3, 5, 7)
inv = lambda x: pow(x, P - 2, P)
is_sq = lambda x: x % P == 0 or pow(x, (P - 1) // 2, P) == 1
sqrt = lambda x: pow(x, (P + 1) // 4, P)


def add(S, T, A):
    if S is None: return T
    if T is None: return S
    (x1, y1), (x2, y2) = S, T
    if x1 == x2 and (y1 + y2) % P == 0: return None
    l = (3 * x1 * x1 + 2 * A * x1 + 1) * inv(2 * y1) % P if S == T else (y2 - y1) * inv(x2 - x1) % P
    x3 = (l * l - A - x1 - x2) % P
    return x3, (l * (x1 - x3) - y1) % P


def mul(n, S, A):
    R = None
    while n:
        if n & 1: R = add(R, S, A)
        S = add(S, S, A); n >>= 1
    return R


def points(A):
    pts = [None]
    for x in range(P):
        f = (x * x * x + A * x * x + x) % P
        if is_sq(f):
            y = sqrt(f)
            pts += [(x, y)] if y == 0 else [(x, y), (x, P - y)]
    return pts


def isogeny(A, l):
    """codomain of the isogeny whose kernel is the F_p-rational l-torsion (the action of one ideal of norm l)"""
    for x in range(1, P):
        f = (x * x * x + A * x * x + x) % P
        if f and is_sq(f):
            K = mul((P + 1) // l, (x, sqrt(f)), A)
            if K is not None: break
    xs = []; T = K
    for _ in range((l - 1) // 2):
        xs.append(T[0]); T = add(T, K, A)
    s = sum(xs) % P; t = sum(inv(x) for x in xs) % P; pr = 1     # Montgomery Velu (Costello-Hisil, Renes)
    for x in xs: pr = pr * x % P
    return (A + 6 * t - 6 * s) * pr * pr % P


def build():
    """every supersingular curve in the orbit of E_0, and the action tables, all by real isogenies"""
    step = {l: {} for l in ELLS}; todo = [0]; seen = {0}
    while todo:
        A = todo.pop()
        assert len(points(A)) == P + 1                          # supersingular
        for l in ELLS:
            B = isogeny(A, l); step[l][A] = B
            if B not in seen: seen.add(B); todo.append(B)
    return seen, step


CURVES, STEP = build()
N = len(CURVES)
ORB = [0]
while len(ORB) < N: ORB.append(STEP[3][ORB[-1]])
assert len(set(ORB)) == N and STEP[3][ORB[-1]] == 0              # the 3-isogeny alone generates: cyclic of order N
IDX = {A: i for i, A in enumerate(ORB)}
EXP = {l: IDX[STEP[l][0]] for l in ELLS}                         # each l-isogeny as a power of the generator


def selfcheck():
    assert N == 27
    for A in CURVES:                                             # commutes everywhere, acts as a translation
        for l in ELLS: assert IDX[STEP[l][A]] == (IDX[A] + EXP[l]) % N
        for a in ELLS:
            for b in ELLS: assert STEP[a][STEP[b][A]] == STEP[b][STEP[a][A]]
        assert IDX[-A % P] == -IDX[A] % N                        # the quadratic twist inverts
    return dict(curves=N, exponents=EXP)


def act(s, A): return ORB[(IDX[A] + s) % N]                      # class-group action by generator^s
def twist(A, d): return A if d == 1 else -A % P


K = 256


def challenge(Z0, Z1, msg):
    h = hashlib.shake_256(repr((Z0, Z1, msg)).encode()).digest(K // 8)
    return [1 if h[i // 8] >> (i % 8) & 1 else -1 for i in range(K)]


class Busy(Exception): pass


class Signer:
    """one validator key; one signing session at a time unless concurrent=True (which is forgeable)"""
    def __init__(s, rng, concurrent=False):
        s.R = rng; s.conc = concurrent; s.open = {}; s.done = 0; s.n = 0
        s.delta = rng.randrange(2); a = [rng.randrange(1, N), rng.randrange(1, N)]
        s.a = a[s.delta]; s.pk = tuple(act(x, 0) for x in a)     # only a[delta] is kept

    def commit(s):
        if s.open and not s.conc: raise Busy()
        R = s.R; d = s.delta; y = [R.randrange(N) for _ in range(K)]
        c = [R.choice((1, -1)) for _ in range(K)]; r = [R.randrange(N) for _ in range(K)]
        Y = [None, None]; Y[d] = [act(v, 0) for v in y]
        Y[1 - d] = [act(r[j], twist(s.pk[1 - d], c[j])) for j in range(K)]
        s.n += 1; s.open[s.n] = (y, c, r)
        return s.n, Y

    def respond(s, sid, cs):
        y, c, r = s.open.pop(sid); d = s.delta
        cd = [cs[j] * c[j] for j in range(K)]; rd = [(y[j] - s.a * cd[j]) % N for j in range(K)]
        s.done += 1
        return ((cd, rd), (c, r)) if d == 0 else ((c, r), (cd, rd))

    def abort(s, sid): s.open.pop(sid, None)


class User:
    def __init__(s, pk, Y, msg, rng, forced=None):
        s.pk, s.Y, s.msg = pk, Y, msg
        s.d = [[rng.choice((1, -1)) for _ in range(K)] for _ in (0, 1)]
        s.z = [[rng.randrange(N) for _ in range(K)] for _ in (0, 1)]
        s.Z = [[act(s.z[b][j], twist(Y[b][j], s.d[b][j])) for j in range(K)] for b in (0, 1)]
        s.c = challenge(s.Z[0], s.Z[1], msg)
        s.cs = [s.c[j] * s.d[0][j] * s.d[1][j] for j in range(K)]

    def finish(s, resp):
        out = []
        for b in (0, 1):
            c, r = resp[b]
            if any(act(r[j], twist(s.pk[b], c[j])) != s.Y[b][j] for j in range(K)): return None
            out.append((bytes((c[j] * s.d[b][j]) & 3 for j in range(K)), bytes((s.z[b][j] + r[j] * s.d[b][j]) % N for j in range(K))))
        if any(resp[0][0][j] * resp[1][0][j] != s.cs[j] for j in range(K)): return None
        return tuple(out)


SG = {1: 1, 3: -1}


def verify(pk, msg, sig):
    if sig is None or len(sig) != 2: return False
    Z = []; cc = []
    for b in (0, 1):
        c, r = sig[b]
        if len(c) != K or len(r) != K or any(x not in SG for x in c) or any(x >= N for x in r): return False
        c = [SG[x] for x in c]; cc.append(c)
        Z.append([act(r[j], twist(pk[b], c[j])) for j in range(K)])
    h = challenge(Z[0], Z[1], msg)
    return all(cc[0][j] * cc[1][j] == h[j] for j in range(K))


def blind_sign(signer, msg, rng):
    sid, Y = signer.commit(); u = User(signer.pk, Y, msg, rng)
    return u.finish(signer.respond(sid, u.cs))


def mix_and_match(signer, rng):
    """Do-Hanzlik-Paracucchi idea: K overlapping sessions give K+1 valid signatures, whatever the group size"""
    ses = [signer.commit() for _ in range(K)]
    d = [[rng.choice((1, -1)) for _ in range(K)] for _ in (0, 1)]; z = [[rng.randrange(N) for _ in range(K)] for _ in (0, 1)]
    Z = [[act(z[b][j], twist(ses[j][1][b][j], d[b][j])) for j in range(K)] for b in (0, 1)]
    h = challenge(Z[0], Z[1], 'forged'); want = [h[j] * d[0][j] * d[1][j] for j in range(K)]
    sigs = []; tries = 0; f = [[[], []], [[], []]]
    for j, (sid, Y) in enumerate(ses):
        t = 0
        while True:
            tries += 1; u = User(signer.pk, Y, ('paid', j, t), rng); t += 1
            if u.cs[j] == want[j]: break
        resp = signer.respond(sid, u.cs); sigs.append((u.msg, u.finish(resp)))
        for b in (0, 1):
            c, r = resp[b][0][j], resp[b][1][j]
            f[b][0].append((c * d[b][j]) & 3); f[b][1].append((z[b][j] + r * d[b][j]) % N)
    sigs.append(('forged', tuple((bytes(f[b][0]), bytes(f[b][1])) for b in (0, 1))))
    return sigs, tries


if __name__ == '__main__':
    R = random.Random(419); print('self-check', selfcheck()); s = Signer(R)
    ok = 0; bad = 0
    for i in range(200):
        g = blind_sign(s, ('note', i), R); ok += verify(s.pk, ('note', i), g); bad += verify(s.pk, ('note', i + 1), g)
        c, r = g[0]; k = R.randrange(K); r2 = bytearray(r); r2[k] = (r2[k] + 1) % N; bad += verify(s.pk, ('note', i), ((c, bytes(r2)), g[1]))
        bad += verify(Signer(R).pk, ('note', i), g)
    print('signed and verified', ok, 'of 200; wrong note, altered or other key accepted', bad)
    sid, _ = s.commit()
    try: s.commit(); print('overlap allowed')
    except Busy: print('second overlapping session refused')
    s.abort(sid)
    # blindness: every session transcript fits every signature through exactly one blinding
    tr = []
    for i in range(12):
        sid, Y = s.commit(); u = User(s.pk, Y, i, R); rs = s.respond(sid, u.cs); tr.append((Y, u.cs, rs, i, u.finish(rs)))
    fit = 0
    for Y, cs, rs, _, _ in tr:
        for _, _, _, m, g in tr:
            good = True; dd = []
            for b in (0, 1):
                c, r = rs[b]; c2 = [SG[x] for x in g[b][0]]; d = [c[j] * c2[j] for j in range(K)]; dd.append(d)
                z = [(g[b][1][j] - r[j] * d[j]) % N for j in range(K)]
                good &= all(act(z[j], twist(Y[b][j], d[j])) == act(g[b][1][j], twist(s.pk[b], c2[j])) for j in range(K))
            h = [SG[x] * SG[y] for x, y in zip(g[0][0], g[1][0])]
            good &= all(h[j] * dd[0][j] * dd[1][j] == cs[j] for j in range(K)); fit += good
    print('blindness: sessions and signatures that fit', fit, 'of', len(tr) ** 2)
    c = Signer(R, concurrent=True); sigs, tries = mix_and_match(c, R)
    print('overlapping sessions allowed: signed', c.done, 'valid signatures obtained', sum(verify(c.pk, m, g) for m, g in sigs), 'distinct messages', len({m for m, _ in sigs}), 'hash tries', tries)
    a = next(x for x in range(N) if act(x, 0) == s.pk[0]); print('toy size: secret key found by trying all curves:', a)
