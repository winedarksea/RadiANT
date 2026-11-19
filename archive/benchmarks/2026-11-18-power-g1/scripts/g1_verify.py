"""tools/ant_verify.py with one addition for Gate G1: each channel's summary
also carries its raw radio-clock receive intervals (g1_radio_intervals_s), so
the transmitter's mean period can be measured to well under a microsecond
(the 16-bit 32768 Hz receive timestamp telescopes over the window).
Arguments are ant_verify.py's, unchanged."""
import sys

sys.path.insert(0, r"C:\Users\Colin\ant_dongle\tools")
import ant_verify as av  # noqa: E402

_orig = av.ChannelAnalyzer.summary


def _summary(self, *a, **k):
    d = _orig(self, *a, **k)
    d["g1_radio_intervals_s"] = list(self.radio_intervals)
    return d


av.ChannelAnalyzer.summary = _summary
sys.exit(av.main())
