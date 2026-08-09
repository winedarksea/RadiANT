# `archive/`

Checked by: [`.github/workflows/linkcheck.yml`](../.github/workflows/linkcheck.yml)
— weekly, for every external URL below this directory and for the 10 MB budget.

The things this project could not get back. The charter — why each item is or
is not redistributable, and what the budget is for — is
[`docs/preservation.md`](../docs/preservation.md). Read that first; this file
is only an index.

| Directory | Holds | State |
|---|---|---|
| [`firmware/`](firmware/) | Two `.uf2` flash readbacks from the Feather, and what each one proves | Complete |
| [`drivers/`](drivers/) | Our own `ant_libusb_win32.inf`, install notes, and provenance for the Garmin package we do **not** redistribute | Complete |
| [`specs/`](specs/) | Pointers, document numbers, SHA-256s, retrieval dates and snapshot URLs. No PDFs | Complete |
| [`host-api/`](host-api/) | `ANT_DLL.dll`'s 154 exports with ordinals, the 44 Zwift resolves, and the JSON the tools read | Complete |
| [`captures/`](captures/) | `.antcap` radio captures and `.antser` serial traces | **Empty — needs a bench session** |
| [`benchmarks/`](benchmarks/) | sdk-ant A/B baselines, including the sensitivity curve | **Empty — needs a bench session** |

The two empty ones are the two the whole plan exists to protect. Each carries a
`README.md` specifying exactly what a bench session must record, so that
session is spent measuring rather than deciding on a format.
