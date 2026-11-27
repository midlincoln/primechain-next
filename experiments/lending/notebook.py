# A bank's book of promissory notes (exploratory, 2026-11-27).
# Time is blocks; one period = 10,000 blocks. Rates are quoted per 100 periods ("a year" only
# for display, assuming 1,000,000 blocks a year). Amounts in micro-units.
#
# The bank: 600M own units, 600M term deposits at 3% due at period 100 (half then leave),
# 400M demand deposits held 100% at the Fed (pass-through, left out). Idle cash is primes and
# earns the holder yield. It buys notes every period while it has cash above a buffer:
#   discount notes  (consumer, unsecured, 10-25 periods, priced at Fed rate + 3.5%)
#   coupon notes    (secured by a ring lien, 100-300 periods, fixed coupon = Fed rate + 1%,
#                    paid every 25 periods, principal at the end)
#   floaters        (half secured, 100-200 periods, coupon = Fed rate + 2%, reset each payment)
# Defaults: a note that defaults is overdue; after 10 periods the collateral or collection
# recovers a share of what was owed. Secured notes are eligible at the Fed's discount window
# (pledged with NOTE_LIEN, 30% haircut). The Fed raises its rate from 5% to 7% at period 50.
import random, statistics as st

Y = 0.035                          # holder yield, per 100 periods
HAIRCUT, BUF = 0.30, 60e6
TYPES = {  # share of new notes, per-period default chance, recovery, eligible-if-secured share
    'discount': dict(w=0.40, h=0.0005, rec=0.20, sec=0.0),
    'coupon':   dict(w=0.35, h=0.0002, rec=0.70, sec=1.0),
    'floater':  dict(w=0.25, h=0.0003, rec=0.50, sec=0.5),
}

def fed_rate(t, hike):
    return 0.07 if hike and t >= 50 else 0.05

class Note:
    def __init__(s, kind, t, rnd, fed):
        s.kind, s.t0 = kind, t
        s.P = rnd.uniform(1e6, 5e6)
        s.secured = rnd.random() < TYPES[kind]['sec']
        s.state = 'current'; s.default_t = None; s.pledged = False
        if kind == 'discount':
            s.term = rnd.randint(10, 25); s.r = fed + 0.035
            s.face = s.P * (1 + s.r * s.term / 100); s.price = s.P
        elif kind == 'coupon':
            s.term = rnd.randint(4, 12) * 25; s.r = fed + 0.01; s.price = s.P
        else:
            s.term = rnd.randint(4, 8) * 25; s.spread = 0.02; s.price = s.P
        s.end = t + s.term
    def owed(s):                     # what is still owed (principal, or face for discount notes)
        return s.face if s.kind == 'discount' else s.P
    def book(s, t):                  # carrying value: amortized cost
        if s.kind == 'discount':
            return min(s.face, s.P * (1 + s.r * (t - s.t0) / 100))
        return s.P
    def market(s, t, fed):           # present value of remaining payments at today's rates
        if s.kind == 'floater':
            return s.P
        if s.kind == 'discount':
            m = fed + 0.035; return s.face / (1 + m * max(s.end - t, 0) / 100)
        m = fed + 0.01; pv = 0.0
        for k in range(t + 1, s.end + 1):
            if (k - s.t0) % 25 == 0:
                cf = s.P * s.r * 25 / 100 + (s.P if k == s.end else 0)
                pv += cf / (1 + m * (k - t) / 100)
        return pv

