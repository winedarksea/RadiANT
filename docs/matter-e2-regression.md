# E2: the regression gate, and the two things it caught that it was not looking for

**Date:** 2026-08-15
**Verdict: no arm's `.config` changed in CONTENT — every symbol that exists in a
baseline `.config` exists at the same value at HEAD, and no symbol appears that
was not there before, with one predicted exception on `bridge.conf`. The
predicted `NCS_SAMPLE_MATTER_*` leak did NOT appear; the `if CHIP` wrap works,
and works better than E1 predicted. But the diff is NOT empty, twice over:
every arm's `.config` has 28-51 symbols RELOCATED within the file, and
`bridge.conf`'s IMAGE grew by 2 564 B of flash and 1 472 B of RAM — a change a
`.config` diff structurally cannot see, on one of the five section 7.4
baselines.**
**Checked by:** twelve builds, six at the baseline commit and six at HEAD, all
`-p always`, all on `nrf54l15dk/nrf54l15/cpuapp`, reproducible from this
document. Nothing was flashed and no serial port was opened.

This is E2 of the Matter plan:

> Rebuild all five existing arms pristine and diff `zephyr/.config` against
> pre-change baselines. Anything else in the diff is a Kconfig-sourcing bug.

E1 is `docs/matter-e1-readback.md` and E3 is `docs/matter-e3-vendoring.md`;
both predicted a specific E2 result and both are checked against below.

## The arms, and why there are six of them

The plan says five. The repository says five in one place and six in another,
and the difference is worth stating rather than resolving silently.

- **`scripts/build_all.ps1` builds no `apps/dongle_thread` configuration at
  all.** Its two matrices are `apps/dongle` / `apps/dongle_ti` and
  `apps/hrm_ble`. The string `dongle_thread` does not occur in the file.
- **`scripts/build_p4.ps1` builds three**: `p4ctrl` (`gate.conf`), `p4med`
  (`thread.conf`) and `p4sed` (`thread.conf;thread_sed.conf`). It adds
  `-DCONFIG_RADIANT_SWEEP_DEBUG=y` on the command line, which is a property of
  that bench sitting rather than of the images (the script's own header says
  so), so it is deliberately absent below.
- **`.github/workflows/build.yml`'s `build-new-apps` job builds six.** The five
  "coexistence arms" that `apps/dongle_thread/Kconfig:800` names by name —
  `gate.conf`, `thread.conf`, `thread_sed.conf`, `coex.conf`, `bridge.conf` —
  plus a sixth `plain` row with no `EXTRA_CONF_FILE` at all and
  `-DRADIANT_BACKEND=null`, which `prj.conf`'s own header calls this
  application's other normal configuration.

All six were built. The `plain` row is not a coexistence baseline and is not
one of the five, but it is an existing arm in CI, it is the only arm on the
null backend, and excluding it from a regression gate on the grounds of a
naming convention would be exactly the kind of silent narrowing
`docs/radiant-silent-check-disarms.md` collects.

| Arm | Board | `-DANT_RADIO` | `-DRADIANT_BACKEND` | `EXTRA_CONF_FILE` | Where it comes from |
|---|---|---|---|---|---|
| `plain` | `nrf54l15dk/nrf54l15/cpuapp` | `core` | `null` | *(none)* | `build.yml` only |
| `gate` | `nrf54l15dk/nrf54l15/cpuapp` | `core` | `nrf` | `gate.conf` | `build.yml`, `build_p4.ps1` (`p4ctrl`) |
| `med` | `nrf54l15dk/nrf54l15/cpuapp` | `core` | `nrf` | `thread.conf` | `build.yml`, `build_p4.ps1` (`p4med`) |
| `sed` | `nrf54l15dk/nrf54l15/cpuapp` | `core` | `nrf` | `thread.conf;thread_sed.conf` | `build.yml`, `build_p4.ps1` (`p4sed`) |
| `coex` | `nrf54l15dk/nrf54l15/cpuapp` | `core` | `nrf` | `coex.conf` | `build.yml` only |
| `bridge` | `nrf54l15dk/nrf54l15/cpuapp` | `core` | `nrf` | `thread.conf;bridge.conf` | `build.yml` only |

No `-DSB_EXTRA_CONF_FILE` on any of them: `matter_sysbuild.conf` is passed by
name to the Matter arm alone, and `apps/dongle_thread/sysbuild.conf` still does
not exist — trap 2, still held.

## The baseline

