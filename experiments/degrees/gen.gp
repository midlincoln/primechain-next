\\ PARI side: multiplication tables, PID generators and rows. Checked independently by check.py.
default(parisizemax,250000000); default(realprecision,100);
ROWMAX = 60;
red(K,g) = {  \\ shorten a generator by units (nearest point in the log lattice)
  my(r=#K.fu); if(r==0, return(g)); g=nfalgtobasis(K,g);
  my(f=(x->my(e=nfeltembed(K,x));vector(r,i,if(i<=K.r1,1,2)*log(abs(e[i])))~));
  my(n=poldegree(K.pol), N=log(abs(nfeltnorm(K,g)))/n);
  my(t=f(g)-vector(r,i,if(i<=K.r1,1,2)*N)~, L=matconcat(vector(r,j,f(K.fu[j]))));
  my(x=round(matsolve(L,t)));
  for(j=1,r,if(x[j],g=nfeltmul(K,g,nfeltpow(K,K.fu[j],-x[j])))); nfalgtobasis(K,g)};
field(pol,B0,tag) = {
  my(K=bnfinit(pol,1)); my(n=poldegree(pol));
  if(K.no>1, print("NOPID ",tag," ",K.disc," ",K.no); return);
  my(B=if(B0==0,matid(n),matconcat(vector(n,j,nfalgtobasis(K,subst(B0[j],'x,Mod('x,K.pol)))))), C=B^-1);
  if(denominator(C)!=1, error("basis ",tag));
  my(T=vector(n,i,vector(n,j,C*nfalgtobasis(K,nfeltmul(K,B[,i],B[,j])))));
  my(M=floor((4/Pi)^K.r2*n!/n^n*sqrt(abs(K.disc))), gens=List(), rows=List());
  my(g(P)=nfalgtobasis(K,red(K,bnfisprincipal(K,P,1)[2])));
  forprime(p=2,max(M,ROWMAX),
    my(L=idealprimedec(K,p), s=[P|P<-L,p^P.f<=M&&P.f<n], o=[P|P<-L,P.f==1]);
    if(p<=M, listput(gens,[p,[C*g(P)|P<-s]]));
    if(p<=ROWMAX, listput(rows,[p,[[C*g(P),P.e]|P<-o]])));
  my(q(v)=Vec(v));
  print("FIELD ",tag," ",n," ",K.r2," ",K.disc," ",[[q(x)|x<-y]|y<-T]," ; ",[[y[1],[q(x)|x<-y[2]]]|y<-gens]," ; ",[[y[1],[[q(x[1]),x[2]]|x<-y[2]]]|y<-rows]," ; ",[q(C*nfalgtobasis(K,u))|u<-K.fu]," ; ",polgalois(pol)[4])};
cubics() = {my(v=readstr("cubic_forms.txt"));
  for(k=1,#v, my(w=strsplit(v[k]," ")); w=apply(eval,w); my(a=w[2],b=w[3],c=w[4],d=w[5]);
    my(p='x^3-b*'x^2+a*c*'x-a^2*d);          \\ minimal polynomial of w; t = -c + b*(w/a) - w^2/a
    my(K=nfinit(p));                          \\ keep this polynomial, so the basis can be named
    field(p,[1,'x,-c+b*'x/a-'x^2/a],Str(a,",",b,",",c,",",d)))};
others(n,G,X) = {my(L=nflist(G,[0,X],-1)); if(#L==0,return); foreach(L,f, field(polredbest(f),0,G))};
\\ small fields of degree n by brute force over polynomials with coefficients -1, 0, 1
brute(n,X,mx) = {my(S=List(),c=0); for(k=0,3^n-1, my(d=digits(3^n+k,3)[2..-1]);
  my(f='x^n+sum(i=1,n,(d[i]-1)*'x^(i-1))); if(polcoef(f,0)==0||!polisirreducible(f),next);
  if(abs(nfdisc(f))>X,next); listput(S,polredabs(f)));
  S=vecsort(Set(S),(a,b)->sign(abs(nfdisc(a))-abs(nfdisc(b)))); foreach(S,f, if(c++>mx,break); field(f,0,"brute"))};
