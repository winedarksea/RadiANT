# Preservation

Checked by: [`.github/workflows/linkcheck.yml`](../.github/workflows/linkcheck.yml).
Its weekly run fails — and opens an issue — if any external URL under
`archive/**` or `docs/**` has rotted, and its budget step fails if `archive/`
exceeds the 10 MB stated below. The redistribution decisions in this document
are legal positions and no test can assert them; they are reviewed, not
checked.

Garmin shut the ANT+ membership and certification programs, and the engineering
support behind them, on **30 June 2025**. Nothing about that made the protocol
stop working, and nothing about it makes the documents disappear tomorrow. What
it removed is the *organisation that was keeping them reachable*. A URL with an
owner behind it rots slowly; a URL with nobody behind it rots at the speed of
the next site redesign.

So `archive/` exists to hold the things this project would not be able to get
back. It is deliberately not a mirror of the internet. It is the subset that is
**perishable, load-bearing, and safe for us to keep** — and, where keeping the
artifact itself is not safe, enough recorded fact that the artifact can be
recognised if a copy turns up somewhere else.

---

## The rule that shapes everything here

**An archive of pointers rots. The durable artifact is derived prose and our
own recordings.**

Every item below is classified against that sentence. A specification we may
not republish still yields `docs/ant-serial-protocol.md` and
`docs/ant-radio-link.md`, which are ours, which are in git, and which survive
thisisant.com going dark. A driver package we may not redistribute still yields
`archive/drivers/install-notes.md` and an INF we wrote ourselves, which are
worth more operationally than the package was. A wire capture of our own device
is ours outright and is the only artifact in the tree that makes the rebuild
*verifiable* rather than merely plausible.

Where the honest answer is "we did not obtain this", it says so and names the
command a human should run. Nothing in `archive/` is inferred, reconstructed
from memory, or filled in to look complete.

---

## The 10 MB budget

**`archive/` may not exceed 10 MB.** That number is stated here so it is
arguable rather than assumed, and enforced in `linkcheck.yml` so it does not
quietly drift.

The reason is that git stores every version of every binary forever. A clone is
not the current size of the tree, it is the sum of everything the tree has ever
been, and a repository people are asked to fork to run CI on is a repository
whose clone cost is a real cost. 10 MB is roughly the point past which
`git clone` stops feeling instant on a normal connection, and it is comfortably
above what text plus one firmware image needs.

**Anything larger becomes a GitHub release asset instead**, referenced from the
`README.md` in the relevant `archive/` subdirectory with its SHA-256. Release
assets are not in the object database, are not cloned, and can be replaced
without rewriting history — which makes them the right home for a large capture
set, a full flash image beyond the one already here, or anything binary that
grows over time.

Current usage:

| Directory | Contents | Size |
|---|---|---|
| `archive/firmware/` | two `.uf2` flash readbacks | ~1.9 MB |
| `archive/drivers/` | our INF, install notes, provenance | < 20 KB |
| `archive/specs/` | pointers, hashes, snapshot URLs | < 20 KB |
| `archive/host-api/` | `ANT_DLL` export list, Zwift subset, JSON | < 30 KB |
| `archive/captures/` | format notes; **no captures recorded yet** | < 10 KB |
| `archive/benchmarks/` | schema and README; **no baselines yet** | < 20 KB |

The two directories with nothing in them are the two that matter most. See
*What is missing* at the end.

---

## Redistribution decisions

