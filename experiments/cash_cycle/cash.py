#!/usr/bin/env python3
"""Cash cycle on the nested-ring tower: print, sell, resell, swap, deposit.

Stand-in cryptography only (RSA blind signatures, 768 bits, trusted dealer, 2-of-3 replicated
shares; Lamport one-time note keys). It tests accounting and the cycle, not security."""
import os,sys,random,hashlib,statistics
from collections import defaultdict,deque
HERE=os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0,os.path.join(HERE,'..','nested_rings'))
import simulate as T
M=10**6;DEN=[100000,10000,1000,100,10,1];R=random.Random(2026)
def H(*x):return hashlib.sha3_256(repr(x).encode()).digest()
NBITS=128  # Lamport bits (test size)
def isp(n):
    if any(n%p==0 for p in (3,5,7,11,13,17,19,23,29,31)):return False
    d=n-1;s=0
    while d%2==0:d//=2;s+=1
    for _ in range(24):
        x=pow(R.randrange(2,n-1),d,n)
        if x in(1,n-1):continue
        for _ in range(s-1):
            x=x*x%n
            if x==n-1:break
        else:return False
    return True
def genp(b):
    while True:
        p=R.getrandbits(b)|(3<<(b-2))|1
        if p%65537!=1 and isp(p):return p
class Key:
    """One RSA key per (series, denomination); d = d0+d1+d2, validator i lacks share i."""
    def __init__(s):
        p,q=genp(384),genp(384);s.n=p*q;phi=(p-1)*(q-1);s.d=pow(65537,-1,phi)
        a,b=R.randrange(phi),R.randrange(phi);s.sh=[a,b,(s.d-a-b)%phi]
    def fdh(s,m):
        o=b''.join(H(m,i) for i in range(3));return int.from_bytes(o,'big')%s.n
    def sign(s,bl,vals):
        """blind signature by a set of validators; a missing share gives a useless result"""
        r=1
        for j in range(3):
            if any(v!=j for v in vals):r=r*pow(bl,s.sh[j],s.n)%s.n
        return r
    def ok(s,m,sg):return pow(sg,65537,s.n)==s.fdh(m)
class Pair419:
    """Class-group notes (isogeny.py, p = 419). Each pair of validators has its own keys; a note
    names its pair and needs a separate blind signature from both."""
    def __init__(s):
        import isogeny as I
        s.I=I;s.pairs=((0,1),(0,2),(1,2));s.k={(p,v):I.Signer(R) for p in s.pairs for v in p}
    def ok(s,m,sg):
        if not sg or sg[0] not in s.pairs:return False
        p,sigs=sg
        return all(s.I.verify(s.k[(p,v)].pk,(p,m),sigs[i]) for i,v in enumerate(p))
    def issue(s,m,up):
        p=R.choice([p for p in s.pairs if set(p)<=up])
        return (p,tuple(s.I.blind_sign(s.k[(p,v)],(p,m),R) for v in p))
