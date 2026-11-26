\\ Completeness of links: the number of ways a subring F fits into K (roots of F's polynomial in K)
\\ can be proved by one prime q where K has a prime of norm q and F's polynomial has exactly that
\\ many roots mod q (roots in K reduce to distinct roots mod q, so the count mod q is an upper bound).
default(parisizemax,150000000);
test(f,d)={my(K=nfinit(subst(f,x,y)),S=nfsubfields(K,d),out=List());
  foreach(S,s, my(g=subst(s[1],y,x)); if(poldegree(g)==poldegree(f)||poldegree(g)==1,next);
    my(r=#nfroots(K,g), first=0, best=99);
    forprime(q=3,20000, if(K.disc%q==0||poldisc(g)%q==0||K.index%q==0,next);
      if(#polrootsmod(subst(f,x,y),q)==0,next); my(c=#polrootsmod(g,q)); best=min(best,c); if(c==r,first=q;break));
    listput(out,[r,first,best]));
  Vec(out)};
run(L,d,name)={my(n=0,worst=0,never=0,c=Map());
  foreach(L,f, foreach(test(f,d),t, n++; if(t[2]==0,never++, worst=max(worst,t[2]))));
  print(name,": ",n," subrings (",#L," rings); completeness proved by one prime for all but ",never,"; largest prime needed ",worst)};
q4=concat([nflist(G,[0,2209],-1)|G<-["C4","V4","D4"]]);
run(q4,2,"quartic rings, quadratic subrings");
s6=concat([nflist(G,[0,300000],-1)|G<-["C6","D6","S3"]]); s6=[f|f<-s6,poldegree(f)==6];
run(s6,2,"sextic rings, quadratic subrings"); run(s6,3,"sextic rings, cubic subrings");
o8=[polcyclo(16),polcyclo(15),polcyclo(20),polcyclo(24),polredabs(polcompositum(polcompositum(x^2-2,x^2-3)[1],x^2-5)[1]),polredabs(polcompositum(polcompositum(x^2+1,x^2-2)[1],x^2-5)[1]),polredabs(polcompositum(x^4-2,x^2+3)[1]),polredabs(polcompositum(x^4-2,x^2-5)[1]),x^8-2];
run(o8,2,"octic rings, quadratic subrings"); run(o8,4,"octic rings, quartic subrings");
