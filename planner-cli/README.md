# planner-cli - headless Subsurface dive planner

`planner-cli` runs the Subsurface dive planner **without the GUI**.  It
reads a dive plan (depth changes, gas mixtures, deco settings, ...) from a
plain text file, runs the exact same core code as the Subsurface GUI
planner (`core/planner.cpp`, `core/deco.cpp`, `core/profile.cpp`) and
outputs the resulting dive profile: deco schedule, ceilings, tissue
saturations, gas consumption, per-sample data.

Its purpose is to serve as a **reference implementation** against which
the decompression algorithm of a dive computer can be validated: feed it
the same plan your firmware would compute and compare ceilings, stops,
SACs and tissue loadings.

## Running with Nix

The repository root contains a `flake.nix` that builds `planner-cli`
(including Subsurface's libdivecomputer fork, pinned to the submodule commit):

```sh
# run directly from GitHub
nix run github:csicar/subsurface-cli -- my-dive.plan
nix run github:csicar/subsurface-cli -- --json my-dive.plan > profile.json

# or from a checkout
nix build .            # -> result/bin/planner-cli
nix run . -- result/share/planner-cli/examples/dive-79m-30min-tx.plan
nix develop            # shell with all build dependencies
```

The example plans and this README are installed to `share/planner-cli/`.

## Building

```
cmake -DSUBSURFACE_TARGET_EXECUTABLE=DownloaderExecutable \
      -DBUILD_PLANNER_CLI=ON \
      -DLIBDC_FROM_PKGCONFIG=ON \
      <path-to>/subsurface
make planner-cli
```

The result is `planner-cli` in the build directory (installed as
`bin/planner-cli` via `make install`).

If libdivecomputer was built in-tree (e.g. via `scripts/build.sh` it lands
in `install-root/lib`), point the loader at it when running the binary:

```
LD_LIBRARY_PATH=<...>/install-root/lib ./planner-cli/planner-cli examples/dive-30m-20min-air.plan
```


## Plan file format

One command per line; `#` or `;` start comments, blank lines are ignored.
All physical values are **metric** (depths in m, rates in m/min,
pressures in bar, SAC in l/min); `ft`, `psi` and `cuft` suffixes are
accepted on input.

### Settings

| command                  | meaning                                         | default |
|--------------------------|-------------------------------------------------|---------|
| `deco-mode <m>`          | `buehlmann`, `vpm` (VPM-B), `recreational`      | buehlmann |
| `gflow <n>`              | gradient factor low (%)                         | 30 |
| `gfhigh <n>`             | gradient factor high (%)                        | 70 |
| `conservatism <n>`       | VPM-B conservatism (0-4)                        | 0 |
| `bottom-sac <l/min>`     | gas consumption during bottom phase              | 20 |
| `deco-sac <l/min>`       | gas consumption during deco                     | 17 |
| `surface-pressure <mbar>`| ambient pressure; **<1000 mbar = altitude / mountain lake diving** | 1013 |
| `salinity <g/l>`         | 1000 = fresh, 10300 = sea water                 | 10300 |
| `descent-rate <m/min>`   |                                                 | 18 |
| `ascent-rate <m/min>`    |                                                 | 9 |
| `ascent-rate-stops <m/min>` | at deco stops                                | 9 |
| `ascent-rate-6m <m/min>` | final 6 m                                       | 9 |
| `last-stop <m>`          | 3 or 6                                          | 6 |
| `safety-stop on/off`     | recreational mode                               | off |
| `o2-breaks on/off`       | backgas breaks during long O2 deco               | off |
| `timestep <s>`          | deco stop granularity (like GUI "Deco time step")| 60 |
| `mode <m>`               | `oc`, `ccr`, `pscr`                              | oc |
| `setpoint <bar>`         | CCR setpoint                                    | 1.3 |
| `bailout on/off`         | for CCR: plan deco on OC bailout gas             | off |
| `simulate on/off`        | treat the plan as the dive as it was **dived**  | off |

By default (`simulate off`) only descent and bottom phase are given;
the planner computes the deco schedule and the way back to the surface.
With `simulate on` the given waypoints are the whole dive - the core
code then behaves like it does for a logged dive (used to check ceilings
"as dived" and end-of-dive tissues).

### Cylinders

```
cyl <mix> [<size>l] [<start>bar] [use=<use>] [change=<depth>]
```

- cylinders are referenced by their index (0 = first declared)
- the planner assumes the tank filled to `<start>` pressure
- `<mix>`: `air`, `ean32`/`nx32`, `o2`, `tx15/45` (also `tmx15/45`, `15/45`)
- `use=`: one of `oc` (default), `diluent`, `oxygen`; the planner only
  distinguishes these - dive tanks for deco are simply `oc`
- `change=<depth>`: depth at which this deco gas becomes available
  during the ascent.  Equivalent to the switch depth column in the GUI
  planner (usually the MOD at 1.6 bar pO2).
- `cyl-change <index> <depth>` is the same attribute spelled separately

### Dive

```
go <depth> [gas=<i>] [rate=<m/min>] [time=<dur>] [po2=<bar>] [mode=<m>]
hold <duration> [gas=<i>] [po2=<bar>] [mode=<m>]
```

- `go`: ascend/descend to `<depth>` on gas `<i>`.  Transit time is the
  distance divided by descent/ascent rate unless `time=` is given
  (useful to faithfully reproduce an as-dived profile).
- `hold`: stay at the current depth for `<duration>`.
- durations: `10min`, `600s`, `90`, `1min30s` (bare numbers are seconds)
- `po2=`/`mode=`: per-segment override for rebreather dives

Example:

```
# tx dive with EAN50 deco
deco-mode buehlmann
gflow 30
gfhigh 70
cyl tx18/45 24l 232bar
cyl ean50 12l 232bar change=21m
go 45m gas=0
hold 25min
```

## Output

- default: human readable summary (settings echo, runtime, deco
  schedule, gas consumption, CNS/OTU, final tissue loadings)
- `--csv`: one row per profile sample: time, depth, ceiling, NDL, TTS,
  GF, surface GF, CNS, inspired gas pressures (pO2/pN2/pHe), ambient
  pressure, density, SAC, cylinder pressure, 16 tissue saturation
  percentages and per-tissue ceilings
- `--json`: everything above as one JSON object
  (settings, dive summary, cylinders, deco stops, gas switch events,
  final tissue loadings, full sample array) - convenient for automated
  comparison against your own implementation
- `--output FILE`: write to file instead of stdout

A value of `-1` for `ndl_s` means "no decompression limit not reached"
(infinite NDL).

### Altitude / mountain lake diving

`surface-pressure` sets the ambient pressure the deco engine starts
from, e.g. `surface-pressure 850` (~1500 m elevation) or `850mbar`,
`0.85bar`, `12.4atm`, `12.4psi` (bare numbers are mbar). Depths remain
water depths (what a submerged pressure gauge reads); the core computes
absolute pressures as surface pressure + hydrostatic column. Note that
subsurface does **not** apply a pre-dive surface interval acclimatization
model: the diver is assumed equilibrated to the given ambient pressure.

## Validation

`examples/dive-79m-30min-tx.plan` reproduces the dive used in
`tests/testplan.cpp` (`TestPlan::testMetric()`).  The known reference
values for that dive are:

| value | testplan.cpp expects |
|-------|----------------------|
| minimum gas | 148 bar |
| gas change to EAN36 | at 33 m |
| gas change to O2 | at 6 m |
| run time | 109 min |