**`244857e514d77dd5fa7e0a6028927132cb595cc1`** — "multichannel testing", the
current `master` tip. The entire Matter/A-C/E1/E3 body of work is *uncommitted*,
so the baseline is simply `HEAD` and there is no merge base to hunt for.

It was materialised with `git worktree add --detach` into a temp path, never by
stashing, resetting or checking out in place. The user's working tree was
verified unchanged before and after (51 modified/untracked entries, the same 51).

```powershell
git worktree add --detach C:\...\e2\base 244857e
```

**One artifact of that method must be normalised before diffing.** Kconfig
writes the module's own path into `.config` as a comment:

```
-# radiant (C:/Users/Colin/AppData/Local/Temp/e2/base/apps/dongle_thread/../../radiant)
+# radiant (C:/Users/Colin/ant_dongle/apps/dongle_thread/../../radiant)
```

That is the worktree's location, not a change. It is rewritten away below and
nowhere else; nothing else was filtered.

## Reproduce

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
$ErrorActionPreference = 'Continue'   # AFTER env.ps1, never before
Push-Location C:\ncs\v3.4.0
```

Then, per arm, twice — once with `-s <worktree>\apps\dongle_thread`, once with
`-s <repo>\apps\dongle_thread`:

```powershell
west -z C:\ncs\v3.4.0\zephyr build -s <tree>\apps\dongle_thread `
     -d <out>\<arm> -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=<null|nrf> `
     "-DEXTRA_CONF_FILE=<the row above>"   > $log 2>&1
```

`> $log 2>&1` and judging on `$LASTEXITCODE` are not optional. Windows
PowerShell 5.1 wraps a native command's stderr in ErrorRecords and west writes
ordinary progress there; with `Stop` in force the first progress line aborts the
run and reads exactly like a broken toolchain. `scripts/build_p4.ps1` says the
same at length.

The file compared is `<out>/<arm>/dongle_thread/zephyr/.config` — the image's
`.config` under the sysbuild layout, not the sysbuild one.

## Result 1 — the content diff is clean, and the predicted leak did not happen

| Arm | base lines | head lines | lines added | lines removed | net new content |
|---|---|---|---|---|---|
| `plain` | 2607 | 2608 | 29 | 28 | one blank line |
| `gate` | 2617 | 2618 | 29 | 28 | one blank line |
| `med` | 3544 | 3545 | 51 | 50 | one blank line |
| `sed` | 3545 | 3546 | 51 | 50 | one blank line |
| `coex` | 3135 | 3136 | 47 | 46 | one blank line |
| `bridge` | 3588 | 3590 | 52 | 50 | one blank line + `# CONFIG_ANT_DONGLE_MATTER_MAP is not set` |

Compared as sets of lines rather than as ordered files, five arms differ by
**one blank line and nothing else** and `bridge` by that blank line plus one
comment line. **No symbol changed value. No symbol was added or removed except
`ANT_DONGLE_MATTER_MAP` on `bridge`.**

**`CONFIG_NCS_SAMPLE_MATTER_LEDS` and `CONFIG_NCS_SAMPLE_MATTER_TEST_EVENT_TRIGGERS`
appear in NONE of the twelve `.config` files, at any value, in any form.**
Zero occurrences of the string `NCS_SAMPLE_MATTER` in all six head builds. The
plan predicted `=y` lines; E1 predicted `# ... is not set` comment lines; the
measured answer is **absent entirely**, which is the third and best outcome.
Kconfig omits a symbol from `.config` when its dependencies are unmet rather
than writing the commented form — the same rule `build.yml`'s `absent:`
assertion syntax was built around — and `if CHIP` with `CONFIG_CHIP` not set is
exactly that. The `osource` wrap in `apps/dongle_thread/Kconfig:809-824` works.

`# CONFIG_CHIP is not set` is present in all six head builds, which is what
makes the sentence above mean something rather than describe a build where CHIP
happened to be on.

`# CONFIG_ANT_DONGLE_MATTER_MAP is not set` on the `bridge` arm is predicted
verbatim by `apps/dongle_thread/Kconfig:741-743` and by E3's own read-back
(`docs/matter-e3-vendoring.md:147-153`): the symbol sits inside
`if RADIANT_BRIDGE`, which only `bridge.conf` sets, and defaults to `n`
everywhere except the Matter arm. The other four coexistence arms never see the
symbol. **Accepted, and it is a comment line rather than an image change.**

## Result 2 — the diff is not empty: 28 to 51 symbols moved position in every arm

This is the part the plan did not anticipate, and it is not confined to the
Matter arm.

