#!/usr/bin/env python3
"""Attacks on the cash cycle, each on a small fresh ledger (stand-in cryptography, see cash.py)."""
from collections import defaultdict
import cash as X
from cash import Note, reveal, change, Cash, H


class Ledger:
    def __init__(s):
        s.bal = defaultdict(lambda: defaultdict(int))
        for p in (13, 17, 31):
            s.bal[(0, p)].update(A=400_000, C=400_000, D=200_000)


def fresh(bound=True, v=50_000):
    L = Ledger(); c = Cash(L, bound)
    ns = [Note(0, d) for d in change(v)]
    assert c.print_('A', [((0, 13), v)], ns); c.check()
    return L, c, ns


def dep(c, n, who):
    m = ('deposit', who)
    return c.deposit([reveal(n, m)], who, m)


# 1 double spend, also inside one deposit
L, c, n = fresh(); assert dep(c, n[0], 'C'); a = dep(c, n[0], 'D'); m = ('deposit', 'D')
b = c.deposit([reveal(n[1], m), reveal(n[1], m)], 'D', m)
print('1 double spend, twice / within one deposit: accepted', a, b, dict(c.rej)); c.check()
# 2 forged note, altered denomination, altered series
L, c, n = fresh(); f = Note(0, 100000); f.sig = n[0].sig; r1 = dep(c, f, 'D')
m = ('deposit', 'D'); r = reveal(n[0], m); r[0]['den'] = 100000; r2 = c.deposit([r], 'D', m)
r = reveal(n[0], m); r[0]['series'] = 1; r3 = c.deposit([r], 'D', m)
print('2 forged / altered denomination / altered series: accepted', r1, r2, r3); c.check()
# 3 thief sees a deposit in transit and redirects it
for bound in (False, True):
    L, c, n = fresh(bound); m = ('deposit', 'C'); seen = reveal(n[0], m)      # C's transaction, visible to everyone
    stolen = c.deposit([seen], 'D', ('deposit', 'D'))
    print('3 redirect a deposit in transit, note', 'with its own key' if bound else 'as a bare serial', ': stolen', stolen); c.check()
# 4 validators printing secretly
L, c, n = fresh(); k = c.key(0, 10000)
def secret(vals):
    x = Note(0, 10000); x.unblind(k, k.sign(x.blind(k), vals)); return x
s = secret([1]); print('4a one validator alone: valid note', k.ok((0, 10000, s.serial()), s.sig))
fk = [secret([0, 2]) for _ in range(5)]
ok = [dep(c, x, 'D') for x in fk]; print('4b two validators together print 5 extra notes: deposits accepted', sum(ok))
hon = [dep(c, x, 'C') for x in n]; print('   honest notes', len(hon), 'redeemed', sum(hon), 'refused', len(hon) - sum(hon), dict(c.rej)); c.check()
# 5 availability
L, c, n = fresh(); c.up = {0, 2}; x = [Note(0, 1000)]; a = c.print_('A', [((0, 17), 1000)], x)
c.up = {1}; y = [Note(0, 1000)]; b = c.print_('A', [((0, 17), 1000)], y); d = dep(c, n[0], 'C')
print('5 one down: printing', a, '| two down: printing', b, '(nothing locked), deposit check needs no signing key', d); c.check()
# 6 sale on chain: buyer without funds; sale off chain: seller keeps a copy
L, c, n = fresh(); o = [Note(0, 10000)]; m = ('sale', o[0].serial())
a = c.swap([reveal(n[0], m)], o, m, ('E', 'A', [((0, 31), 10000)])); still = dep(c, n[0], 'A')
print('6a buyer cannot pay: sale done', a, '| seller still owns the note', still); c.check()
L, c, n = fresh(); c.move((0, 31), 'C', 'A', 10000); first = dep(c, n[0], 'A'); second = dep(c, n[0], 'C')
print('6b off chain, buyer pays first and seller deposits the kept copy: seller', first, 'buyer', second, '-> buyer loses unless they swap at once'); c.check()
# 7 blindness: every printed blinded value fits every deposited note of its kind
L, c, n = fresh(v=50_000); [dep(c, x, 'C') for x in n]; fit = tot = 0
for (s1, d1, bl) in c.pr_log:
    for (s2, d2, ser, sig) in c.dep_log:
        if (s1, d1) == (s2, d2):
            kk = c.key(s1, d1); rr = pow(bl * pow(kk.fdh((s1, d1, ser)), -1, kk.n) % kk.n, kk.d, kk.n); tot += 1
            fit += bl == kk.fdh((s1, d1, ser)) * pow(rr, 65537, kk.n) % kk.n
print('7 blindness: print/deposit pairs consistent', fit, 'of', tot)
# 8 retired series
L, c, n = fresh(); dep(c, n[0], 'C'); left = c.retire(0); print('8 retired: late deposit accepted', dep(c, n[1], 'C'), '| backing moved', left, '| outstanding', c.outstanding()); c.check()
