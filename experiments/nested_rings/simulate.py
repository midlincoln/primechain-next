#!/usr/bin/env python3
"""Reward flow in three nested rings, Q < Q(sqrt 2) < degree 4 < degree 8 (R11-R13).

Every generator is verified first (norm, label, automorphisms), independently of PARI.
Then two methods must agree exactly: paying every owner at once, and claiming later
from the balance history. Run: python3 simulate.py (about a few minutes)."""
import os
HERE=os.path.dirname(os.path.abspath(__file__))
import random,sys,bisect
from collections import defaultdict
LIM=60000
sv=bytearray([1])*(LIM+1);sv[0]=sv[1]=0
for i in range(2,int(LIM**.5)+1):
    if sv[i]:sv[i*i::i]=bytearray(len(sv[i*i::i]))
PR=[i for i in range(LIM+1) if sv[i]]
def pm(a,b):
    o=[0]*(len(a)+len(b)-1)
    for i,x in enumerate(a):
        for j,y in enumerate(b):o[i+j]+=x*y
    return o
TW={1:[-2,0,1]}
for k in (2,3):
    f=TW[k-1];acc=[0];pw=[1]
    for c in f:
        acc=[(acc[i] if i<len(acc) else 0)+c*(pw[i] if i<len(pw) else 0) for i in range(max(len(acc),len(pw)))]
        pw=pm(pw,[-2,0,1])
    TW[k]=acc
def det(M):
    M=[r[:] for r in M];n=len(M);s=1;pv=1
    for i in range(n-1):
        if M[i][i]==0:
            for j in range(i+1,n):
                if M[j][i]: M[i],M[j]=M[j],M[i];s=-s;break
            else:return 0
        for j in range(i+1,n):
            for l in range(i+1,n):M[j][l]=(M[j][l]*M[i][i]-M[j][i]*M[i][l])//pv
        pv=M[i][i]
    return s*M[-1][-1]
def norm(g,f):
    n=len(f)-1;rows=[];v=g+[0]*(n-len(g))
    for _ in range(n):
        rows.append(v);t=[0]+v;c=t.pop();v=[t[i]-c*f[i] for i in range(n)]
    return det(rows)
def ev(g,x,q):
    r=0
    for c in reversed(g):r=(r*x+c)%q
    return r
def cheb(a,x,q):
    p,c=2%q,x%q
    for _ in range(a-1):p,c=c,(x*c-p)%q
    return c
# --- verify every generator independently of PARI
roots={};bad=0;gensok=0
raw=[]
for line in open(os.path.join(HERE,'tower_generators.txt')):
    a=line.split(' ',2);raw.append((int(a[0]),int(a[1]),eval(a[2])))
