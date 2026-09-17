# ICMP echo response as a dmsystem service

## Why this exists

dmicmp needs no setup from other modules to do its core job - it registers
itself with [dmip](https://github.com/choco-technologies/dmip) automatically
in `dmod_init()` (see `src/dmicmp.c`), and from that point on answers ICMP
Echo Requests (ping) with no further calls needed. The catch is exactly that:
`dmod_init()` only runs once something *loads* dmicmp - nothing does that on
its own at boot. `configs/` gives
[dmsystem](https://github.com/choco-technologies/dmsystem)'s `libsystemd` a
way to do it automatically.

## Why a `.dme` script instead of `exec=dmicmp` directly

dmicmp is a **Library**-type DMOD module (see `CMakeLists.txt`), not an
Application - it has no `main()`, so nothing can spawn it as a process the
way `libsystemd` starts a unit (`Dmod_RunModuleDetached`, see dmsystem's
[`app/libsystemd/docs/configuration.md`](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md)).
What actually needs to happen is "load this module into the running DMOD
system", which is a different operation - the one
[`dmell`](https://github.com/choco-technologies/dmell) exposes as its own
`module load`/`module unload` built-in. `configs/icmp-start.dme` and
`configs/icmp-stop.dme` are two-line wrappers over exactly that, so a
`libsystemd` unit (whose `exec` must be something it can spawn) can drive
them the same way it drives any other unit.

## Files

| File | Does |
|------|------|
| [`configs/icmp-start.dme`](../configs/icmp-start.dme) | `module load dmicmp` - starts answering pings. |
| [`configs/icmp-stop.dme`](../configs/icmp-stop.dme) | `module unload dmicmp` - stops answering pings. |
| [`configs/icmp.ini`](../configs/icmp.ini) | The dmsystem unit itself: `exec=dmell`, `args=<path to icmp-start.dme>`, `type=oneshot`. |

## Why `type=oneshot`, and why there is no "stop"

`icmp-start.dme` only needs to run once: unlike a `type=simple` unit (a
server expected to keep running until explicitly stopped), dmicmp stays
loaded - and answering pings - entirely on its own once `dmod_init()` has
run, independent of the script or process that triggered it. `type=oneshot`
tells `libsystemd` this exit is expected, not a crash (see dmsystem's
configuration docs, "Service type").

This does mean `service stop icmp` has nothing to act on: `libsystemd` stops
a unit by killing its tracked process
(`libsystemd_stop_service_internal()`), and a oneshot unit's process is
already gone by the time it would run. `libsystemd` also has no `ExecStop`-
style key yet - a unit file has no way to name a separate command to run on
stop. So turning ping response back off is a deliberate separate step, not
something `service stop icmp` does: run `icmp-stop.dme` directly.

```bash
dmell /opt/dmicmp/configs/icmp-stop.dme
```

Whether dmicmp is currently answering pings or not is therefore controlled
entirely by which of the two scripts was last run - `module load`/`module
unload` are idempotent-safe either way (loading an already-loaded module or
unloading an already-unloaded one just fails harmlessly and is logged, it
does not crash the shell).

## Enabling at boot

Install dmicmp with its `.dmr` (see the root [README.md](../README.md)), then
drop `icmp.ini` into `libsystemd`'s units directory and point its `args` at
wherever `icmp-start.dme` was installed:

```bash
cp /opt/dmicmp/configs/icmp.ini /etc/dmsystem/units/icmp.ini
# args= in icmp.ini must match the actual install path, e.g.:
#   args=/opt/dmicmp/configs/icmp-start.dme
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

See also dmsystem's [configuration.md](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#dependency-ordering)
if you want dmicmp brought up only `after`/once a particular network
interface unit (`after=`/`requires=` in `icmp.ini`) is already running.
