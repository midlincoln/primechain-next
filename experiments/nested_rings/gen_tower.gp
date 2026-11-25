\\ one generator per prime q that has a degree-1 prime, at each level of Q < Q(sqrt2) < Q(zeta16)+ < Q(zeta32)+
TW=vector(3);TW[1]=y^2-2;TW[2]=subst(TW[1],y,y^2-2);TW[3]=subst(TW[2],y,y^2-2);
g1(k)={my(b=bnfinit(TW[k],1),n=poldegree(TW[k]));if(b.no!=1,error("not pid"));forprime(q=2,60000,if(q>2&&#polrootsmod(TW[k],q)==0,next);my(P=idealprimedec(b,q)[1],v,e);if(P.f!=1,next);v=bnfisprincipal(b,P,1);e=lift(nfbasistoalg(b,v[2]));print(k," ",q," ",Vecrev(e,n)))};
for(k=1,3,g1(k));
