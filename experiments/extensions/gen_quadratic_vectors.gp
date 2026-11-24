\\ Generate test vectors for docs/extension-certificates.md (quadratic fields, v1 of the spec).
\\ PARI only *finds* generators; the C++ verifier checks them independently.
\\ Usage: gp -q gen_quadratic_vectors.gp > quadratic_vectors.txt
\\
\\ Conventions (spec section 2): omega = (1+sqrt(D))/2 if D = 1 mod 4, else sqrt(D/4);
\\ f = minimal polynomial of omega; O_K = Z[omega]; elements c0 + c1*omega.
\\ Ring-prime label above q: (D, q, r) with r a root of f mod q; generators are given for the
\\ prime labeled by the smaller root r. Eligibility: |D| <= F^2 (eligible at the first prime p
\\ with p^2 >= |D|).
PMAX = 50;                                   \\ fields eligible at a prime <= PMAX
omegapol(D) = if(D % 4 == 1 || D % 4 == -3, x^2 - x + (1 - D)/4, x^2 - D/4);
mk(D) = if(D > 0, sqrt(D)/2, 2/Pi*sqrt(-D));
eligibleAt(D) = my(p = 2); while(p^2 < abs(D), p = nextprime(p + 1)); p;
\\ roots of f mod q, sorted
roots(f, q) = vecsort(apply(t -> lift(t), polrootsmod(f, q)));
\\ generator (c0, c1) of the prime ideal (q, omega - r), or 0 if not principal
gen(bnf, nf, q, r) = {
  my(I = idealhnf(nf, q, x - r), g = bnfisprincipal(bnf, I, 1));
  if(g[1] != 0, return(0));
  my(v = lift(nfbasistoalg(nf, g[2])));
  [polcoef(v, 0), polcoef(v, 1)];
}
fstr(fa) = my(s = ""); for(i = 1, #fa~, s = Str(s, if(i > 1, "*", ""), fa[i, 1], "^", fa[i, 2])); s;
emit(D) = {
  my(P = eligibleAt(D)); if(P > PMAX, return);
  my(f = omegapol(D), nf = nfinit(f), bnf = bnfinit(f, 1), gens = "", ok = 1);
  forprime(q = 2, floor(mk(D)),
    my(rs = roots(f, q));
    if(#rs == 0, next);                      \\ inert: nothing to prove
    my(g = gen(bnf, nf, q, rs[1]));
    if(g === 0, ok = 0; break);
    gens = Str(gens, if(gens == "", "", ","), q, ":", rs[1], ":", g[1], ":", g[2]));
  if(ok, print("FIELD D=", D, " FACTORS=", fstr(factor(abs(D))), " GENS=", gens),
         print("NOCERT D=", D, " H=", bnf.no));
}
my(v = version()); print("# quadratic fields eligible at a prime <= ", PMAX, " (|D| <= p^2); PARI ", v[1], ".", v[2], ".", v[3]);
for(D = 5, PMAX^2, if(isfundamental(D), emit(D)));
for(n = 3, PMAX^2, if(isfundamental(-n), emit(-n)));
\\ Field-chain entries (spec section 4) for a few rings, p < 2000. Inert primes are listed only so
\\ the verifier can check its skip rule; they are never recorded on chain (R9).
entry(nf, bnf, f, D, p) = {
  my(rs = roots(f, p));
  if(#rs == 0, print("ENTRY D=", D, " P=", p, " TYPE=inert"); return);
  my(g = gen(bnf, nf, p, rs[1]), t = if(#rs == 1, "ramified", "split"));
  print("ENTRY D=", D, " P=", p, " TYPE=", t, " R=", rs[1], " GEN=", g[1], ":", g[2]);
}
foreach([8, 5, 17, 28, 376, -4, -3, -163], D, my(f = omegapol(D), nf = nfinit(f), bnf = bnfinit(f, 1)); forprime(p = 2, 2000, entry(nf, bnf, f, D, p)));