class Note:
    """Bearer note: a Lamport secret; the public key hash is the serial."""
    def __init__(s,series,den):
        s.series=series;s.den=den;s.sk=R.randbytes(32);s.sig=None
    def sec(s,i,bit):return H(s.sk,i,bit)
    def pk(s):
        return [(H(s.sec(i,0)),H(s.sec(i,1))) for i in range(NBITS)]
    def serial(s):
        if not hasattr(s,'_ser'):s._ser=H(s.pk())
        return s._ser
    def blind(s,k):
        s.r=R.randrange(2,k.n);return k.fdh((s.series,s.den,s.serial()))*pow(s.r,65537,k.n)%k.n
    def unblind(s,k,b):s.sig=b*pow(s.r,-1,k.n)%k.n
    def authorize(s,msg):
        d=H(msg);return [s.sec(i,(d[i//8]>>(i%8))&1) for i in range(NBITS)]
def lamport_ok(pk,msg,w):
    d=H(msg);return len(w)==NBITS and all(H(w[i])==pk[i][(d[i//8]>>(i%8))&1] for i in range(NBITS))
def change(v):
    o=[]
    for d in DEN:
        o+=[d]*(v//d);v%=d
    return o
class Cash:
    FEE=0   # a 1-unit fee would break round notes into change (299,999 needs 47 notes)
    CAP=None   # limit on cash outstanding, checked with every print
    def __init__(s,sim,bound=True,scheme='rsa'):
        s.scheme=scheme
        s.sim=sim;s.q=deque();s.spent=set();s.out=defaultdict(int);s.ret=set();s.keys={};s.up={0,1,2};s.bound=bound
        s.recorded=set();s.printed=0;s.deposited=0;s.fees=0;s.recordlog=[];s.rej=defaultdict(int);s.treasury=0;s.anon=[]
        s.pr_log=[];s.dep_log=[];s.pending=None
    def key(s,ser,d):
        if (ser,d) not in s.keys:s.keys[(ser,d)]=Key() if s.scheme=='rsa' else Pair419()
        return s.keys[(ser,d)]
    def outstanding(s):return sum(d*n for (_,d),n in s.out.items())
    def backing(s):return sum(v for _,v in s.q)
    def check(s):
        assert s.outstanding()==s.backing(),(s.outstanding(),s.backing())
        c=defaultdict(int)
        for a,v in s.q:c[a]+=v
        for a in s.sim.bal:
            assert s.sim.bal[a].get('VAULT',0)==c[a]
    def move(s,a,f,t,x):
        b=s.sim.bal[a];assert b[f]>=x;b[f]-=x;b[t]+=x
    def cosign(s,k,bl):
        assert bl in s.recorded   # a validator signs only what the chain has recorded as paid for
        v=sorted(s.up)
        if len(v)<2:return None
        return k.sign(bl,R.sample(v,2))
    def print_(s,who,lots,notes):
        """lock units, then notes are signed blind"""
        v=sum(x for _,x in lots)
        if v!=sum(n.den for n in notes) or any(s.sim.bal[a][who]<x for a,x in lots) or len(s.up)<2:
            s.rej['print']+=1;return False
        if s.CAP is not None and s.outstanding()+v>s.CAP:s.rej['over the cash limit']+=1;return False
        for a,x in lots:s.move(a,who,'VAULT',x);s.q.append((a,x))
        s.issue(notes);s.printed+=v;return True
    def issue(s,notes):
        for n in notes:
            k=s.key(n.series,n.den)
            if s.scheme!='rsa':
                n.sig=k.issue((n.series,n.den,n.serial()),s.up);s.out[(n.series,n.den)]+=1;s.pr_log.append((n.series,n.den,None));continue
            bl=n.blind(k);s.recorded.add(bl);sg=s.cosign(k,bl);n.unblind(k,sg)
            s.out[(n.series,n.den)]+=1;s.pr_log.append((n.series,n.den,bl))
    def valid(s,ins,msg):
        seen=set()
        for (n,w) in ins:
            ser=n['serial']
            if ser in s.spent or ser in seen:s.rej['double spend']+=1;return False
            seen.add(ser)
            if n['series'] in s.ret:s.rej['retired series']+=1;return False
            if n['den'] not in DEN or not s.key(n['series'],n['den']).ok((n['series'],n['den'],ser),n['sig']):s.rej['forged note']+=1;return False
            if s.bound and (H(n['pk'])!=ser or not lamport_ok(n['pk'],msg,w)):s.rej['not authorized by note key']+=1;return False
            if s.out[(n['series'],n['den'])]<=0:s.rej['more notes than printed']+=1;return False
        return True
    def spend(s,ins):
        for n,_ in ins:
            s.spent.add(n['serial']);s.anon.append(s.out[(n['series'],n['den'])]);s.out[(n['series'],n['den'])]-=1;s.dep_log.append((n['series'],n['den'],n['serial'],n['sig']))
    def take(s,x):
        got=[]
        if s.pending:s.q=deque(sorted(s.q,key=lambda t:s.pending(t[0])))
        while x:
            a,v=s.q.popleft();t=min(v,x);got.append((a,t));x-=t
            if v>t:s.q.appendleft((a,v-t))
        return got
    def payfee(s):
        for a,x in s.take(s.FEE):s.move(a,'VAULT','FEES',x);s.fees+=x
    def swap(s,ins,outs,msg,payment=None):
        """old notes spent, fresh notes signed; an optional payment in primes is atomic with it"""
        tot=sum(n['den'] for n,_ in ins)
        if sum(o.den for o in outs)!=tot-s.FEE or len(s.up)<2 or not s.valid(ins,msg):s.rej['swap']+=1;return False
        if payment:
            frm,to,lots=payment
            if any(s.sim.bal[a][frm]<x for a,x in lots):s.rej['buyer cannot pay: sale undone']+=1;return False
        s.spend(ins);s.payfee();s.issue(outs)
        if payment:
            for a,x in lots:s.move(a,frm,to,x)
        return True
    def deposit(s,ins,who,msg):
        tot=sum(n['den'] for n,_ in ins)
        if not s.valid(ins,msg):return False
        s.spend(ins)
        for a,x in s.take(tot):s.move(a,'VAULT',who,x)
        s.deposited+=tot;return True
    def retire(s,ser):
        s.ret.add(ser);v=0
        for k in list(s.out):
            if k[0]==ser:v+=k[1]*s.out[k];s.out[k]=0
        for a,x in s.take(v):s.move(a,'VAULT','TREASURY',x)
        s.treasury+=v;return v
def reveal(n,msg):return ({'series':n.series,'den':n.den,'serial':n.serial(),'sig':n.sig,'pk':n.pk()},n.authorize(msg))
def lots_of(sim,who,v,pick=None):
    o=[];c=[a for a in sim.assets if sim.bal[a].get(who,0)>0];R.shuffle(c)
    for a in c:
        x=min(v,sim.bal[a][who]);o.append((a,x));v-=x
        if not v:return o
    return None
def run(seed,scheme='rsa',cap=None):
    R.seed(seed);sim=T.Sim(seed);st=defaultdict(int);C=Cash(sim,scheme=scheme);C.CAP=cap;sim.alias={'VAULT':'SEIG'};wal=defaultdict(list);lost=[0];sim.assets=[]
    users=['U%d'%i for i in range(8)];seigs=[]
    if scheme!='rsa':
        C.pending=lambda a:sum(1 for k in (1,2,3) if k>a[0] and T.PR[sim.b+1]>=T.OPEN[k] and (k,a[1]) in T.roots and not sim.paidrow(k,a[1]))
    def hook(s,b,prev):
        if b%10==0:
            s.assets=R.sample(list(s.bal),min(400,len(s.bal)))
        ser=0 if b<3000 else 1
        if b%10==0 and b>=20:
            v=R.choice([1000,5000,12345,50000,100000]);buyer=R.choice(['A','B'])
            L=lots_of(s,buyer,v)
            if L:
                ns=[Note(ser,d) for d in change(v)]
                if C.print_(buyer,L,ns):wal[buyer]+=ns;st['printed']+=1
                if cap:assert C.outstanding()<=cap
        if b%10==3 and b>20:   # resale to a user, on chain (atomic) or off chain
            sel=R.choice(['A','B']);N=[n for n in wal[sel] if n.series not in C.ret][:6]
            if N:
                buyer=R.choice(['C','D']+users);tot=sum(n.den for n in N);on=R.random()<.5
                L=lots_of(s,buyer,tot-C.FEE if on else tot)
                if L is None and buyer in users: 
                    buyer=R.choice(['C','D']);L=lots_of(s,buyer,tot-C.FEE if on else tot)
                if L:
                    if on:
                        outs=[Note(ser,d) for d in change(tot-C.FEE)];msg=('sale',b,[o.serial() for o in outs]);ok=C.swap([reveal(n,msg) for n in N],outs,msg,(buyer,sel,L))
                        if not ok:
                            assert len(C.up)<2;st['sale refused, two validators down']+=1;return
                        wal[buyer]+=outs;st['sold on chain']+=1
                    else:
                        for a,x in L:C.move(a,buyer,sel,x)
                        wal[buyer]+=N;st['sold off chain']+=1
                    wal[sel]=[n for n in wal[sel] if n not in N]
        if b%10==5:            # payment from user to user, off chain; the receiver swaps in half of the cases
            f=R.choice([u for u in wal if wal[u]] or ['A']);N=[n for n in wal[f] if n.series not in C.ret][:4]
            if N and f not in('A','B'):
                t=R.choice([u for u in users if u!=f]);wal[f]=[n for n in wal[f] if n not in N]
                tot=sum(n.den for n in N)
                if R.random()<.5 and tot>1 and len(C.up)>1:
                    outs=[Note(ser,d) for d in change(tot-C.FEE)];msg=('swap',b,[o.serial() for o in outs]);assert C.swap([reveal(n,msg) for n in N],outs,msg);N2=outs
                    # payer tries to spend the old notes after handing them over
                    msg=('cheat',b);assert not C.deposit([reveal(N[0],msg)],f,msg);st['double spend refused']+=1
                else:N2=N
                if R.random()<.03:lost[0]+=sum(n.den for n in N2);st['payments lost']+=1
                else:wal[t]+=N2
                st['paid off chain']+=1
        if b%10==8:            # exchange back to primes
            f=R.choice([u for u in wal if wal[u] and u not in('A','B')] or ['C']);N=[n for n in wal[f] if n.series not in C.ret][:6]
            if N:
                msg=('deposit',b,f);assert C.deposit([reveal(n,msg) for n in N],f,msg);wal[f]=[n for n in wal[f] if n not in N];st['exchanged back']+=1
        if 1000<=b<1100:C.up={0,2}        # one validator down
        elif 1500<=b<1520:C.up={1}        # two down: nothing may be printed
        else:C.up={0,1,2}
        if 2960<=b<=3600 and b>=3000:     # old series exchanged before the deadline
            for u in list(wal):
                N=[n for n in wal[u] if n.series==0][:3]
                if N and sum(n.den for n in N)>1:
                    tot=sum(n.den for n in N);outs=[Note(1,d) for d in change(tot-C.FEE)];msg=('series',b,u,[o.serial() for o in outs])
                    if C.swap([reveal(n,msg) for n in N],outs,msg):wal[u]=[n for n in wal[u] if n not in N]+outs
        if b==3601:
            st['value left in retired series']=C.retire(0)
            z=[n for u in wal for n in wal[u] if n.series==0]
            if z:msg=('late',);assert not C.deposit([reveal(z[0],msg)],'C',msg)
        if b%50==0:C.check()
        else:assert C.outstanding()==C.backing()
    sim.hook=hook;sim.run();C.check()
    seig=sum(v.get('SEIG',0) for v in sim.bal.values())
    return sim,C,st,lost[0],seig,wal
