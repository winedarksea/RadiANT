# radiant

A clean-room, ANT+-compatible link layer for Zephyr, as a self-contained
Zephyr module. It decides what goes on the air and when; a radio HAL backend
decides how. No Nordic sdk-ant, no proprietary radio stack, no path outside
this directory - `radiant/` is meant to be dropped into a project with
nothing else from the repository it came from on its include path.

This file covers what the module is, what it needs, and how to add it to a
project. For a working ~40-line downstream receiver, see
[`docs/integration.md`](docs/integration.md) and
[`../tests/import_smoke/`](../tests/import_smoke/), the application that
integration guide is drawn from and that CI builds against an isolated copy
of this directory to prove the claim above is true, not aspirational.

## What it is

- 32 ANT+-protocol-compatible channels (`RADIANT_CHANNEL_COUNT`), the serial
  protocol's own ceiling - a superset of the 8 a stock ANT+ stick configures.
- Background scan mode, which stock ANT+ sticks advertise off and do not
  implement.
- A vendor-neutral radio HAL (`include/radiant/radiant_radio_hal.h`) with two
  backends shipped today - nRF52/nRF53/nRF54L (direct-peripheral and an
  MPSL-timeslot-arbitrated variant for coexisting with BLE or Thread) and a
  TI CC13x2/CC26x2 port - and a capability-query struct so core policy never
  tests for a backend by name.
- Optional security (`CONFIG_RADIANT_SEC`): AES-based payload
  authentication/confidentiality and a compat-attestation layer, entirely
  zero-cost when off.
- A sample bus and sink registry (`src/bridge/`) and the ANT+ device profile
  library (`src/profiles/`) - library material for a node or a bridge to
  build against; see "What is not compiled for you" below for why these are
  source you add, not a linked library you get for free.

See [`../docs/backends.md`](../docs/backends.md) for the full backend/HAL
contract, [`../docs/ant-radio-link.md`](../docs/ant-radio-link.md) for the
on-air reference this module was built against, and
[`../docs/decisions/`](../docs/decisions/) for the design decisions that are
not up for re-litigation from a downstream project (naming, the clean-room
policy, the security scope).

## What it needs

- Zephyr, via `ZEPHYR_EXTRA_MODULES` (or a west manifest project entry - see
  below). No other module and nothing from sdk-ant, on any backend.
- A radio, if you want the module to receive or transmit anything -
  `CONFIG_RADIANT_BACKEND_NULL` has a real lifecycle and clock and refuses
  every arm, which is the right choice for a compile check or an application
  that only needs the module to *link*. It is this module's **Kconfig** choice
  default, which is what `tests/import_smoke` builds against on `native_sim`.
  Note that no *application* in this repository defaults to it any more - each
  defaults to a backend its silicon can really use, because a null-radio image
  is indistinguishable from a dead antenna. See
  [`cmake/radiant_backend.cmake`](../cmake/radiant_backend.cmake) for that
  reasoning.
- `-DRADIANT_BACKEND=nrf` or `=cc26xx` for a real radio, chosen the same way
  every application in this repository chooses it - see
  [`cmake/radiant_backend.cmake`](../cmake/radiant_backend.cmake). That helper
  lives at the repository root, **not** inside this directory: it is a
  convenience for the applications here, and an out-of-tree consumer either
  copies it or does the two lines by hand as shown below. Nothing in the
  module's own build depends on it, which is what keeps `radiant/` importable
  standalone - the property the `radiant/ imports standalone` CI job proves.

## Adding it to a project

Point `ZEPHYR_EXTRA_MODULES` at a checkout of this directory, before
`find_package(Zephyr)`:

```cmake
# radiant_backend.cmake lives at this repository's root, beside radiant/ -
# copy it into your own tree if you want the helper.
include(path/to/radiant_backend.cmake)
radiant_select_backend(path/to/radiant nrf RADIANT_BACKEND_CONF)
# ... merge RADIANT_BACKEND_CONF into your build's Kconfig fragments ...
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)
radiant_assert_backend("my_app")   # after find_package(Zephyr), once Kconfig has run
```

Or, more simply, without the shared helper:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES path/to/radiant)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)
```

then set `CONFIG_RADIANT=y` and `CONFIG_RADIANT_BACKEND_NRF=y` (or `_CC26XX`)
in your own `prj.conf`. The shared helper exists because
`-DRADIANT_BACKEND=...` on the command line does not reach an image built
under sysbuild unless something parses the sysbuild cache for it - see that
file's own header for the full mechanism - but a project not using sysbuild,
or one willing to set the Kconfig symbol directly, does not need it.

In a west manifest, add this repository as a project and set
`west-commands`/`group-filter` as needed, or - once this module is split out
by `git filter-repo`, which this self-containment rule is what keeps a
non-event - point at the split repository directly. Nothing about the
`ZEPHYR_EXTRA_MODULES` mechanism above changes either way.

## What is not compiled for you

`radiant/CMakeLists.txt` builds the core link layer, the security primitives
and the selected radio backend - `zephyr_library()`, one Zephyr library
target, gated by `CONFIG_RADIANT`. It does **not** compile `src/bridge/` or
`src/profiles/`, even though both live inside this directory. Those are
library material a consuming application adds to its own `target_sources()`,
the same way [`../apps/hrm_ble/CMakeLists.txt`](../apps/hrm_ble/CMakeLists.txt)
and [`../tests/import_smoke/CMakeLists.txt`](../tests/import_smoke/CMakeLists.txt)
do - because which profile a node speaks and which sinks a bridge feeds are
product decisions, not module ones, and linking every profile into every
image that only wants the core would cost flash for code nothing calls.

The antr_* adapter (`radiant_on_message()`'s one required implementation,
the two `radiant_event_*` port hooks, and everything that turns this module's
channel/event/scheduler API into a running receiver) is also not here - see
[`../apps/common/ant_radio_radiant.c`](../apps/common/ant_radio_radiant.c)
for the reference implementation, application-side by design (the module
must reach no application header, and an ANT-serial dongle's bridge protocol
is exactly that). A project that is not building an ANT-serial dongle writes
its own, much smaller version of it - `docs/integration.md` is that version,
in full.
