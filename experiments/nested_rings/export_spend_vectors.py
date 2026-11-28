#!/usr/bin/env python3
"""Export the runs of spend_test.py as test vectors for the engine (B3.3, next/tests/assets_tests.cpp).

Runs the three Alice/Ovanes scenarios and the five random runs exactly as spend_test.py does, and
writes every block (rows, transfers, main-chain shares, restarts), every decision, and at the end
of each run a digest of all balances of the "pay everyone at once" ledger.

  python3 export_spend_vectors.py > spend_vectors.txt

Format (one record per line, fields separated by spaces):
  RING k D lower v               D = 0 for the tower rings; v = value of one ring prime (A4), e*v issued
  RUN name                       a new run starts (fresh nodes; 2 and 3 exist)
  BLOCK b p restart              restart = 1 if node B restarts after the block
  SHARE address amount           main-chain reward of block b
  ROW k q miner n e0 j0 ...      n ring primes; e = ramification index, j = index of the prime below
                                 in the lower ring (-1: the main-chain prime q)
  TX sender k q i fk fq fi nonce m receiver amount ...   asset (k, q, i), fee asset, 1-unit fee
  RES r1 r2 ...                  one result per TX of the block: ok, nonce, no_such_prime, amount, not_enough
  FINAL balances dust made digest   digest = sha3-256 over sorted lines "k q i address amount\\n" of every
                                 nonzero balance of the reference ledger ('<pool>' is the fee pool)
"""
import hashlib, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spend_test as S

out = sys.stdout
def D(k): return int(S.NAMES[k][2:]) if S.NAMES[k].startswith('D=') else 0
def value(k): return S.M // (2 if D(k) else int(S.NAMES[k].split()[-1]))   # issued(X) = e * value
for k in sorted(S.NAMES): out.write('RING %d %d %d %d\n' % (k, D(k), S.LOWER[k], value(k)))

def code(r): return 'ok' if r is None else r.replace(' ', '_')

Base = S.Net
class RecNet(Base):
    def step(s, B, restart=False):
        b, rows, txs, p, shares = B
        out.write('BLOCK %d %d %d\n' % (b, p, 1 if restart else 0))
        for a, x in shares.items(): out.write('SHARE %s %d\n' % (a, x))
        for k, q, miner in rows:
            n = len(S.ROOTS[(k, q)]); parts = []
            for i in range(n):
                e = S.issued((k, q, i)) // value(k); assert e * value(k) == S.issued((k, q, i))
                Y = S.below((k, q, i))
                parts.append('%d %d' % (e, -1 if Y[0] == 0 else Y[2]))
            out.write('ROW %d %d %s %d%s\n' % (k, q, miner, n, ''.join(' ' + x for x in parts)))
        for a, X, outs, F, n in txs:
            out.write('TX %s %d %d %d %d %d %d %d %d%s\n' % (a, X[0], X[1], X[2], F[0], F[1], F[2], n, len(outs),
                                                          ''.join(' %s %d' % (c, x) for c, x in outs)))
        r = Base.step(s, B, restart)
        out.write('RES%s\n' % ''.join(' ' + code(x) for x in r))
        return r
    def check(s):
        n = Base.check(s); lines = []
        for X, d in s.ref.bal.items():
            for a, v in d.items():
                if v: lines.append((tuple(X), a, v))
        lines.sort()
        h = hashlib.sha3_256(''.join('%d %d %d %s %d\n' % (X[0], X[1], X[2], a, v) for X, a, v in lines).encode()).hexdigest()
        out.write('FINAL %d %d %d %s\n' % (len(lines), s.ref.dust, s.ref.made, h))
        return n

S.Net = RecNet
for when, fee13, name in ((499, False, 'sold_before_split_fee_elsewhere'), (499, True, 'sold_before_split_fee_in_13'),
                          (500, False, 'sold_in_block_of_split')):
    out.write('RUN %s\n' % name); S.scenario(when, fee13)
for seed in range(5):
    out.write('RUN seed%d\n' % seed); S.rand(seed)
