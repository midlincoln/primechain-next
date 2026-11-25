"""Abstract 3-level tower. Push (credit everybody at once) vs look-up (nothing written for an owner
until they spend): owned(X,a,t) = due(X,a) + net transfers of X by a up to t."""
import random,sys,bisect
from collections import defaultdict
M=10**6
def run(seed,mut=None,blocks=500):
    R=random.Random(seed); A=['a%d'%i for i in range(12)]; V=['v0','v1','v2']
    bal=defaultdict(lambda:defaultdict(int))           # push reference
    net=defaultdict(list); rows={}; pays=defaultdict(list); made={}; below={}; stored=changes=lookups=0
    main={}; compare=0
    def nett(X,a,t):
        h=net.get((X,a),[]); i=bisect.bisect_right(h,(t,float('inf')))
        return h[i-1][1] if i else 0
    def due(X,a):
        nonlocal lookups
        lookups+=1
        if X[0]==0: return main.get(X,{}).get(a,0)
        h,miner,founder,mi,fo,recs,vals=rows[X]; s=(mi if a==miner else 0)+(fo if a==founder else 0)
        for L,amt in recs:
            t=h if mut=='wrong moment' else h-1
            s+=amt*owned(L,a,t)//made[L]
        if vals and a in vals[1]:
            q,rem=divmod(vals[0],len(vals[1])); s+=q+(vals[1].index(a)<rem)
        return 0 if (mut=='transfers only' and X[0]>0) else s
    def owned(X,a,t): return due(X,a)+nett(X,a,t)
    def change(X,a,x,b):
        nonlocal stored,changes
        changes+=1; h=net[(X,a)]; cur=h[-1][1] if h else 0
        # keep the previous entry only if X paid something after it: payout at p with b0 < p <= b
        keep = not h or any(h[-1][0]<p<=b for p in pays[X])
        if mut=='always overwrite': keep=not h
        if h and not keep: h[-1]=(b,cur+x)
        else: h.append((b,cur+x)); stored+=1
        if len(h)>1 and h[-2][0]==b: h[-2:]=[h[-1]]
    front=[None,0,0,0]; order=[]
    for b in range(1,blocks):
        # ring rows paid in block b; a higher level waits for rows of the level below in earlier blocks
        new=[]; lim=[len(order)]+front[1:]
        for k in (1,2,3):
            for _ in range(R.randint(0,3)):
                if front[k]>=min(lim[:k]): break
                i=front[k]; front[k]+=1; q=order[i]
                parents=[(0,i)] if k==1 else [X for X in rows if X[0]==k-1 and X[1]==i]
                for P in parents:
                    if R.random()<.5: continue
                    for j in (0,1):
                        X=(k,i,P[2:]+(j,)); I=M>>k; up=I*35//100; fo=I*15//100; mi=I-up-fo
                        recs=[];S=up;c=P;kk=k
                        while True:
                            if c[0]==0: recs.append((c,S));break
                            recs.append((c,S//2));S-=S//2;c=below[c]
                        new.append((X,P,I,mi,fo,recs,R.choice(A),'F%d'%k))
        todo=[]
        for X,P,I,mi,fo,recs,m,f in new:
            below[X]=P; made[X]=I; vals=None
            if X[1]<2:      # genesis primes: nobody holds them, the bottom share goes to validators
                s=recs[-1][1]; recs=recs[:-1]; vals=(s,tuple(V));
                for n,v in enumerate(V): todo.append((X,v,s//3+(n<s%3)))
            todo+= [(X,m,mi),(X,f,fo)]
            for L,amt in recs:
                for a,v in bal[L].items(): todo.append((X,a,amt*v//made[L]))
            rows[X]=(b,m,f,mi,fo,recs,vals)
        for X,P,I,mi,fo,recs,m,f in new:
            for L,_ in rows[X][5]: pays[L].append(b)
        for X,a,x in todo: bal[X][a]+=x
        # main prime
        i=len(order); order.append(i); X=(0,i); made[X]=M
        if i>=2:
            cut=sorted(R.randint(0,M) for _ in range(2)); d=defaultdict(int)
            for x in (cut[0],cut[1]-cut[0],M-cut[1]): d[R.choice(A)]+=x
            main[X]=dict(d)
            for a,x in d.items(): bal[X][a]+=x
        for _ in range(10):      # transfers, after payouts
            X=R.choice(list(rows)) if rows and R.random()<.7 else (0,R.randrange(len(order)))
            hs=[a for a in bal[X] if bal[X][a]]
            if not hs: continue
            a=R.choice(hs); c=R.choice(A+['n%d'%R.randrange(300)]); x=R.randint(1,bal[X][a])
            o=owned(X,a,b); compare+=1
            if o!=bal[X][a]: return 'DIFFERENT at block %d'%b
            if a==c: continue
            bal[X][a]-=x; bal[X][c]+=x; change(X,a,-x,b); change(X,c,x,b)
    for X in bal:
        for a in set(bal[X])|set(A[:3]):
            compare+=1
            if owned(X,a,b)!=bal[X][a]: return 'DIFFERENT at end'
    pushed=sum(1 for X in bal for a in bal[X])
    return 'equal: %d comparisons, %d ring primes, %d balances if written, %d entries stored, %d changes'%(compare,len(rows),pushed,stored,changes)
for s in range(5): print('seed',s,run(s))
for m in ('wrong moment','always overwrite','transfers only'):
    print('mutation:',m,[run(s,m) .split(':')[0][:24] for s in range(3)])
