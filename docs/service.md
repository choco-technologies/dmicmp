# ICMP echo response as a dmsystem service

## Why this exists

dmicmp needs no setup from other modules to do its core job - it registers
itself with [dmip](https://github.com/choco-technologies/dmip) automatically
in `dmod_init()` (see `src/dmicmp.c`), and from that point on answers ICMP
Echo Requests (ping) with no further calls needed. The catch is exactly that:
`dmod_init()` only runs once something *loads and enables* dmicmp - nothing
does that on its own at boot. `configs/icmp.ini` gives
[dmsystem](https://github.com/choco-technologies/dmsystem)'s `libsystemd` a
way to do it automatically.

## `exec=dmicmp`, `type=module`

dmicmp is a **Library**-type DMOD module (see `CMakeLists.txt`), not an
Application - it has no `main()`, so nothing can spawn it as a process the
way `libsystemd` starts a `simple`/`oneshot` unit (`Dmod_RunModuleDetached`).
What actually needs to happen is "load this module into the running DMOD
system, then enable it" - `dmod_init()` (which is what actually registers
dmicmp with dmip) only runs on *enable*, not on load alone.

`libsystemd` supports this natively via `type=module`: a unit with
`type=module` has its `exec` loaded and enabled (`Dmod_LoadModuleByName` +
`Dmod_EnableModule`) when started, and disabled and unloaded
(`Dmod_DisableModule` + `Dmod_UnloadModule`) when stopped - no separate
launcher script needed, unlike the process-spawning `simple`/`oneshot` types.
See dmsystem's
[configuration docs](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#typemodule-services-backed-by-a-library-module-not-a-process)
for the full mechanism.

## Files

| File | Does |
|------|------|
| [`configs/icmp.ini`](../configs/icmp.ini) | The dmsystem unit: `exec=dmicmp`, `type=module`. |

## `service start`/`service stop` both work

Unlike the old `type=oneshot` + `.dme` script setup, `type=module` gives
`libsystemd` a real notion of "running" for this unit - `service status icmp`
reports it as running exactly while dmicmp is enabled, and `service stop
icmp` actually turns ping response off again (disables and unloads the
module), no separate script to run by hand:

```bash
service start icmp
service status icmp
service stop icmp
```

## Enabling at boot

Install dmicmp with its `.dmr` (see the root [README.md](../README.md)), then
drop `icmp.ini` into `libsystemd`'s units directory:

```bash
cp /opt/dmicmp/configs/icmp.ini /etc/dmsystem/units/icmp.ini
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

See also dmsystem's [configuration.md](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#dependency-ordering)
if you want dmicmp brought up only `after`/once a particular network
interface unit (`after=`/`requires=` in `icmp.ini`) is already running.
