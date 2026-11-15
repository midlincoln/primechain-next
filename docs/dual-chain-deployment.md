# Running both chains on the existing validator hosts

Status: deployment plan, not an installation or authorization to change the live
chain. Read-only host inspection performed on 2026-11-15. No services, firewall
rules, wallets, binaries, or SSH trust entries were changed.

## Observed hosts

| Validator | Host | Old-chain TCP listeners | Proposed Next TCP ports |
| --- | --- | --- | --- |
| 1 | 192.81.209.230 | 8339, 8340, 8341 | 9339, 9340, 9341: no listeners |
| 2 | 137.184.129.231 | 8339, 8340, 8341 | 9339, 9340, 9341: no listeners |
| 4 | 134.209.215.17 | 8339, 8340, 8341 | 9339, 9340, 9341: no listeners |

`ss -lntup` and selected `systemctl show` properties were inspected over SSH.
All three hosts reported `primechain-launch-node.service` active. This confirms
service activity and listeners, not chain agreement or consensus health.
Validator 2 also has nginx listeners on 80 and 8080. All three have SSH on 22
and local DNS listeners on 53.

The previously retired validator 3 at 67.205.172.245 presented a changed SSH host
key. Inspection stopped at host verification. Do not bypass the warning or reuse
this address until ownership and its host fingerprint are verified independently
through the provider console. Its ports and current role are unverified.

## Port plan

| Role | Old chain, unchanged | Next, proposed |
| --- | --- | --- |
| Public status/mining | TCP 8339 | TCP 9339 |
| Validator peers | TCP 8340 | TCP 9340 |
| Record/block synchronization | TCP 8341 | TCP 9341 |

The Next ports have no listeners on the inspected hosts at the observation time.
They have not been bound, reserved, externally tested, or opened in firewalls.
The prototype currently uses ephemeral localhost test ports, not these ports.
Recheck listeners, configured services/socket units, and provider firewall rules
immediately before launch. Fail startup on an occupied requested port; never
terminate its owner or silently fall back to old-chain ports.

## Firewall observations

- Validators 1 and 2: UFW active, default incoming deny. No allow rules for
  9339-9341. Their old-chain peer port 8340 currently allows source
  134.209.215.17; these existing rules were not changed or audited for correctness.
- Validator 4: UFW inactive and `nft list ruleset` returned an empty ruleset.
  Legacy iptables and provider-level filtering were not checked; this is not a
  claim of unrestricted public reachability.
- Provider/cloud firewalls were not inspected on any host.

Before deployment, explicitly review host and provider firewall rules. Initially
restrict all Next ports to the intended test hosts/operator access. Once public
access is approved, expose only the public and sync endpoints as required;
restrict 9340 to the new chain's validator peers. Do not blindly clone or replace
the live chain's firewall configuration. Handle IPv4/IPv6 bindings deliberately.

## Separate deployment identity and storage

Observed old-chain paths, which must remain untouched:

- Unit: `primechain-launch-node.service` on all three hosts.
- Chain: `/home/primechain/primechain-launch-data/chain.dat`.
- Validator 1/2 binary: `/home/primechain/primechain/build/primechain-sync-server`.
- Validator 4 binary: `/home/primechain/build/primechain-sync-server`.
- Wallets: `/home/primechain/validator-1.wallet`, `validator-2.wallet`, and
  `validator-4.wallet` on their respective hosts.
- Service user: `primechain` on validators 1/2; `root` on validator 4. No old-chain
  service-user changes are part of this plan.

Proposed Next layout, not yet created or provisioned:

```text
service:       primechain-next-validator.service
service user:  primechain-next
release files: /opt/primechain-next/releases/<release>/
configuration: /etc/primechain-next/
chain state:   /var/lib/primechain-next/data/
new keys:      /var/lib/primechain-next/wallets/
runtime locks: /run/primechain-next/
logs:          journalctl -u primechain-next-validator.service
```

Use fresh Next signing identities and a separate genesis/network identity. Do
not copy the old wallets or share journals, snapshots, indexes, peer lists,
bootstrap endpoints, caches, or temporary lock paths. Explicitly domain-separate
signed protocol messages and reject old-chain data/peers; distinct ports alone
do not prevent cross-chain replay. Deployment and rollback must target only
Next's unit and release path, never overwrite the old binary or use broad
process-name kill/restart commands.

## Before launching two chains

1. Finish continuous block installation/recovery and isolated multi-process
   network tests. The current fixture is not a deployable validator network.
2. Check CPU load, available RAM, disk capacity/growth, and I/O contention under
   old-chain load. This port inspection does not establish resource capacity.
3. Set reviewed CPU, memory, task, and I/O limits for Next so experimental work
   cannot exhaust resources needed by the old chain. Start with small workloads.
4. Recheck port ownership, firewall reachability, separate identities/paths, and
   network/signature-domain separation. Agree on the actual launch validator set.
5. Deploy only the separate Next service; verify the old service, chain progress,
   and resource headroom before and after startup. Stop only Next if it causes
   contention. Keep a tested Next-only rollback procedure.

No deployment, firewall edit, key generation, or remote service modification was
performed as part of the inspection.