sq2={}
for k,q,g in raw:
    n=2**k
    if abs(norm(g,TW[k]))!=q: bad+=1
    own=None
    if q==2: rs=[0]
    else:
        low=roots[(k-1,q)] if k>1 else [0]
        rs=[]
        for l in low:
            t=(l+2)%q
            if q%4==3: s=pow(t,(q+1)//4,q)
            else:
                # Tonelli
                Q=q-1;S=0
                while Q%2==0:Q//=2;S+=1
                z=2
                while pow(z,(q-1)//2,q)!=q-1:z+=1
                M,c,tt,s=S,pow(z,Q,q),pow(t,Q,q),pow(t,(Q+1)//2,q)
                while tt not in(0,1):
                    i=0;u=tt
                    while u!=1:u=u*u%q;i+=1
                    b=pow(c,1<<(M-i-1),q);M=i;c=b*b%q;tt=tt*c%q;s=s*b%q
            assert s*s%q==t; rs+= [s,q-s]
    roots[(k,q)]=sorted(rs)
    assert all(ev(TW[k],r,q)==0 for r in rs) and len(rs)==(1 if q==2 else n)
    hits=[r for r in rs if ev(g,r,q)==0]
    if len(hits)!=1:bad+=1
    if q>2:  # automorphisms: theta -> C_a(theta); one generator must give all n primes
        got=set()
        for a in range(1,2**(k+1),2):
            got|={r for r in rs if ev(g,cheb(a,r,q),q)==0}
        if got!=set(rs):bad+=1
        else:gensok+=1
for (k,q),rs in roots.items():
    if k>1:
        for r in rs: assert (r*r-2)%q in roots[(k-1,q)]
print("generators checked",len(raw),"bad",bad,"one generator gave all n primes:",gensok)

OPEN={1:3,2:46,3:46341}; FOUND={1:'A',2:'B',3:'C'}; POW={'A':40,'B':30,'C':20,'D':10}; N=list(POW)
M=10**6
class Sim:
    hook=None      # called after each block's payouts and transfers
    alias={}       # payouts earned by one address are credited to another (cash vault)
    def __init__(s,seed,transfers=False,rule='own',cad=None,lazy=False,perblock=4):
        s.R=random.Random(seed);s.tr=transfers;s.rule=rule;s.cad=cad;s.lazy=lazy;s.pb=perblock
        s.bal=defaultdict(lambda:defaultdict(int));s.hist=defaultdict(list)  # (asset,addr)->[(block,bal)]
        s.recs={};s.iss={};s.sup={};s.claimed={};s.pend=[];s.src=defaultdict(lambda:defaultdict(int));s.issued=0;s.dust=0
        s.trace=[];s.checks=0
    def credit(s,a,who,x,tag,b):
        if x==0:return
        s.src[who][tag]+=x
    def w(s):return s.R.choices(N,[POW[x] for x in N])[0]
    def run(s,stop=None):
        bal=s.bal;bal[(0,2)]['VAL']=M;s.sup[(0,2)]=M;bal[(0,3)]['VAL']=M;s.sup[(0,3)]=M;s.iss[(0,2)]=s.iss[(0,3)]=0
        ex=defaultdict(lambda:defaultdict(int)) # lazy explicit
        s.ex=ex;H=s.hist
        def setx(a,who,v,b):
            h=H[(a,who)]
            if h and h[-1][0]==b:h[-1]=(b,v)
            else:h.append((b,v))
            ex[a][who]=v
        for z in (2,3):setx((0,z),'VAL',M,0)
        front={1:0,2:0,3:0};fr=dict(front)  # index into PR of next row
        for b in range(1,len(PR)-1):
            p=PR[b+1];prev=PR[b]  # prev is final before this block
            s.b=b;s.front=front
            # ---- ring batch decided before p closes: rows q<=prev
            events=[];start=dict(front)
            for k in (1,2,3):
                if prev<OPEN[k]:continue
                for _ in range(s.pb):
                    i=front[k];paid=0;close=None;lim=b+1
                    if k>1:lim=min(lim,start[k-1])
                    while i<lim:
                        q=PR[i];isp=(k,q) in roots
                        if isp:paid+=1
                        i+=1
                        if paid and not isp or paid==32 or (not paid and i-front[k]==1024):close=i;break
                    if close is None:break
                    m=s.w();events.append((k,m,[PR[j] for j in range(front[k],close) if (k,PR[j]) in roots]));front[k]=close
            new=[]
            for k,m,qs in events:
                for q in qs:
                    rs=roots[(k,q)];iss=M//len(rs)
                    for r in rs:
                        X=(k,q,r);s.issued+=iss;s.sup[X]=iss;s.iss[X]=b
                        up=iss*35//100;fo=iss*15//100;mi=iss-up-fo
                        rec=[];S=up;kk=k;rr=r
                        while True:
                            kk-=1
                            if kk==0:rec.append(((0,q),S));break
                            rr=(rr*rr-2)%q;pa=(kk,q,rr);rec.append((pa,S//2));S-=S//2
                        cr=[(m,mi,'ring miner %d'%k),(FOUND[k],fo,'founder %d'%k)];new.append((X,rec,cr))
            # push reference
            if s.rule=='own':
                todo=[]
                for X,rec,cr in new:
                    for who,x,t in cr:todo.append((X,who,x,t))
                    for a,amt in rec:
                        tot=0
                        for who,v in bal[a].items():
                            x=amt*v//s.sup[a];tot+=x;todo.append((X,s.alias.get(who,who),x,'from level %d to level %d'%(X[0],a[0])))
                        s.dust+=amt-tot
                for X,who,x,t in todo:
                    if x:bal[X][who]+=x;s.src[who][t]+=x
                if s.lazy:
                    for X,rec,cr in new:
                        s.recs[X]=(b,rec)
                        agg=defaultdict(int)
                        for who,x,t in cr:agg[who]+=x
                        for who,x in agg.items():setx(X,who,x,b)
            else:
                for X,rec,cr in new:
                    agg=defaultdict(int)
                    rr=[]
                    for a,amt in rec:
                        snap={w:v for w,v in bal[a].items() if v};rr.append((a,amt,snap,sum(snap.values())))
                    for who in set(x for *_,sn,_t in rr for x in sn):s.pend.append((X,who,rr))
                for X,rec,cr in new:
                    for who,x,t in cr:bal[X][who]+=x;s.src[who][t]+=x
            # main block p
            c=defaultdict(int);comps=p-prev-1;per=(M//2)//comps if comps else 0;f=s.w()
            for _ in range(comps):c[s.w()]+=per
            c[f]+=M-per*comps;A=(0,p);s.sup[A]=M;s.iss[A]=b;s.issued+=M
            for who,x in c.items():
                bal[A][who]+=x;s.src[who]['main chain']+=x
                if s.lazy:setx(A,who,x,b)
            # transfers (after payouts)
            if s.hook:s.hook(s,b,prev)
            if s.tr:
                for _ in range(6):
                    a=s.R.choice(s.keys) if hasattr(s,'keys') and s.R.random()<.7 and s.keys else (0,s.R.choice(PR[2:b+2]))
                    if not bal[a]:continue
                    frm=s.R.choice(N);to=s.R.choice(N+['E%d'%s.R.randrange(40)])
                    v=bal[a][frm];x=s.R.randint(0,v) if v else 0
                    if s.lazy and a in s.recs:s.claim(a,frm,b,setx)
                    if x and frm!=to:
                        bal[a][frm]-=x;bal[a][to]+=x
                        if s.lazy:
                            assert ex[a][frm]>=x
                            setx(a,frm,ex[a][frm]-x,b);setx(a,to,ex[a][to]+x,b)
                if b%20==0:s.keys=[k for k in s.recs][-3000:]
                if s.lazy and s.recs and b%3==0:  # random claims, by anyone, for anyone; also repeated claims
                    for _ in range(30):
                        a=s.R.choice(s.keys) if getattr(s,'keys',None) else None
                        if a:s.claim(a,s.R.choice(N+['VAL','E1']),b,setx)
            if s.rule!='own':
                keep=[]
                if b%25==0 or b==len(PR)-2:
                    for X,who,rr in s.pend:
                        if b%s.cad.get(who,25)==0 or b==len(PR)-2:
                            for a,amt,sn,t in rr:
                                x=amt*sn.get(who,0)//t if t else 0
                                if x:bal[X][who]+=x;s.src[who]['from level %d to level %d'%(X[0],a[0])]+=x
                        else:keep.append((X,who,rr))
                    s.pend=keep
        s.front=front;s.setx=setx
        return s
    def paidrow(s,k,q):return bisect.bisect_left(PR,q)<s.front[k]
    def expl(s,a,who,b):
        h=s.hist.get((a,who))
        if not h:return 0
        i=bisect.bisect_right(h,(b,1<<200))-1
        return h[i][1] if i>=0 else 0
    def ent(s,X,who):
        if X not in s.recs:return 0
        b,rec=s.recs[X];return sum(amt*s.eff(a,who,b-1)//s.sup[a] for a,amt in rec)
    def eff(s,a,who,b):
        v=s.expl(a,who,b)
        if a in s.recs and s.iss[a]<=b:
            c=s.claimed.get((a,who))
            if c is None or c>b:v+=s.ent(a,who)
        return v
    def claim(s,a,who,b,setx):
        if (a,who) in s.claimed:return
        e=s.ent(a,who);s.claimed[(a,who)]=b
        setx(a,who,s.ex[a][who]+e,b)
if __name__=='__main__':
    # 1. equivalence under stress
    tot=0
    for seed in range(1,6):
        s=Sim(seed,True,'own',None,True).run();B=len(PR)
        items=[(a,w) for a in s.bal for w in set(s.bal[a])|set(N)|{'VAL','E1'}];s.R.shuffle(items)
        mism=0
        for a,w in items[:60000]:
            if s.eff(a,w,B)!=s.bal[a].get(w,0):mism+=1
        s.R.shuffle(items)
        for a,w in items:  # claim everything in random order, twice
            if a in s.recs:s.claim(a,w,B,s.setx);s.claim(a,w,B,s.setx)
        m2=sum(s.ex[a].get(w,0)!=s.bal[a].get(w,0) for a,w in items)
        held=sum(sum(v.values()) for v in s.bal.values())-2*M
        print("seed",seed,"assets",len(s.bal),"balances",len(items),"mismatch before/after claims",mism,m2,"issued",s.issued,"held+dust==issued",held+s.dust==s.issued,"dust",s.dust,"ring frontiers",[PR[min(x,len(PR)-1)] for x in s.front.values()])