```
@@ -37,6 +37,35 @@ CONFIG_RADIANT_ENROL_PHYSICAL=y
 # end of RadiANT profile policy

+# CONFIG_PM_OVERRIDE_EXTERNAL_DRIVER_CHECK is not set
+CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024
+# CONFIG_ASSERT is not set
+...
+# CONFIG_BT is not set
+# CONFIG_OPENTHREAD is not set
+CONFIG_PSA_CRYPTO_DRIVER_CRACEN=y
+CONFIG_LOG=y
+# CONFIG_SHELL is not set
```

…with each of those lines removed, unchanged, from wherever it used to sit
hundreds of lines further down.

| Arm | symbols relocated |
|---|---|
| `plain` | 28 |
| `gate` | 28 |
| `med` | 50 |
| `sed` | 50 |
| `coex` | 46 |
| `bridge` | 51 (the 50 above plus `ANT_DONGLE_MATTER_MAP`) |

**Root cause, and it is a real property of the change rather than generator
noise.** `apps/dongle_thread/Kconfig` now `osource`s three files —
connectedhomeip's `Kconfig.features` and `Kconfig.defaults` and NCS's
`samples/matter/common/src/Kconfig` — at lines 811-813, i.e. **before**
`source "Kconfig.zephyr"` at line 826. Kconfig's writer emits each symbol once,
at the FIRST node in the menu tree that defines it. Those three files are full
of `config X / default …` overrides for symbols Zephyr and NCS define elsewhere
— `BT` at `Kconfig.defaults:152`, `OPENTHREAD` at `:278`, `SHELL` at `:507`,
`MBEDTLS_HEAP_SIZE` at `:378`, `ZVFS_OPEN_MAX` at `:48`,
`SPI_NOR_FLASH_LAYOUT_PAGE_SIZE` at `Kconfig.features:31`, and so on. Every one
of those now has its first definition node inside this application's Kconfig
instead of inside `Kconfig.zephyr`'s subtree, so its line moves to the top of
the file.

**`if CHIP` does not prevent this, and the file's own comment already says
why**: "Kconfig's `if` adds a dependency, it does not hide a definition". The
definition node is still in the tree, still visited first, and the symbol's
*value* is computed globally from all its nodes — which is exactly why the
values are unchanged. The wrap does everything it was written to do; position is
simply not one of the things it controls.

One symbol is worth naming because it shows the third `osource` reaches further
than its name suggests: **`TRUSTED_STORAGE_BACKEND_AEAD_MAX_DATA_SIZE`** moves
too, and it is not an `NCS_SAMPLE_MATTER_*` symbol at all — it comes from
`nrf/samples/matter/common/src/persistent_storage/Kconfig:42`, reached through
that file's `rsource`. Its value (256) is Zephyr's own and did not change.

**Is this the "Kconfig-sourcing bug" the plan warns about?** It is a
Kconfig-sourcing *consequence*, and it changes no image. But it does mean
**`diff zephyr/.config` is no longer a usable regression gate for this
application unless the comparison is order-insensitive**, and any future
baseline comparison — including a re-run of this one — has to say so. That is
the finding: not a broken build, a broken measuring instrument. Sourcing the
three files at the very end, after `source "Kconfig.zephyr"`, would restore
positional stability; whether that is worth doing is a call for whoever owns
the Kconfig, and this document does not make it.

The extra blank line in all six arms is the same mechanism: the writer emits a
blank line around the new block.

## Result 3 — the image gate, which is where the real finding is

A `.config` diff cannot see what got compiled. Five arms come through clean and
one does not.

**`plain`, `gate`, `med`, `sed`, `coex`: ALLOCATED SECTIONS BYTE-IDENTICAL and
the defined-symbol set identical**, base against head, on
`arm-zephyr-eabi-size -A` and `arm-zephyr-eabi-nm --defined-only`. The only
sections that differ at all are `.debug_line` and `.debug_str`, by ~250-550 B,
and they differ because the baseline tree lives at a longer filesystem path than
the repository — debug info records real paths and `-fmacro-prefix-map` does not
reach it. This is the same allocated-sections-only rule the `zero-cost` CI job
already argues for at length.

**`bridge` is a different answer.**

| | base | head | delta |
|---|---|---|---|
| FLASH | 307 496 B (19.70 %) | **310 060 B (19.87 %)** | **+2 564 B** |
| RAM | 185 672 B (70.83 %) | **187 144 B (71.39 %)** | **+1 472 B** |
| `text` | 280 444 | 282 628 | +2 184 |
| `rodata` | 19 808 | 20 196 | +388 |
| `bss` | 78 882 | 80 356 | +1 474 |

