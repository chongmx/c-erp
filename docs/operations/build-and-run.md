# Build and run

C++20, CMake, PostgreSQL. Dependencies (drogon, libpqxx, nlohmann/json) are
vendored under `3rdparty/` — there is nothing to fetch at build time. The
frontend has **no build step**.

---

## First run on a new machine

```bash
./scripts/deps/install.sh          # toolchain, wkhtmltopdf, OWL, PostgreSQL
cmake -B ./build && cmake --build ./build
./scripts/server.sh --install --start
```

Each step is also a script of its own under `scripts/deps/`, so a machine that
already has PostgreSQL can skip it with `--no-db`.

## Build

```bash
cmake -B ./build            # configure — first time, or after CMakeLists.txt changes
cmake --build ./build       # build
rm -rf ./build              # full rebuild
```

`scripts/build.sh` wraps this and also builds the admin console:

```bash
./scripts/build.sh                 # configure + build both binaries
./scripts/build.sh --server        # ./build/c-erp only
./scripts/build.sh --admin         # ./build/erp-admin only
./scripts/build.sh --clean         # wipe ./build first
./scripts/build.sh --jobs 8        # override parallelism
```

Two binaries come out:

| | |
|---|---|
| `./build/c-erp` | the server |
| `./build/erp-admin` | the loopback-only IT operator console |

### Two things about the build worth knowing

**The source list is a glob evaluated at configure time.** `CMakeLists.txt`
picks up `*.cpp` from `main.cpp`, `core/`, `modules/` and `factories/`. A newly
added file needs `cmake -B ./build` re-run **once** before it compiles.

**`erp_tests` is not in the default target.** `cmake --build ./build` stays the
fast path; `tests/run.sh` builds the test binary explicitly.

## Run

```bash
./build/c-erp                            # config/system.cfg
./build/c-erp --config path/to.cfg       # somewhere else
./build/c-erp --provision                # provision + migrate every tenant, then exit
```

`--provision` (alias `--migrate`) runs the full boot path — so every tenant gets
`ensureSchema_()` and every pending migration — then exits without serving. It
is what deployment migrations and `tools/provision_tenant.sh` call.

Default listen address is `127.0.0.1:8069`; `GET /healthz` is the liveness
probe.

## As a service

```bash
./scripts/server.sh --install     # create /etc/systemd/system/c-erp.service
./scripts/server.sh --start       # --stop  --restart  --status
./scripts/server.sh --enable      # start at boot  (--disable)
./scripts/server.sh --logs -f     # follow
```

`--enable` / `--disable` only change boot behaviour; they do not start or stop a
running service. That is systemd's own semantics and is deliberate — you can
disable at boot without dropping users mid-session.

**Stray processes.** A `c-erp` started by hand — `./build/c-erp &`, or a
`setsid`/`nohup` that outlived its shell — is invisible to systemd, so
`systemctl stop` reports success while the port stays bound and the old binary
keeps serving. `--stop` kills strays too, and `--status` reports them loudly
rather than showing a tidy "inactive" beside a listening port.

Every state-changing action is verified afterwards rather than assumed:
`systemctl` returning 0 only means the request was accepted.

## The admin console

`./build/erp-admin` is a separate binary, is **not** a service, and nothing
starts it. It binds loopback only.

```bash
./scripts/build.sh --run-admin      # build it, start it, print the URL
./scripts/build.sh --admin-url      # re-print the URL of a running console
./scripts/build.sh --stop-admin
./scripts/build.sh --admin-port N   # default 8072
```

## Seed data

None of it is needed to run the test suite — `tests/run.sh` restores a clean
baseline and seeds its own fixtures. Seed when you want to *look* at the
application.

```bash
./scripts/seed.sh              # what exists, and what each dataset is
./scripts/seed.sh parts        # the electronics catalogue
./scripts/seed.sh rental       # a demo storage facility
./scripts/seed.sh website      # the CMS pages
./scripts/seed.sh all
```

Every dataset is idempotent and removable (`--clean` / `--clear`).

## Where things live

`scripts/` is **operational only** — build it, deploy it, run it, reset its
database, seed it. Nothing test-related belongs there; a test is a folder under
`tests/`. `scripts/README.md` is the authoritative index, including a table of
where each retired script moved to.

## Frontend

Edit `web/static/src/app.js` (or a file under `web/static/src/components/`) and
reload the browser. No npm, no bundler, no watch process. A new component file
must be added to `web/static/index.html` **before** any component that names it —
script order is load-bearing. See
[../architecture/frontend.md](../architecture/frontend.md).

## Related

- [configuration.md](configuration.md) — every setting in `config/system.cfg`
- [database.md](database.md) — snapshots, the baseline, backups
- [testing.md](testing.md) — the suite
- [deployment.md](deployment.md) — the production topology
