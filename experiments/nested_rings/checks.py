#!/usr/bin/env python3
"""Mutation checks (each must be caught) and income tables for the nested-ring simulation."""
import os
HERE=os.path.dirname(os.path.abspath(__file__))
import resource;resource.setrlimit(resource.RLIMIT_AS,(800<<20,800<<20))
import io,contextlib
src=open(os.path.join(HERE,'simulate.py')).read().split("if __name__=='__main__'")[0]
def load(t):
    g={'__name__':'x','__file__':os.path.join(HERE,'simulate.py')}
    with contextlib.redirect_stdout(io.StringIO()):exec(t,g)
    return g
G=load(src);Sim=G['Sim'];N=G['N'];PR=G['PR']
# mutations: each must be caught
muts={'balance read at h instead of h-1':("s.eff(a,who,b-1)//","s.eff(a,who,b)//"),
 'double claim allowed':("if (a,who) in s.claimed:return","pass"),
 'transfer does not claim first':("if s.lazy and a in s.recs:s.claim(a,frm,b,setx)","pass"),
 'no remainder passed down (keep half, drop odd unit)':("S-=S//2","S=S//2")}
for name,(a,b) in muts.items():
    assert src.count(a)==1,name
    try:
        g=load(src.replace(a,b));s=g['Sim'](1,True,'own',None,True).run();B=len(PR)
        items=[(x,w) for x in s.bal for w in set(s.bal[x])|set(N)]
        for x,w in items:
            if x in s.recs:s.claim(x,w,B,s.setx);s.claim(x,w,B,s.setx)
        m=sum(s.ex[x].get(w,0)!=s.bal[x].get(w,0) for x,w in items)
        held=sum(sum(v.values()) for v in s.bal.values())-2*10**6
        print('mutation:',name,'-> mismatches',m,'conservation',held+s.dust==s.issued)
    except AssertionError as e:print('mutation:',name,'-> caught by assertion')
def table(title,s):
    print('\n'+title);tags=sorted({t for w in s.src for t in s.src[w]})
    tot=defaultdict(int)
    for w in N+['VAL']:
        d=s.src[w];up={k:sum(v for t,v in d.items() if t.startswith('from') and t.endswith('level %d'%k)) for k in (0,1,2)}
        mi=sum(v for t,v in d.items() if t.startswith('ring'));fo=sum(v for t,v in d.items() if t.startswith('founder'))
        print(w,'main %.1f ringmine %.1f founder %.1f up0 %.1f up1 %.1f up2 %.1f total %.1f'%tuple(x/1e6 for x in (d['main chain'],mi,fo,up[0],up[1],up[2],sum(d.values()))))
    return
from collections import defaultdict
table('all owned units count',Sim(7).run())
cad={'A':25,'B':100,'C':1000,'D':10**9}
table('claimed only: A every 25 blocks, B 100, C 1000, D at the end',Sim(7,rule='claimed',cad=cad).run())
table('claimed only: everyone every 25',Sim(7,rule='claimed',cad={w:25 for w in N}).run())
print('opening blocks',[next(i for i,p in enumerate(PR) if p>=x) for x in (3,46,46341)],len(PR))
# trace 31
s=Sim(7).run()
for a in sorted(x for x in s.bal if x[1]==31):print(a,dict(s.bal[a]),'block',s.iss[a])
