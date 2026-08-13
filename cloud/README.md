# Cloud differential benchmark (Latitude.sh)

Spin up a quiet baremetal box, run clotho(base) vs clotho(head) over the perf
suite on consistent hardware, gate for regressions, pull the report, tear down.
Clotho is built **locally** as a static binary and shipped — the paid box does
no building, only measurement.

## Pipeline

```
LOCAL (free)                          BOX (paid, hourly)
build-refs.sh  --base --head          box-setup.sh   deps + quiet hardware (via cloud-init)
  -> out/clotho-{base,head} (static)  bench-remote.sh  perf.py run x2 + compare, core-pinned
run.sh orchestrates: build -> terraform apply -> arm watchdog -> ship -> bench -> pull -> destroy
```

One command:

```sh
export LATITUDESH_AUTH_TOKEN=...
cp terraform/terraform.tfvars.example terraform/terraform.tfvars   # fill project/plan/site/ssh
cloud/run.sh --base main --head my-feature --mode local            # keep warm while iterating
cloud/run.sh --base main --head my-feature --mode ci --cases picorv32 gate_rca   # unattended
```

Report lands in `cloud/reports/<utc-stamp>/` (base.json, head.json, report.txt,
env.txt, oracles.json). Exit code is the regression gate (0 clean, 1 regression).

## Two teardown modes

- `--mode ci`: provision → bench → `terraform destroy`. Watchdog idle-kill off;
  hard cap (`--max-min`, default 180) is the only in-box backstop.
- `--mode local`: provision → bench → **leave up**. The in-box watchdog deletes
  the server after `--idle-min` (default 60) with no SSH session or running
  bench — keeps it warm while you iterate, kills it when you walk away.

**Deletion, not power-off, stops hourly billing.** Cost backstops, in order:
`terraform destroy` on run.sh exit/error (primary) → in-box watchdog idle-kill
(local) → in-box hard cap (both, survives your laptop dying mid-session).

## Verify at signup (not yet confirmable without an account)

1. **Hourly minimum increment** — is a server deleted after 10 min billed 10 min
   or a full hour? Public docs don't say. If a 1-hour minimum exists, `--mode
   local` keep-warm is free upside; if per-minute, prefer `--mode ci`. Correctness
   is unaffected (both modes delete); only the keep-warm tradeoff changes.
2. **API auth + delete endpoint** — watchdog uses
   `DELETE https://api.latitude.sh/servers/<id>` with `Authorization: Bearer <token>`.
   Confirm the scheme (`--auth-header`) and path against current API docs.
3. **SSH user** — `run.sh --ssh-user` defaults to `root`; confirm the Ubuntu
   image's default login user.
4. `terraform validate` / `apply` — needs the provider (downloaded on `init`)
   and live creds; not runnable offline.

## Security

The watchdog needs a Latitude token on the box to self-delete. run.sh injects it
over SSH (`chmod 600`), never into terraform state or user_data. **Use a
dedicated, rotatable token** (`WATCHDOG_TOKEN=...`), not your main one.

## Status

Verified locally: build-refs (static, portable), bench-remote (pin + gate +
bundle), box-setup + cloud-init (YAML + round-trip), watchdog (keep/idle/cap
dry-run), all bash syntax. Unverified until account: the four items above.
