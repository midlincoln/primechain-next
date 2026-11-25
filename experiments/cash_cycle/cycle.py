#!/usr/bin/env python3
"""Full cash cycle on the three-ring tower. Usage: cycle.py rsa|419 [cash limit]"""
import sys
import cash
scheme = sys.argv[1]; cap = int(sys.argv[2]) if len(sys.argv) > 2 else None
sim, C, st, lost, seig, wal = cash.run(1, scheme, cap)
print(scheme, 'limit', cap, dict(st))
print('printed', C.printed, 'deposited', C.deposited, 'retired', C.treasury, 'outstanding', C.outstanding(), 'backing', C.backing(),
      'balanced', C.printed - C.deposited - C.treasury == C.outstanding() == C.backing())
print('lost', lost, 'seigniorage', seig, 'refused', dict(C.rej))
print('anonymity set min', min(C.anon), 'median', cash.statistics.median(C.anon), 'notes spent', len(C.anon))
print('money conserved', sum(sum(v.values()) for v in sim.bal.values()) - 2 * 10**6 + sim.dust == sim.issued)