Twenty-two defined symbols appear and one disappears:

```
+ profile_common_decode_84            + radiant_common_adapter_{init,decode,is_common_page}
+ profile_env_decode_{capabilities,temperature}   + radiant_env_adapter_{init,decode}
+ profile_fec_decode_{general,trainer}            + radiant_power_adapter_{init,decode,decode_fec}
+ profile_power_decode_std           + common_adapters / env_adapters / power_adapters
+ post_power / post_temperature      + configure_and_open / open_one.isra.0 / self_profiles / self_row
- dwell_update
```

**Root cause: the seven new sources are added inside
`if(CONFIG_RADIANT_BRIDGE)`** in `apps/dongle_thread/CMakeLists.txt`, and
`bridge.conf` is the one existing arm that sets `CONFIG_RADIANT_BRIDGE=y`. The
last four added symbols and the one removed symbol are the rewritten
`src/self_channels.c`, which is compiled under the same condition. Nothing is
mis-sourced; the placement is deliberate and the CMake comment explains it.

**But `bridge.conf` is one of the five arms whose section 7.4 numbers are
supposed to stay comparable, and its image is no longer the image those numbers
were taken on.** `apps/dongle_thread/Kconfig:734-737` states the intent
precisely — "the five existing coexistence arms have to stay byte-identical, and
`bridge.conf` is one of them" — and that is the sentence this measurement
contradicts. `ANT_DONGLE_MATTER_MAP` was correctly defaulted `n` to protect that
property; the packages A-C adapters were added unconditionally under
`RADIANT_BRIDGE` and were not.

This is reported, not fixed. Three answers are available and none of them is
E2's to pick: gate the new adapters on their own symbols the way
`ANT_DONGLE_MATTER_MAP` is gated; accept the change and re-take the `bridge.conf`
row of section 7.4; or declare the bridge arm no longer a coexistence baseline.
What is not available is leaving section 7.4's bridge row as a measurement of
this image, because it is not one.

## The compiled-sources check

The plan asks for confirmation that the new sources are genuinely absent from an
arm that does not ask for them. Object-file inventory of each build tree, plus
the head bridge arm's `.map`:

| Arm | `CONFIG_RADIANT_BRIDGE` | objects, base | objects, head | new sources compiled |
|---|---|---|---|---|
| `med` | n | 678 | 678 | **none** |
| `coex` | n | 280 | 280 | **none** |
| `bridge` | y | 694 | **701** | all seven |

The seven on the bridge arm are `radiant_power_adapter.c`,
`radiant_env_adapter.c`, `radiant_common_adapter.c`, `profile_common.c`,
`profile_power_decode.c`, `profile_fec.c` and `profile_env.c`, each with 15-19
references in `zephyr.map`. `radiant_hr_adapter.c` was already there in both.

**`radiant_matter.c` has zero references in the bridge arm's map**, which is the
positive confirmation that `ANT_DONGLE_MATTER_MAP=n` does what E3 says it does.

**`profile_power.c` is NOT compiled** — only `profile_power_decode.c`. That
split is the reason the CMake comment gives for existing (the master side
reaches `profile_compat.c` and thence `radiant_sec`), and the object inventory
is where it can be checked rather than asserted.

## Limits of this run

- **`nrf54l15dk/nrf54l15/cpuapp` only.** Every arm in `build-new-apps` for this
  application is that board; the `dongle_ti` row is a different application and
  was not touched.
- **Configure and compile only.** Nothing was flashed and no serial port was
  opened. Every claim here is from `.config`, `zephyr.elf`, `zephyr.map` and
  object inventories.
- **The working tree moved during the run, and one arm was rebuilt because of
  it.** Package E3 landed mid-sitting: `apps/dongle_thread/Kconfig` and
  `CMakeLists.txt` were rewritten at 14:30, after the `plain` arm had been built
  at 14:29. That arm was rebuilt `-p always` afterwards and both files were
  re-verified unchanged at the end of the run, so **all six head builds reflect
  `Kconfig` as of 14:30:59 and `CMakeLists.txt` as of 14:30:34**. Anything
  landing after that is outside this measurement. A regression gate run against
  a tree that other agents are editing is worth re-running once the tree is
  still.
- **`bridge` was built to completion in two passes** (the first was interrupted;
  the second resumed the same build directory with the same `.config`). The
  `.config` was generated in the first pass and is the one diffed.