| Class | Decision | Why |
|---|---|---|
| **Our own firmware images** (`archive/firmware/`) | **Committed, with a caveat** | They are builds of this repository, so the *sources* are ours. But `CURRENT-after-0.8.0.uf2` is a whole-flash readback of a device running the sdk-ant backend, so `libant.a`'s object code is inside it. See the caveat below — this is the one item in `archive/` whose position is not clean. |
| **Garmin's libusb-win32 driver package** | **Not redistributed** | The package is Garmin's. libusb0 inside it is LGPL, and LGPL on a component does not make the vendor's package redistributable — it constrains what Garmin owed *us*, not what we may hand on. What we keep instead is the SHA-256 of every file, both provenance paths, and an INF we wrote from scratch. |
| **ANT Message Protocol and Usage Rev 5.1** | **Not redistributed — pointers and hash only** | Free to download is not the same as ours to republish. Garmin retains copyright in the document; no licence accompanies the download. The SHA-256 lets anyone confirm the copy they find is the copy we read, which is the part that actually matters for a clean-room record. |
| **ANT+ device profile documents** | **Not redistributed, and not committed even as mirrors** | Adopter-gated, Garmin copyright. Committing a mirror would combine redistribution risk with clean-room contamination in one act: it would put adopter-gated material in the same tree as `ant_core/**`, whose whole defence is that it never touched it. Listed in `archive/specs/README.md` as references only. |
| **`ANT_DLL.dll` export names and ordinals** | **Committed** | An export table is a list of names and numbers describing an interface. Names are not copyrightable subject matter, and the ordinals are facts about a binary's layout. Recording them is the same act as recording a USB PID. This is the evidence that bounds what *any* Windows ANT application can ask a dongle for, which is why the optional-feature decisions in `README.md` are answerable rather than arguable. |
| **`ANT_DLL.dll` and `ZwiftApp.exe` themselves** | **Not committed** | Third-party binaries, no redistribution right, and no need: the facts extracted from them are in `archive/host-api/` and the extraction procedure is written down so anyone with their own copy can reproduce it byte for byte. |
| **Our own wire captures** (`archive/captures/`) | **Committed — highest value, entirely safe** | Recordings of our own device, made on our own bench, with our own tools. No third party has any interest in them. They are also the only thing here that turns "the rebuild is compatible" from a claim into a regression test. |
| **sdk-ant performance baselines** (`archive/benchmarks/`) | **Committed — and the most perishable item in the project** | Numbers we measured. They are not derived from sdk-ant's code, only from its behaviour on our hardware. The moment access to sdk-ant lapses these become unobtainable, and every A/B gate in `docs/testing.md` is measured against them. You cannot A/B against a baseline you never recorded. |

### The caveat on `archive/firmware/CURRENT-after-0.8.0.uf2`

That file is a full flash readback — Adafruit bootloader 0.8.0 at `0x1000`, our
application at `0x26000` — of a Feather running the sdk-ant backend. The
application therefore contains `libant.a`'s compiled code, linked in.

It predates this policy and is already in the repository's history, so removing
it now would cost a history rewrite and would not un-publish anything. It is
kept because it is the only artifact that documents the exact bootloader window
(`0x1000`–`0xEA000`) the flash-log offsets are chosen against, and that fact is
otherwise unrecoverable without a bench session. But it is recorded here as a
known exception rather than left to be discovered:

- **Do not attach it to a release**, and do not add further sdk-ant-linked
  images to `archive/`.
- A `core` or `stub` build carries no `libant.a` and has none of this problem.
  Once `ant_core` can produce an equivalent readback, that is the image this
  directory should keep.
- If the sdk-ant licence position is ever formally reviewed, this file is the
  first thing on the list.

---

## What is missing, and who must produce it

Two directories are empty of data and that is the single largest gap in the
preservation effort. Both need a human at the bench; neither can be produced by
reading this repository.

1. **`archive/captures/radio/*.antcap`** — one capture per ANT+ profile,
   roughly 150 KB each, recorded from real hardware with
   `tools/ant_verify.py --record`. These replay with no board at all and are
   what every future decoder change is regression-tested against.
   `archive/captures/radio/README.md` specifies the exact set.
2. **`archive/benchmarks/*.json`** — the Phase 4 sdk-ant baselines, including
   the attenuated-link sensitivity curve. `archive/benchmarks/README.md` and
   the schema beside it define precisely what a bench session must write, so
   that session spends its time measuring rather than inventing a format.

Everything else in `archive/` was obtainable from a desk and has been obtained.
