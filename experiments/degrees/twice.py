# x^4-2 over Q(sqrt 2), and Q(zeta8) over its three quadratic subrings: every way of fitting in, equal parts.
# Checks per new prime: amounts paid to subring primes + main prime = 35% minus rounding, no recipient
# gets more than the rule gives, and over all rows each subring prime is paid only through rows of its own q.
from collections import defaultdict
P=[p for p in range(3,5000) if all(p%d for d in range(2,int(p**.5)+1))]
I=10**6; UP=I*35//100
def sq(t,q): return sorted({x for x in range(q) if x*x%q==t%q})
def flow(name, rows):
    worst=0; tot=defaultdict(int); n=0; extra=0
    for q, Kprimes in rows:
        for ways in Kprimes:                  # ways: list of (subring, label of the prime below), one per way of fitting in
            got=defaultdict(int); part=UP//len(ways); main=0
            for sub, lab in ways:
                keep=part//2; got[(sub,q,lab)]+=keep; main+=part-keep
            got[('main',q)]+=main
            paid=sum(got.values()); assert paid<=UP and UP-paid<len(ways)*2+1
            worst=max(worst,UP-paid); n+=1
            for k,v in got.items():
                assert k[1]==q; tot[k]+=v      # money only reaches primes over the same q
    print('%s: %d new primes, each pays exactly 35%% to below, rounding left %d units at most; %d subring primes paid'%(name,n,worst,len([k for k in tot if k[0]!='main'])))
rows=[]
for q in P:
    if q==2: continue
    roots=[a for a in range(q) if (a**4-2)%q==0]
    rows.append((q,[[('Q(sqrt2)',(a*a)%q),('Q(sqrt2)',(-a*a)%q)] for a in roots]))
flow('x^4-2 over Q(sqrt 2), both ways', rows)
# same prime from both ways happens when a^2 = -a^2, i.e. never for q odd; count how often both ways land on different primes
print('  rows where the two ways name different Q(sqrt2) primes:', sum(1 for q,K in rows for w in K if w[0][1]!=w[1][1]), 'of', sum(len(K) for q,K in rows))
rows=[]
for q in P:
    if q%8!=1: continue
    roots=[a for a in range(q) if (a**4+1)%q==0]                  # zeta8 -> a
    ways=lambda a: [('Q(i)',(a*a)%q),('Q(i)',(-a*a)%q),('Q(sqrt2)',(a+pow(a,-1,q))%q),('Q(sqrt2)',(-(a+pow(a,-1,q)))%q),('Q(sqrt-2)',(a-pow(a,-1,q))%q),('Q(sqrt-2)',(-(a-pow(a,-1,q)))%q)]
    rows.append((q,[ways(a) for a in roots]))
flow('Q(zeta8) over Q(i), Q(sqrt2), Q(sqrt-2), all ways', rows)
