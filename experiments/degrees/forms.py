#!/usr/bin/env python3
"""All cubic fields with |D| <= X, from reduced binary cubic forms (Delone-Faddeev, Belabas)."""
import sys
def disc(a,b,c,d): return b*b*c*c - 4*a*c**3 - 4*b**3*d - 27*a*a*d*d + 18*a*b*c*d
def reduced(a,b,c,d):
    D = disc(a,b,c,d)
    if a <= 0 or b < 0 or (b == 0 and d <= 0): return False
    if D < 0:
        return d*d - a*a > b*d - a*c and -(a-b)**2 - a*c < a*d - b*c < (a+b)**2 + a*c
    P, Q, R = b*b - 3*a*c, b*c - 9*a*d, c*c - 3*b*d
    if not abs(Q) <= P <= R: return False
    if Q == 0 and d >= 0: return False
    if P == Q and not b < abs(3*a - b): return False
    if P == R and not (a <= abs(d) and (abs(d) != a or b < abs(c))): return False
    return True
def table(a,b,c,d):                            # basis 1, w, t
    E = [[1,0,0],[0,1,0],[0,0,1]]
    ww, wt, tt = [-a*c, b, -a], [-a*d, 0, 0], [-b*d, d, -c]
    return [E, [E[1], ww, wt], [E[2], wt, tt]]
def rational_root(a,b,c,d):
    if a == 0 or d == 0: return True
    for x in range(1, abs(d)+1):
        if d % x: continue
        for y in range(1, abs(a)+1):
            if a % y: continue
            for s in (x, -x):
                if a*s**3 + b*s*s*y + c*s*y*y + d*y**3 == 0: return True
    return False
def all_forms(X):
    from degree import maximal
    out = []
    for a in range(1, 7):
        for b in range(0, 18):
            for c in range(-60, 61):
                for d in range(-80, 81):
                    D = disc(a,b,c,d)
                    if D == 0 or abs(D) > X or not reduced(a,b,c,d) or rational_root(a,b,c,d): continue
                    if maximal(table(a,b,c,d), D): out.append((D,a,b,c,d))
    return sorted(out, key=lambda x: (abs(x[0]), x))
if __name__ == '__main__':
    for f in all_forms(int(sys.argv[1])): print(*f)
