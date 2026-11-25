#!/usr/bin/env python3
"""How can two validators sign one blind note? One dishonest validator (0) signs anything for free;
validators 1 and 2 sign exactly once per paid note that names them. Class-group notes, p = 419."""
import random
import isogeny as I
R = random.Random(7)
print('paid notes -> valid notes a cheater obtains')


def any_two(tokens, named):
    """one key per validator; a note needs any two signatures; the chain names the pair, or nobody (all may sign)"""
    S = [I.Signer(R) for _ in range(3)]; notes = {}; used = 0; spare = []
    def give(v, m): notes.setdefault(m, {})[v] = I.blind_sign(S[v], m, R)
    for t in range(tokens):
        for v in ((1, 2) if named else (1, 2)): give(v, ('n', t, v)); give(0, ('n', t, v))
    return sum(sum(I.verify(S[v].pk, m, g) for v, g in d.items()) >= 2 for m, d in notes.items())


def honest_three(tokens):
    """no pair named, all three honest: three signatures per token instead of two"""
    S = [I.Signer(R) for _ in range(3)]; notes = {}; got = [(v, t) for t in range(tokens) for v in range(3)]
    # pair up 3*tokens signatures into notes with two different validators each
    per = [[v for v, _ in got if v == w] for w in range(3)]; i = 0; cnt = [tokens] * 3
    while sorted(cnt)[1] > 0:
        a, b = sorted(range(3), key=lambda w: -cnt[w])[:2]; cnt[a] -= 1; cnt[b] -= 1; m = ('h', i); i += 1
        notes[m] = {a: I.blind_sign(S[a], m, R), b: I.blind_sign(S[b], m, R)}
    return sum(all(I.verify(S[v].pk, m, g) for v, g in d.items()) for m, d in notes.items())


def pair_keys(tokens, pair):
    """a key per (pair, validator); a note names its pair and needs both"""
    S = {v: I.Signer(R) for v in pair}; notes = {}
    for t in range(tokens):
        for v in pair:
            if v != 0: m = ('p', t, v); notes.setdefault(m, {})[v] = I.blind_sign(S[v], m, R)
    for m in notes:
        if 0 in pair: notes[m][0] = I.blind_sign(S[0], m, R)
    # honest pair (1,2): the cheater can only use both signatures on one message
    if 0 not in pair:
        notes = {('p', t): {v: I.blind_sign(S[v], ('p', t), R) for v in pair} for t in range(tokens)}
    return sum(len(d) == 2 and all(I.verify(S[v].pk, m, g) for v, g in d.items()) for m, d in notes.items())


T = 6
print('any two validators, nobody named, all honest:   ', T, '->', honest_three(T))
print('any two validators, honest pair named, 1 cheats:', T, '->', any_two(T, True))
print('pair keys, pair (1,2), validator 0 cheats:      ', T, '->', pair_keys(T, (1, 2)))
print('pair keys, pair (0,1), validator 0 cheats:      ', T, '->', pair_keys(T, (0, 1)))
