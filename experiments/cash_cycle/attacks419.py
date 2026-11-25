#!/usr/bin/env python3
"""Attacks on class-group notes (p = 419) in the cash ledger, including counterfeiting with stolen keys."""
from collections import defaultdict
import cash as X
from cash import Note, reveal, change, Cash
import isogeny as I


class Ledger:
    def __init__(s):
        s.bal = defaultdict(lambda: defaultdict(int))
        for p in (13, 17, 31): s.bal[(0, p)].update(A=400_000, C=400_000, D=200_000)


def fresh(v=50_000, cap=None):
    L = Ledger(); c = Cash(L, scheme='419'); c.CAP = cap; ns = [Note(0, d) for d in change(v)]
    assert c.print_('A', [((0, 13), v)], ns); c.check(); return L, c, ns


def dep(c, n, who):
    m = ('deposit', who); return c.deposit([reveal(n, m)], who, m)


L, c, n = fresh(); print('honest deposit', dep(c, n[0], 'C'), '| again', dep(c, n[0], 'D'))
m = ('deposit', 'D')
for what, ch in (('denomination', {'den': 100000}), ('series', {'series': 1}), ('pair', None)):
    r = reveal(n[1], m)
    if ch: r[0].update(ch)
    else: r[0]['sig'] = (next(p for p in ((0, 1), (0, 2), (1, 2)) if p != n[1].sig[0]), n[1].sig[1])
    print('altered', what, 'accepted', c.deposit([r], 'D', m))
r = reveal(n[1], m); p, s = n[1].sig; r[0]['sig'] = (p, (s[0], s[0])); print('one signature used twice accepted', c.deposit([r], 'D', m))
print('redirected deposit accepted', c.deposit([reveal(n[1], ('deposit', 'C'))], 'D', ('deposit', 'D'))); c.check()
L, c, n = fresh(); c.up = {0, 2}; x = [Note(0, 1000)]; a = c.print_('A', [((0, 17), 1000)], x); c.up = {1}
print('one validator down: printing', a, 'deposit', dep(c, x[0], 'C'), '| two down: printing', c.print_('A', [((0, 17), 1000)], [Note(0, 1000)])); c.check()
L, c, n = fresh(50_000, cap=60_000)
print('cash limit 60,000: 10,000 more', c.print_('A', [((0, 17), 10_000)], [Note(0, 10_000)]), '| 1 more', c.print_('A', [((0, 17), 1)], [Note(0, 1)])); c.check()
# counterfeiting: at toy size the keys are found by trying all 27 curves
L, c, n = fresh(50_000); k = c.key(0, 10000); before = sum(v['D'] for v in L.bal.values()); fk = 0
for i in range(20):
    f = Note(0, 10000); pair = (0, 1); sg = []
    for v in pair:
        t = I.Signer(X.R); t.pk = k.k[(pair, v)].pk; t.delta = 0
        t.a = next(x for x in range(I.N) if I.act(x, 0) == t.pk[0]); sg.append(I.blind_sign(t, (pair, (0, 10000, f.serial())), X.R))
    f.sig = (pair, tuple(sg)); fk += dep(c, f, 'D')
tot = sum(v['D'] for v in L.bal.values()) - before
hon = sum(dep(c, x, 'C') for x in n)
print('counterfeiter with stolen keys: 20 forged notes, accepted', fk, '| stolen', tot, 'of', 50_000, 'outstanding | honest notes still paid', hon, 'of', len(n))
print('main ledger total unchanged', sum(sum(v.values()) for v in L.bal.values()) == 3_000_000); c.check()
