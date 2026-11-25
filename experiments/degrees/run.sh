# PARI data: XDG_RUNTIME_DIR=... systemd-run --user --scope -p MemoryMax=400M nice -n 19 bash run.sh
gp -q -s 60M gen.gp <<< 'cubics()' > cubic.txt
gp -q -s 60M gen.gp <<< 'foreach(["C4","V4","D4","A4","S4"],G,others(4,G,2209))' > quartic.txt
gp -q -s 60M gen.gp <<< 'brute(5,30000,40)' > quintic.txt
gp -q -s 60M gen.gp <<< 'brute(6,100000,40)' > sextic.txt
gp -q -s 60M gen.gp <<< 'brute(7,1000000,6)' > septic.txt