def run(seed, hike=True, liquidity='pledge', T=150):
    rnd = random.Random(seed)
    cash, E0 = 1200e6, 600e6
    term, term_rate = 600e6, 0.03
    fed_loan = 0.0
    notes, log = [], {}
    inc = dict(discount=0.0, coupon=0.0, floater=0.0, cash=0.0, losses=0.0, deposits=0.0, fed=0.0,
               sale_loss=0.0)
    for t in range(T):
        fed = fed_rate(t, hike)
        # income on idle cash, cost of term deposits and of the Fed loan (accrued per period)
        c = cash * Y / 100; cash += c; inc['cash'] += c
        d = term * term_rate / 100; inc['deposits'] += d; cash -= d
        f = fed_loan * fed / 100; inc['fed'] += f; cash -= f
        # payments and defaults
        for n in notes:
            if n.state == 'current' and rnd.random() < TYPES[n.kind]['h'] * (1.5 if n.kind == 'floater' and fed > 0.05 else 1):
                n.state, n.default_t = 'overdue', t
            if n.state == 'overdue' and t - n.default_t == 10:
                got = TYPES[n.kind]['rec'] * n.owed(); loss = n.book(t) - got
                cash += got; inc['losses'] += loss; n.state = 'closed'
                continue
            if n.state != 'current':
                continue
            if n.kind == 'discount' and t == n.end:
                cash += n.face; inc['discount'] += n.face - n.P; n.state = 'closed'
            elif n.kind in ('coupon', 'floater') and (t - n.t0) % 25 == 0 and t > n.t0:
                rate = n.r if n.kind == 'coupon' else fed + n.spread
                cpn = n.P * rate * 25 / 100; cash += cpn; inc[n.kind] += cpn
                if t >= n.end:
                    cash += n.P; n.state = 'closed'
        notes = [n for n in notes if n.state != 'closed']
        # term deposits fall due at period 100: interest was paid as it accrued; half leave
        if t == 100:
            need = term / 2; term -= need; term_rate = fed - 0.02; cash -= need
            if cash < BUF:
                short = BUF - cash
                if liquidity == 'pledge':
                    room = sum((1 - HAIRCUT) * n.book(t) for n in notes if n.secured and not n.pledged)
                    take = min(short, room); fed_loan += take; cash += take
                    left = take
                    for n in notes:
                        if left <= 0: break
                        if n.secured and not n.pledged:
                            n.pledged = True; left -= (1 - HAIRCUT) * n.book(t)
                else:                                # sell coupon notes at market value
                    for n in sorted(notes, key=lambda n: n.kind != 'coupon'):
                        if cash >= BUF: break
                        if n.kind == 'discount': continue
                        mv = n.market(t, fed); inc['sale_loss'] += n.book(t) - mv
                        cash += mv; n.state = 'closed'
                    notes = [n for n in notes if n.state != 'closed']
        # repay the Fed when cash allows
        if fed_loan > 0 and cash > BUF * 2:
            r = min(fed_loan, cash - BUF * 2); fed_loan -= r; cash -= r
            if fed_loan == 0:
                for n in notes: n.pledged = False
        # buy new notes (issue-and-pay with the borrower), a few per period
        for _ in range(12):
            if cash < BUF + 5e6 or t >= 100 and fed_loan > 0: break
            kind = rnd.choices(list(TYPES), [v['w'] for v in TYPES.values()])[0]
            n = Note(kind, t, rnd, fed); cash -= n.price; notes.append(n)
        if t in (49, 99, 120, T - 1):
            log[t] = snapshot(notes, t, fed, cash, fed_loan, term, E0)
    return inc, log

def snapshot(notes, t, fed, cash, fed_loan, term, E0):
    by = {k: [n for n in notes if n.kind == k] for k in TYPES}
    book = sum(n.book(t) for n in notes)
    mkt = sum(n.market(t, fed) if n.state == 'current' else n.book(t) for n in notes)
    return dict(
        face={k: sum(n.owed() for n in v) / 1e6 for k, v in by.items()},
        count={k: len(v) for k, v in by.items()},
        overdue=sum(n.owed() for n in notes if n.state == 'overdue') / 1e6,
        pledged=sum(n.owed() for n in notes if n.pledged) / 1e6,
        fed_loan=fed_loan / 1e6, cash=cash / 1e6,
        equity_book=(cash + book - term - fed_loan) / 1e6,
        equity_mkt=(cash + mkt - term - fed_loan) / 1e6,
        coupon_book=sum(n.book(t) for n in by['coupon']) / 1e6,
        coupon_mkt=sum(n.market(t, fed) for n in by['coupon'] if n.state == 'current') / 1e6)

