\\ Test vectors v2 for the engine's ring module (B3.2): as gen_quadratic_vectors.gp, plus
\\ - UNIT=c0:c1 for D > 0: a unit eps != +-1 (PARI's fundamental unit), on the basis 1, omega;
\\ - every generator reduced by eps to the minimum of the height c0^2 + c1^2 (R10, no padding).
\\ PARI only finds; the C++ tests check everything independently.
\\ Usage: gp -q gen_quadratic_vectors_v2.gp > quadratic_vectors_v2.txt
PMAX = 50;
omegapol(D) = if(D % 4 == 1 || D % 4 == -3, x^2 - x + (1 - D)/4, x^2 - D/4);
mk(D) = if(D > 0, sqrt(D)/2, 2/Pi*sqrt(-D));
eligibleAt(D) = my(p = 2); while(p^2 < abs(D), p = nextprime(p + 1)); p;
roots(f, q) = vecsort(apply(t -> lift(t), polrootsmod(f, q)));
co(nf, a) = my(v = lift(nfbasistoalg(nf, a))); [polcoef(v, 0), polcoef(v, 1)];
ht(nf, a) = my(c = co(nf, a)); c[1]^2 + c[2]^2;
unit(nf, bnf) = if(nf.r1 == 2, nfalgtobasis(nf, bnf.fu[1]), 0);
red(nf, g, u) = {
  if(u === 0, return(g));
  my(ui = nfeltpow(nf, u, -1), t);
  while(ht(nf, t = nfeltmul(nf, g, u)) < ht(nf, g), g = t);
  while(ht(nf, t = nfeltmul(nf, g, ui)) < ht(nf, g), g = t);
  g;
}
gen(bnf, nf, q, r, u) = {
  my(I = idealhnf(nf, q, x - r), g = bnfisprincipal(bnf, I, 3));
  if(g[1] != 0, return(0));
  co(nf, red(nf, nfalgtobasis(nf, g[2]), u));
}
fstr(fa) = my(s = ""); for(i = 1, #fa~, s = Str(s, if(i > 1, "*", ""), fa[i, 1], "^", fa[i, 2])); s;
emit(D) = {
  my(P = eligibleAt(D)); if(P > PMAX, return);
  my(f = omegapol(D), nf = nfinit(f), bnf = bnfinit(f, 1), gens = "", ok = 1, u);
  if(bnf.no > 1, print("NOCERT D=", D, " H=", bnf.no); return);
  u = unit(nf, bnf);
  forprime(q = 2, floor(mk(D)),
    my(rs = roots(f, q));
    if(#rs == 0, next);
    my(g = gen(bnf, nf, q, rs[1], u));
    if(g === 0, ok = 0; break);
    gens = Str(gens, if(gens == "", "", ","), q, ":", rs[1], ":", g[1], ":", g[2]));
  if(!ok, error("class number 1 but no generator ", D));
  my(us = if(u === 0, "", my(c = co(nf, u)); Str(" UNIT=", c[1], ":", c[2])));
  print("FIELD D=", D, " FACTORS=", fstr(factor(abs(D))), " GENS=", gens, us);
}
my(v = version()); print("# v2: quadratic fields eligible at a prime <= ", PMAX, "; units and reduced generators; PARI ", v[1], ".", v[2], ".", v[3]);
for(D = 5, PMAX^2, if(isfundamental(D), emit(D)));
for(n = 3, PMAX^2, if(isfundamental(-n), emit(-n)));
entry(nf, bnf, f, D, p, u) = {
  my(rs = roots(f, p));
  if(#rs == 0, print("ENTRY D=", D, " P=", p, " TYPE=inert"); return);
  my(g = gen(bnf, nf, p, rs[1], u), t = if(#rs == 1, "ramified", "split"));
  print("ENTRY D=", D, " P=", p, " TYPE=", t, " R=", rs[1], " GEN=", g[1], ":", g[2]);
}
foreach([8, 5, 17, 28, 376, -4, -3, -163], D, my(f = omegapol(D), nf = nfinit(f), bnf = bnfinit(f, 1), u = unit(nf, bnf)); forprime(p = 2, 2000, entry(nf, bnf, f, D, p, u)));
