# Ceremony model (2026-11-26, exploratory)

`ceremony.py`: a toy ceremony on small CSIDH-type primes (p = 4·f·l₁⋯lₙ − 1). Participants
in turn apply a secret walk (exponents −1..1 on the small ideals) to the last accepted
supersingular curve over F_p, and prove in zero knowledge that the new curve came from the
old one (a sigma protocol with rejection sampling, as in SeaSign; 16 hash-derived rounds).
A model chain accepts contributions only in order and with a valid proof. Small primes give
no security. Python, under a memory cap.

| p | contributions | proof | check one proof | refused | final curve found by search |
|---|---|---|---|---|---|
| 419 (9 bits, 3 ideals) | 4 in 3.8 s | 80 B | 0.07 s | a curve not reached from the last one; a stale contribution | after 7 curves |
| 180179 (18 bits, 5 ideals) | 4 in 24 s | 128 B | 0.44 s | the same | after 690 curves, 4.2 s |

Real sizes: CSIDH-512 or larger primes (74 small ideals), at least 128 rounds, and a class
group or rejection sampling tuned for size. Basso et al. (2022/1469) do the F_{p²} version
with their own proof; Mokrani–Jao (2023/984) the F_p version. Not measured here.
