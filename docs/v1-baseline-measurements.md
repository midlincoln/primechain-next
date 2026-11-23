# v1 baseline measurements

Status: measurement of the live v1 testnet, 2026-11-22. Read-only. No server,
service or configuration was changed.

This is the "establish the baseline" step of the PrimeChain Next plan: where
the time and resources of the running chain actually go, before Next's
protocol is designed to be faster.

## Setup

- Network: launch-testnet-3, three validators, 2-of-3 quorum.
- Hosts: each validator runs on 1 vCPU and 2 GB RAM (DigitalOcean, nyc1).
  - val1: 192.81.209.230
  - val2: 137.184.129.231
  - val4: 134.209.215.17
- Validator source: commit `f7138dd`.
- The composite lottery is disabled by protocol rule from integer 29,306
  (`kCompositeLotteryDisabledFromInteger`, commit `d46f8f9`). The service still
  starts with `--composite-lottery-*` flags, but they have no effect at
  today's frontier.

## Method

1. **Record timing.** Every 10 seconds for 2.3 hours, the public `status` of
   all three validators was polled (816 rounds, 16:33–18:51 UTC). The script:

   ```sh
   while true; do
     t=$(date -u +%s)
     for ip in 192.81.209.230 137.184.129.231 134.209.215.17; do
       echo "$t $ip $(primechain-client status $ip 8339)"
     done
     sleep 10
   done
   ```

2. **Memory and CPU.** Every 10 seconds for 2.1 hours on val1, the
   validator's `VmRSS`, `VmSwap`, `VmSize` and CPU were recorded, together
   with the timestamp and size of `chain.dat.snapshot`.
3. **Query latency.** Single read-only queries were sent to each validator
   over TCP, and the time to the first reply line was measured.
4. **Logs.** The last 24 hours of validator logs were summarized on all three
   hosts (message types and counts), and the last 30 days of kernel logs were
   checked for out-of-memory kills.

## Results

### Speed

| Metric | Value |
|---|---|
| Integers finalized | 83 (37,459 → 37,542) |
| Rate | **36.2 integers/hour, about 99.6 s per integer** |
| Interval between new records | median 101 s, p90 111 s, max 122 s |
| Primes | 10 (4.4 per hour) |
| Integers per finalization | always 1 |

The pace is remarkably regular, with no long stalls. Only 2 "round stalled"
events appear in val1's logs over 24 hours. The September blog post measured
about 124 s per record.

### Consensus health

The validators were never more than 1 integer apart, and were apart in only
0.5–2.1% of polls. There were no failed responses. Consensus is correct; it
is just slow.

### Query latency: the state reload

| Query | Rebuilds chain state | val1 | val2 | val4 |
|---|---|---|---|---|
| `GET_VERSION` | no | 79 ms | 27 ms | 45 ms |
| `GET_COMMIT_PHASE` | no | 60 ms | 23 ms | 51 ms |
| `GET_MINING_VIEW` | **yes** | **2.4–2.5 s** | **1.7–2.2 s** | **1.3–1.4 s** |

Queries that don't rebuild state cost about the network round trip. Any
request that constructs a fresh `SequentialNode` and calls `load()` costs
**1.3–2.5 s of CPU** on these hosts. `sync_server.cpp` does this in **39
places**, covering votes, round changes, candidate validation, mining views
and more. Finalizing one integer involves dozens of such requests between the
miner and validators and among validators, all queued on one CPU. That is the
most likely explanation for the steady ~100 s per integer, and for the
validators' constant ~83% CPU on a chain with almost no arithmetic to do.

*Measurement note:* a first attempt waited for a fixed 120 bytes, which made
`GET_COMMIT_PHASE` look like it took 5 s. That was the server's idle timeout.
Reading to the end of the first line gives the numbers above.

### Memory

- Over the 2.1 hours on val1: RSS median 180 MB, max **1.1 GB**; RSS + swap
  max 1.27 GB; `VmSize` 0.66–1.6 GB.
- There were **120 jumps of 400 MB or more in `VmSize`** (typically about
  600–800 MB), roughly one per minute. That is heavy allocation churn.
- `chain.dat.snapshot` (about 1.9 MB) is rewritten about every 10 seconds.
  It was never missing, so the full `loadAll()` fallback (reading the whole
  ~900 MB `chain.dat`) did not occur in this window.
- **Out-of-memory kills:** val1 on 2026-11-21 14:22 UTC and val2 on
  2026-11-10 11:24 UTC (process `primechain-sync`). Both were restarted
  automatically by systemd within seconds. val4 had none in 30 days. These
  kills are most likely moments when several state rebuilds overlap.

### Wasted messages (last 24 h, from the logs)

| Message | val1 | val2 | val4 |
|---|---|---|---|
| vote request refused: "not assigned to this finalization committee / proposer" | 136 | 292 | 0 |
| "record propagation: peer did not return record response" | 94 | 96 | 8 |
| "mempool sync skipped: peer chain sync advanced" | 46 | 33 | 774 |
| connect timeouts / could not connect | 16 | 0 | 26 |

The refused vote requests are gap G3 in `round-change-audit.md`: only an
assigned committee of 2 may sign each round, but requests go to all three
validators.

## Conclusions for PrimeChain Next

1. **The dominant cost is an implementation choice, not the protocol:**
   rebuilding the full node state for each request. Next must keep **one
   authoritative in-memory state** with incremental updates, and never reload
   it per request. On its own, that could plausibly take the per-integer time
   from ~100 s to a few seconds.
2. **After that, network round trips dominate** (about 25–80 ms between these
   hosts). One two-phase decision per **segment** of K integers costs a few
   round trips, so thousands of integers per hour is a realistic target on
   the same hardware, depending on K.
3. **Accepting any quorum of signers** (two-phase model, recommendation 4 in
   the audit) removes the ~430 refused vote requests per day.
4. **Less memory churn means the same 1-CPU, 2 GB hosts stay up,** with no
   out-of-memory kills. That matters, because no hardware upgrade is planned.
5. **The lottery stays out:** it is already disabled in v1, and segment caps
   plus multi-miner decisions cover its fairness purpose without an extra
   network round.

## Not measured

- A breakdown of each integer's ~100 s into protocol phases (commit, reveal,
  votes) would need instrumented builds on a local copy of the network. It
  was not done, to avoid touching the live chain.
- Miner-side timing: the miners are external.
- The 2-hour window is a sample. Behaviour under heavy contention (several
  miners racing) may differ.
