#!/usr/bin/env python3
"""Which units does a redeemer receive? One user tries to turn barren primes into fertile ones.

Fertility is the real one for the degree-8 ring of the nested tower, just before it opens:
primes that split completely there will pay 0.0875 per unit to their holders; others pay 0."""
import os, random, sys
from collections import deque
HERE = os.path.dirname(os.path.abspath(__file__))
gens = {}
for line in open(os.path.join(HERE, '..', 'nested_rings', 'tower_generators.txt')):
    k, q, _ = line.split(' ', 2)
    gens.setdefault(int(q), set()).add(int(k))
LIM = 46341
sieve = bytearray([1]) * (LIM + 1)
for i in range(2, int(LIM ** .5) + 1):
    if sieve[i]:
        sieve[i * i::i] = bytearray(len(sieve[i * i::i]))
primes = [p for p in range(5, LIM) if sieve[p]]
FERTILE8 = {p for p in primes if 3 in gens.get(p, ())}    # will pay in the degree-8 ring
paid = {p: len(gens.get(p, set()) - {3}) for p in primes}  # rings that have paid so far: 0, 1 or 2


def run(rule, rounds, seed, fertile=None, rate=0.0875, public=True, vault_primes=3000, unit=1000, stake=100_000, fee_bp=0):
    R = random.Random(seed)
    fertile = FERTILE8 if fertile is None else fertile
    vault = deque((p, unit) for p in R.sample(primes, vault_primes))
    barren = [p for p in primes if p not in fertile]
    hand = [(p, unit) for p in R.sample(barren, stake // unit)]
    cost = 0
    for _ in range(rounds):
        keep = [h for h in hand if h[0] in fertile]
        lock = [h for h in hand if h[0] not in fertile]
        want = sum(x for _, x in lock)
        if not want:
            break
        cost += want * fee_bp / 10_000
        vault.extend(lock)
        got = []
        if rule == 'least paid first':
            vault = deque(sorted(vault, key=lambda h: paid[h[0]]))
        if rule == 'fewest pending first':  # pending payouts are visible only once the ring is open
            vault = deque(sorted(vault, key=lambda h: h[0] in fertile if public else 0))
        if rule == 'random':
            v = list(vault)
            R.shuffle(v)
            vault = deque(v)
        if rule == 'pro rata':
            total = sum(x for _, x in vault)
            got, rest = [], []
            left = want
            for p, x in vault:
                t = min(x * want // total, left)
                left -= t
                got.append((p, t)) if t else None
                rest.append((p, x - t))
            vault = deque(h for h in rest if h[1])
            want = left
        while want:
            p, x = vault.popleft()
            t = min(x, want)
            got.append((p, t))
            want -= t
            if x > t:
                vault.appendleft((p, x - t))
        hand = keep + got
    gain = rate * sum(x for p, x in hand if p in fertile)
    return 100 * sum(x for p, x in hand if p in fertile) / stake, gain, cost


if __name__ == '__main__':
    q17 = {p for p in primes if pow(17, (p - 1) // 2, p) in (0, 1)}   # split or ramify in Q(sqrt 17)
    cases = [('degree-8 ring, open, backlog waiting', None, .0875, True),
             ('new ring Q(sqrt 17), open, backlog waiting', q17, .35, True),
             ('new ring Q(sqrt 17), only its founder knows', q17, .35, False)]
    for name, good, rate, public in cases:
        print(name)
        for rule in ('oldest first', 'random', 'pro rata', 'least paid first', 'fewest pending first'):
            out = []
            for rounds in (1, 10, 40):
                res = [run(rule, rounds, s, good, rate, public) for s in range(5)]
                out.append('%d rounds: %.0f%% fertile, gain %.1f%%' % (
                    rounds, sum(r[0] for r in res) / 5, sum(r[1] for r in res) / 5 / 1000))
            print('  %-21s' % rule, ' | '.join(out))
    for bp in (100, 500, 1000):
        res = [run('oldest first', 40, s, q17, .35, True, fee_bp=bp) for s in range(5)]
        print('Q(sqrt 17), oldest first, round-trip fee %d%%: gain %.1f%%, fees paid %.1f%%' % (
            bp // 100, sum(r[1] for r in res) / 5000, sum(r[2] for r in res) / 5000))
