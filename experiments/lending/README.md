# Lending models (exploratory, 2026-11-27)

Python, no engine code; all inputs are assumptions (default rates, recovery, deposit and
Fed rates, a price of 0.1 cent per micro-unit). Holder yield 3.5% (rule A4, 10 rings a year).

- `offchain_loan.py`: a bank lends micro-units to a person with nothing on chain. Break-even
  rates, a bank's return over 1,000 loans, and the borrower's cost in dollars when the
  micro-unit's price moves.
- `deposits.py`: a bank with demand and term deposits, reserves and the Fed's discount window;
  bank runs at reserve ratios 100%, 50%, 20% and 0%.
- `notebook.py`: a bank's book of promissory notes (discount notes, coupon notes, floaters),
  defaults and recoveries, a Fed rate rise from 5% to 7%, half the term deposits leaving at
  period 100 (pledging secured notes to the Fed against selling coupon notes), and the Fed's
  computed view of the bank. One period = 10,000 blocks. About 30 s for 40 runs per case.