def avg(rows, key, sub=None):
    vals = [r[key] if sub is None else r[key][sub] for r in rows]
    return st.mean(vals)

if __name__ == '__main__':
    S = range(40)
    res = {(h, l): [run(s, h, l) for s in S] for h in (False, True) for l in ('pledge', 'sell')}
    base = res[(True, 'pledge')]
    print("A bank's book of notes, 40 runs averaged. One period = 10,000 blocks; 100 periods shown as a year.\n")
    print("1. The book (Fed rate raised from 5% to 7% at period 50):")
    print(f"   {'period':>7}{'discount':>14}{'coupon':>14}{'floater':>14}{'overdue':>9}{'cash':>7}   (face in M, number of notes)")
    for t in (49, 99, 149):
        L = [r[1][t] for r in base]
        cells = ''.join(f"{avg(L,'face',k):>8.0f} ({avg(L,'count',k):>3.0f})" for k in TYPES)
        print(f"   {t+1:>7}{cells}{avg(L,'overdue'):>9.1f}{avg(L,'cash'):>7.0f}")
    print("\n2. Income over periods 1-150 (M micro-units), without and with the rate rise:")
    print(f"   {'':<28}{'no rise':>9}{'rise':>9}")
    for k, lab in (('discount', 'discount notes (discounts)'), ('coupon', 'coupon notes (coupons)'),
                   ('floater', 'floaters (coupons)'), ('cash', 'holder yield on idle cash'),
                   ('losses', 'losses on defaults'), ('deposits', 'interest to term depositors'),
                   ('fed', 'interest to the Fed')):
        a = st.mean(r[0][k] for r in res[(False, 'pledge')]) / 1e6
        b = st.mean(r[0][k] for r in res[(True, 'pledge')]) / 1e6
        sign = -1 if k in ('losses', 'deposits', 'fed') else 1
        print(f"   {lab:<28}{sign*a:>9.1f}{sign*b:>9.1f}")
    roe = [100 * (avg([r[1][149] for r in res[(h, 'pledge')]], 'equity_book') - 600) / 600 for h in (False, True)]
    print(f"   {'gain on 600M capital':<28}{roe[0]:>8.1f}%{roe[1]:>8.1f}%   (over 150 periods, about 1.5 years)")
    L = [r[1][99] for r in base]
    print(f"\n3. The rise and fixed coupons: at period 100 the coupon notes are carried at {avg(L,'coupon_book'):.0f}M")
    print(f"   but worth {avg(L,'coupon_mkt'):.0f}M at the new rates (an unrealized loss); floaters stay at par and pay more.")
    print("\n4. Period 100: 300M of term deposits leave. Two ways to find the money:")
    for l, lab in (('pledge', 'pledge secured notes to the Fed'), ('sell', 'sell coupon notes to other banks')):
        R = res[(True, l)]; L = [r[1][149] for r in R]; L120 = [r[1][120] for r in R]
        sl = st.mean(r[0]['sale_loss'] for r in R) / 1e6
        print(f"   {lab:<34} Fed loan at 121: {avg(L120,'fed_loan'):>5.0f}M, pledged {avg(L120,'pledged'):>4.0f}M;"
              f" loss on sales {sl:>4.1f}M; capital at 150: {avg(L,'equity_book'):.0f}M")
    L = [r[1][120] for r in base]
    print("\n5. The Fed's view of this bank at period 121 (computed from the chain):")
    print(f"   notes held: discount {avg(L,'face','discount'):.0f}M, coupon {avg(L,'face','coupon'):.0f}M,"
          f" floaters {avg(L,'face','floater'):.0f}M; overdue {avg(L,'overdue'):.1f}M;"
          f" pledged to the Fed {avg(L,'pledged'):.0f}M; owed to the Fed {avg(L,'fed_loan'):.0f}M")
