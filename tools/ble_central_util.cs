// SPDX-License-Identifier: Apache-2.0
//
// ble_central_util - the radio-free half of ble_central: option parsing,
// statistics, verdicts, FTMS record parsers and a JSON writer, plus the
// `selftest` mode that exercises them.
//
// WHY A SELFTEST MODE AND NOT A tools/test_*.py. Every other helper in tools/
// is Python and is tested with pytest, but this logic lives in the C# tool
// that uses it, and a Python twin of it would be a second implementation that
// could pass while the real one is wrong. `ble_central selftest` runs the
// real code with no radio at all, so it is safe on a machine whose Bluetooth
// is busy and it is the first thing to run after editing any of this.
//
// C# 5 ONLY. The compiler is the .NET Framework csc.exe that ships with
// Windows (scripts\ble_central.ps1), which stops at C# 5: no string
// interpolation, no `?.`, no `out var`, no tuples, no `await` inside
// catch/finally. Writing any of them is a compile error that points at this
// file, not at the compiler version.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

static partial class BleCentral
{
    // ── options ─────────────────────────────────────────────────────────────

    class Options
    {
        public string Name = "";
        public string Machine = "treadmill"; // treadmill | bike
        public int Cycles = 50;
        public string Notify = "hrs";        // hrs | treadmill | bike | SSSS:CCCC
        public string Json;                  // null = no JSON
        public bool SkipUnencrypted;
        public bool UnpairAtEnd;
        public bool Uncached;
        public bool Strict;
        public bool NoProbe;
        public int ScanSeconds = 90;
        public int DataTimeoutS = 15;
        public int ReadvTimeoutS = 30;
        public int CpTimeoutS = 10;
        public string DeviceLog;          // Zephyr console capture to cross-check against
        public int GapSeconds = 15;       // scanner-gap calibration window before cycle 1
        public bool WatchWhileConnecting; // old behaviour: keep the active scan up during connect
    }

    // `--key value` and `--flag` after the positional name. Unknown keys are
    // an error rather than ignored: a misspelt `--cycle 5` silently running
    // the default 50 cycles is a 40-minute mistake.
    static Options ParseOptions(string[] args, int start)
    {
        var o = new Options();
        for (int i = start; i < args.Length; i++)
        {
            string a = args[i];
            Func<string> next = () =>
            {
                if (i + 1 >= args.Length)
                {
                    throw new ArgumentException(a + " needs a value");
                }
                i++;
                return args[i];
            };
            switch (a)
            {
                case "--machine":
                    o.Machine = next().ToLowerInvariant();
                    if (o.Machine != "treadmill" && o.Machine != "bike")
                    {
                        throw new ArgumentException("--machine is treadmill or bike");
                    }
                    break;
                case "--cycles": o.Cycles = ParsePositive(next(), a); break;
                case "--notify":
                    o.Notify = next().ToLowerInvariant();
                    {
                        Guid sv, ch;
                        string lb;
                        ResolveNotify(o.Notify, out sv, out ch, out lb); // throws ArgumentException
                    }
                    break;
                case "--json": o.Json = next(); break;
                case "--scan-seconds": o.ScanSeconds = ParsePositive(next(), a); break;
                case "--data-timeout": o.DataTimeoutS = ParsePositive(next(), a); break;
                case "--readv-timeout": o.ReadvTimeoutS = ParsePositive(next(), a); break;
                case "--cp-timeout": o.CpTimeoutS = ParsePositive(next(), a); break;
                case "--skip-unencrypted": o.SkipUnencrypted = true; break;
                case "--unpair-at-end": o.UnpairAtEnd = true; break;
                case "--uncached": o.Uncached = true; break;
                case "--strict": o.Strict = true; break;
                case "--no-probe": o.NoProbe = true; break;
                case "--device-log": o.DeviceLog = next(); break;
                case "--gap-seconds": o.GapSeconds = ParseNonNegative(next(), a); break;
                case "--watch-while-connecting": o.WatchWhileConnecting = true; break;
                default:
                    throw new ArgumentException("unknown option '" + a + "'");
            }
        }
        return o;
    }

    static int ParseNonNegative(string s, string what)
    {
        int v;
        if (!int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) || v < 0)
        {
            throw new ArgumentException(what + " needs a non-negative integer, got '" + s + "'");
        }
        return v;
    }

    static int ParsePositive(string s, string what)
    {
        int v;
        if (!int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) || v <= 0)
        {
            throw new ArgumentException(what + " needs a positive integer, got '" + s + "'");
        }
        return v;
    }

    // ── verdicts ────────────────────────────────────────────────────────────

    // INCONCLUSIVE is not a soft PASS. It means the instrument could not tell
    // (the bench raced the server, Windows auto-paired, a precondition did not
    // hold) and the case needs a rerun or a human; --strict turns it into a
    // failing exit for release runs.
    enum Verdict { Pass, Fail, Inconclusive, Skip }

    static string Word(Verdict v)
    {
        switch (v)
        {
            case Verdict.Pass: return "PASS";
            case Verdict.Fail: return "FAIL";
            case Verdict.Inconclusive: return "INCONCLUSIVE";
            default: return "SKIP";
        }
    }

    static Verdict Worst(IEnumerable<Verdict> vs)
    {
        bool inc = false;
        foreach (var v in vs)
        {
            if (v == Verdict.Fail)
            {
                return Verdict.Fail;
            }
            if (v == Verdict.Inconclusive)
            {
                inc = true;
            }
        }
        return inc ? Verdict.Inconclusive : Verdict.Pass;
    }

    static int ExitCodeFor(IEnumerable<Verdict> vs, bool strict)
    {
        var w = Worst(vs);
        if (w == Verdict.Fail || (strict && w == Verdict.Inconclusive))
        {
            return 1;
        }
        return 0;
    }

    // ── statistics ──────────────────────────────────────────────────────────

    class Summary
    {
        public int N;
        public double Min, Median, Max;
    }

    static Summary Summarise(IEnumerable<double> values)
    {
        var s = values.OrderBy(x => x).ToList();
        if (s.Count == 0)
        {
            return null;
        }
        double med = (s.Count % 2 == 1)
                   ? s[s.Count / 2]
                   : (s[s.Count / 2 - 1] + s[s.Count / 2]) / 2.0;
        return new Summary { N = s.Count, Min = s[0], Median = med, Max = s[s.Count - 1] };
    }

    static string Fmt(Summary s)
    {
        if (s == null)
        {
            return "n=0";
        }
        return string.Format(CultureInfo.InvariantCulture,
                             "min {0,7:F0}  median {1,7:F0}  max {2,7:F0} ms  (n={3})",
                             s.Min, s.Median, s.Max, s.N);
    }

    // ── reconnect-soak pass criteria (plan Phase 5 item 5) ──────────────────

    const double READV_LIMIT_MS = 2000.0;
    const double DATA_LIMIT_MS = 10000.0;

    // Re-advertise is bracketed, not measured, on Windows. The link went down
    // somewhere between our release (`ub` is measured from there, so it is an
    // UPPER bound on the device's re-advertise latency) and the moment
    // Windows admitted it (`lb`, a LOWER bound; null unless the status probe
    // CONFIRMED the drop of a link this cycle actually had). Only a bracket
    // that sits wholly on one side of the limit is a verdict.
    //
    // What the first bench run (2026-11-27, HRM, 50 cycles) taught, against
    // the device's own console:
    //   - `ub` is dominated by Windows, not the device: Windows keeps the link
    //     ~8.9 s after the last handle is released (lazy close), and the
    //     device re-advertised within 0.5 ms of every disconnect.
    //   - "not seen advertising" is the HOST going blind: in one stretch the
    //     device advertised for 60 s and Windows neither surfaced a packet
    //     nor connected. So an unseen re-advertise is INCONCLUSIVE, not FAIL.
    //   - a cycle that never connected has no link to release, and the
    //     probe's instant "Disconnected" in such a cycle is not a drop.
    static Verdict ReadvVerdict(bool linked, double? ub, double? lb, double limit)
    {
        if (!linked)
        {
            return Verdict.Skip;
        }
        if (!ub.HasValue)
        {
            return Verdict.Inconclusive;
        }
        if (ub.Value <= limit)
        {
            return Verdict.Pass;
        }
        if (lb.HasValue && lb.Value > limit)
        {
            return Verdict.Fail;
        }
        return Verdict.Inconclusive;
    }

    // The soak's re-advertise verdict. A device log, when given, is the
    // authority and the Windows brackets become information. Without one, a
    // Windows-side FAIL needs a measured scanner gap; with the gap unmeasured
    // the instrument's resolution is unknown and FAIL becomes INCONCLUSIVE.
    static Verdict ReadvOverall(IEnumerable<Verdict> perCycle, double? gapMs, DeviceLogStats dev, double limit)
    {
        if (dev != null)
        {
            return DeviceReadvVerdict(dev, limit);
        }
        var vs = perCycle.Where(v => v != Verdict.Skip).ToList();
        if (vs.Count == 0)
        {
            return Verdict.Inconclusive;
        }
        var w = Worst(vs);
        if (w == Verdict.Fail && !gapMs.HasValue)
        {
            return Verdict.Inconclusive;
        }
        return w;
    }

    // Data resumes after LINK RESTORE: a cycle that never connected restored
    // nothing and is judged under "connect", not here.
    static Verdict DataVerdict(bool linked, double? dataMs, double limit)
    {
        if (!linked)
        {
            return Verdict.Skip;
        }
        if (!dataMs.HasValue)
        {
            return Verdict.Fail;
        }
        return dataMs.Value <= limit ? Verdict.Pass : Verdict.Fail;
    }

    // ── device console cross-check (-DeviceLog) ─────────────────────────────

    // A Zephyr console capture: `[HH:MM:SS.mmm,uuu] <inf> hrm: BLE
    // disconnected (0x13)` then `... BLE advertising (restart)`, optionally
    // with ANSI colour codes and a leading host wall-clock `HH:MM:SS `. The
    // device's own uptime stamp is the clock used; the host prefix is ignored.
    class DeviceLogStats
    {
        public int Connects, Disconnects, Restarts;
        public int Unpaired;      // a disconnect followed by a connect, or another disconnect, with no restart
        public bool TrailingOpen; // the capture ends after a disconnect with no restart (truncated?)
        public List<double> ReadvMs = new List<double>();
        public List<double> DiscToConnectMs = new List<double>();
    }

    static readonly System.Text.RegularExpressions.Regex AnsiRx =
        new System.Text.RegularExpressions.Regex("\x1b\\[[0-9;]*[A-Za-z]");
    static readonly System.Text.RegularExpressions.Regex UptimeRx =
        new System.Text.RegularExpressions.Regex("\\[(\\d+):(\\d{2}):(\\d{2})\\.(\\d{3}),(\\d{3})\\]");

    static double? ConsoleUptimeMs(string line)
    {
        var m = UptimeRx.Match(line);
        if (!m.Success)
        {
            return null;
        }
        Func<int, double> g = i => double.Parse(m.Groups[i].Value, CultureInfo.InvariantCulture);
        return ((g(1) * 60 + g(2)) * 60 + g(3)) * 1000.0 + g(4) + g(5) / 1000.0;
    }

    static DeviceLogStats ParseDeviceLog(IEnumerable<string> lines)
    {
        var st = new DeviceLogStats();
        double? pendingDisc = null; // waiting for its advertising restart
        double? lastDisc = null;    // waiting for the next connect
        foreach (var raw in lines)
        {
            string line = AnsiRx.Replace(raw, "");
            var t = ConsoleUptimeMs(line);
            if (!t.HasValue)
            {
                continue;
            }
            if (line.Contains("BLE disconnected"))
            {
                st.Disconnects++;
                if (pendingDisc.HasValue)
                {
                    st.Unpaired++;
                }
                pendingDisc = t;
                lastDisc = t;
            }
            else if (line.Contains("advertising (restart)"))
            {
                st.Restarts++;
                if (pendingDisc.HasValue)
                {
                    st.ReadvMs.Add(t.Value - pendingDisc.Value);
                    pendingDisc = null;
                }
            }
            else if (line.Contains("BLE connected"))
            {
                st.Connects++;
                if (pendingDisc.HasValue)
                {
                    st.Unpaired++;
                    pendingDisc = null;
                }
                if (lastDisc.HasValue)
                {
                    st.DiscToConnectMs.Add(t.Value - lastDisc.Value);
                    lastDisc = null;
                }
            }
        }
        st.TrailingOpen = pendingDisc.HasValue;
        return st;
    }

    static Verdict DeviceReadvVerdict(DeviceLogStats d, double limit)
    {
        if (d.ReadvMs.Count == 0)
        {
            return Verdict.Inconclusive; // the capture holds no disconnect/restart pair
        }
        if (d.Unpaired > 0)
        {
            return Verdict.Fail; // a disconnect the device never re-advertised after
        }
        return d.ReadvMs.Max() <= limit ? Verdict.Pass : Verdict.Fail;
    }

    // The gap between consecutive sightings of the device while it is
    // advertising. It is the INSTRUMENT's resolution: a re-advertise cannot be
    // timed more finely than the host scanner surfaces packets, and on this
    // bench it surfaced about one in seven (see Connect()). Pairs closer than
    // 15 ms are an ADV_IND and its SCAN_RSP, one event, and gaps over 5 s are
    // a connection, not a scan gap.
    static double? MedianAdvGapMs(IList<DateTime> times)
    {
        var gaps = new List<double>();
        for (int i = 1; i < times.Count; i++)
        {
            double g = (times[i] - times[i - 1]).TotalMilliseconds;
            if (g >= 15.0 && g <= 5000.0)
            {
                gaps.Add(g);
            }
        }
        var s = Summarise(gaps);
        return s == null ? (double?)null : s.Median;
    }

    // ── FTMS ────────────────────────────────────────────────────────────────

    // Control Point Response Code (Table 4.23): 0x80, the request op code,
    // the result code, optional parameters. Returns the result code, or -1 if
    // `r` is not a response to `op`.
    static int CpResult(byte[] r, byte op)
    {
        if (r == null || r.Length < 3 || r[0] != 0x80 || r[1] != op)
        {
            return -1;
        }
        return r[2];
    }

    class DataParse
    {
        public bool Ok;
        public string Error = "";
        public int Flags;
        public int Elapsed = -1; // seconds; -1 = field absent
    }

    // {flag bit, field octets}. Bit 0 is "More Data" and is INVERTED: the
    // instantaneous speed is present when it is 0. Sizes are FTMS v1.0
    // Table 4.5 (Treadmill Data) and Table 4.9 (Indoor Bike Data); grouped
    // fields (inclination + ramp angle, the three energy fields, force +
    // power) are one entry because one flag gates them together.
    static readonly int[][] TreadmillFields =
    {
        new[] { 0, 2 },  // instantaneous speed (present when bit 0 == 0)
        new[] { 1, 2 },  // average speed
        new[] { 2, 3 },  // total distance
        new[] { 3, 4 },  // inclination + ramp angle setting
        new[] { 4, 4 },  // positive + negative elevation gain
        new[] { 5, 1 },  // instantaneous pace
        new[] { 6, 1 },  // average pace
        new[] { 7, 5 },  // total energy + per hour + per minute
        new[] { 8, 1 },  // heart rate
        new[] { 9, 1 },  // metabolic equivalent
        new[] { 10, 2 }, // elapsed time
        new[] { 11, 2 }, // remaining time
        new[] { 12, 4 }, // force on belt + power output
    };
    const int TreadmillElapsedBit = 10;

    static readonly int[][] BikeFields =
    {
        new[] { 0, 2 },  // instantaneous speed (present when bit 0 == 0)
        new[] { 1, 2 },  // average speed
        new[] { 2, 2 },  // instantaneous cadence
        new[] { 3, 2 },  // average cadence
        new[] { 4, 3 },  // total distance
        new[] { 5, 2 },  // resistance level
        new[] { 6, 2 },  // instantaneous power
        new[] { 7, 2 },  // average power
        new[] { 8, 5 },  // total energy + per hour + per minute
        new[] { 9, 1 },  // heart rate
        new[] { 10, 1 }, // metabolic equivalent
        new[] { 11, 2 }, // elapsed time
        new[] { 12, 2 }, // remaining time
    };
    const int BikeElapsedBit = 11;

    // A record whose flags do not account for exactly its length is a FAIL,
    // not a best effort: a client parses by the flags, so a server whose
    // flags and bytes disagree is mis-read by every app, silently.
    static DataParse ParseMachineData(bool bike, byte[] d)
    {
        var p = new DataParse();
        if (d == null || d.Length < 2)
        {
            p.Error = "shorter than the 2-octet flags field";
            return p;
        }
        int flags = d[0] | (d[1] << 8);
        p.Flags = flags;
        if ((flags & ~0x1FFF) != 0)
        {
            p.Error = string.Format("RFU flag bits set (0x{0:X4}); the record cannot be parsed", flags);
            return p;
        }
        var table = bike ? BikeFields : TreadmillFields;
        int elapsedBit = bike ? BikeElapsedBit : TreadmillElapsedBit;
        int at = 2;
        foreach (var f in table)
        {
            bool present = (f[0] == 0) ? (flags & 1) == 0 : (flags & (1 << f[0])) != 0;
            if (!present)
            {
                continue;
            }
            if (at + f[1] > d.Length)
            {
                p.Error = string.Format("flags 0x{0:X4} need more than {1} octets (field for bit {2})",
                                        flags, d.Length, f[0]);
                return p;
            }
            if (f[0] == elapsedBit)
            {
                p.Elapsed = d[at] | (d[at + 1] << 8);
            }
            at += f[1];
        }
        if (at != d.Length)
        {
            p.Error = string.Format("flags 0x{0:X4} account for {1} octets, record has {2}",
                                    flags, at, d.Length);
            return p;
        }
        p.Ok = true;
        return p;
    }

    // Fitness Machine Feature bit -> data-record flag bit that must agree with
    // it. A feature claimed but never sent, or sent but not claimed, is what
    // makes an app show a blank tile or ignore a field. More Data splits are
    // handled by the caller taking the UNION of flags over several records.
    static readonly int[][] TreadmillFeatureToFlag =
    {
        new[] { 0, 1 },   // average speed
        new[] { 2, 2 },   // total distance
        new[] { 3, 3 },   // inclination
        new[] { 4, 4 },   // elevation gain
        new[] { 5, 5 },   // pace (instantaneous)
        new[] { 9, 7 },   // expended energy
        new[] { 10, 8 },  // heart rate
        new[] { 11, 9 },  // metabolic equivalent
        new[] { 12, 10 }, // elapsed time
        new[] { 13, 11 }, // remaining time
        new[] { 15, 12 }, // force on belt and power output
    };

    static readonly int[][] BikeFeatureToFlag =
    {
        new[] { 0, 1 },   // average speed
        new[] { 1, 2 },   // cadence
        new[] { 2, 4 },   // total distance
        new[] { 7, 5 },   // resistance level
        new[] { 9, 8 },   // expended energy
        new[] { 10, 9 },  // heart rate
        new[] { 11, 10 }, // metabolic equivalent
        new[] { 12, 11 }, // elapsed time
        new[] { 13, 12 }, // remaining time
        new[] { 14, 6 },  // power measurement
    };

    static List<string> FeatureFlagMismatches(bool bike, uint features, int flagUnion)
    {
        var bad = new List<string>();
        foreach (var m in bike ? BikeFeatureToFlag : TreadmillFeatureToFlag)
        {
            bool claimed = (features & (1u << m[0])) != 0;
            bool sent = (flagUnion & (1 << m[1])) != 0;
            if (claimed != sent)
            {
                bad.Add(string.Format("feature bit {0} {1} but data flag bit {2} {3}",
                                      m[0], claimed ? "set" : "clear",
                                      m[1], sent ? "sent" : "never sent"));
            }
        }
        return bad;
    }

    // Supported X Range (0x2AD4/5/6/8): minimum, maximum, minimum increment,
    // each 16-bit. Speed is unsigned; inclination, resistance and power are
    // signed in v1.0. Returns "" when sane.
    static string CheckRange(byte[] d, bool signedMinMax)
    {
        if (d == null || d.Length != 6)
        {
            return string.Format("length {0}, expected 6", d == null ? 0 : d.Length);
        }
        int min = d[0] | (d[1] << 8);
        int max = d[2] | (d[3] << 8);
        int step = d[4] | (d[5] << 8);
        if (signedMinMax)
        {
            min = (short)min;
            max = (short)max;
        }
        if (min > max)
        {
            return string.Format("minimum {0} > maximum {1}", min, max);
        }
        if (step == 0)
        {
            return "minimum increment is 0";
        }
        return "";
    }

    // FTMS Service Data (FTMS v1.0 SS3.1.1): UUID 0x1826, Flags (bit 0 =
    // Fitness Machine Available), Fitness Machine Type uint16 (bit 0
    // treadmill ... bit 5 indoor bike). `sd` is the AD payload AFTER the type
    // octet, i.e. starting with the UUID. Returns "" when it advertises the
    // wanted machine type.
    static string CheckServiceData(byte[] sd, bool bike)
    {
        if (sd == null)
        {
            return "no FTMS Service Data (AD type 0x16, UUID 0x1826) in any advertisement";
        }
        if (sd.Length < 5)
        {
            return string.Format("FTMS Service Data is {0} octets, expected UUID + Flags + Type = 5", sd.Length);
        }
        int type = sd[3] | (sd[4] << 8);
        int want = bike ? (1 << 5) : 1;
        var errs = new List<string>();
        if ((sd[2] & 0x01) == 0)
        {
            errs.Add("Fitness Machine Available flag is clear");
        }
        if ((type & want) == 0)
        {
            errs.Add(string.Format("machine type 0x{0:X4} lacks the {1} bit", type, bike ? "indoor bike (5)" : "treadmill (0)"));
        }
        return string.Join("; ", errs);
    }

    // ── JSON (no serializer in the reference set, and none is needed) ───────

    static string J(string s)
    {
        if (s == null)
        {
            return "null";
        }
        var b = new StringBuilder("\"");
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': b.Append("\\\""); break;
                case '\\': b.Append("\\\\"); break;
                case '\n': b.Append("\\n"); break;
                case '\r': b.Append("\\r"); break;
                case '\t': b.Append("\\t"); break;
                default:
                    if (c < 0x20)
                    {
                        b.AppendFormat("\\u{0:x4}", (int)c);
                    }
                    else
                    {
                        b.Append(c);
                    }
                    break;
            }
        }
        return b.Append('"').ToString();
    }

    static string J(double? v)
    {
        return v.HasValue ? v.Value.ToString("F1", CultureInfo.InvariantCulture) : "null";
    }

    static string J(Summary s)
    {
        if (s == null)
        {
            return "null";
        }
        return string.Format(CultureInfo.InvariantCulture,
                             "{{\"n\": {0}, \"min_ms\": {1:F1}, \"median_ms\": {2:F1}, \"max_ms\": {3:F1}}}",
                             s.N, s.Min, s.Median, s.Max);
    }

    static string Hex(byte[] b)
    {
        return b == null ? "" : BitConverter.ToString(b).Replace("-", " ");
    }

    // ── selftest ────────────────────────────────────────────────────────────

    static int SelfTest()
    {
        int fails = 0;
        Action<bool, string> check = (ok, what) =>
        {
            Console.WriteLine("{0}  {1}", ok ? "PASS" : "FAIL", what);
            if (!ok)
            {
                fails++;
            }
        };

        var s = Summarise(new double[] { 5, 1, 3 });
        check(s.N == 3 && s.Min == 1 && s.Median == 3 && s.Max == 5, "Summarise odd count");
        s = Summarise(new double[] { 4, 1, 3, 2 });
        check(s.Median == 2.5 && s.Max == 4, "Summarise even count averages the middle pair");
        check(Summarise(new double[0]) == null, "Summarise of nothing is null, not zeros");

        check(ReadvVerdict(true, 1500, null, 2000) == Verdict.Pass, "readv: upper bound under limit passes");
        check(ReadvVerdict(true, 2500, 300, 2000) == Verdict.Inconclusive, "readv: bracket straddling limit is inconclusive");
        check(ReadvVerdict(true, 4000, 2500, 2000) == Verdict.Fail, "readv: confirmed lower bound over limit fails");
        check(ReadvVerdict(true, 4000, null, 2000) == Verdict.Inconclusive, "readv: unconfirmed disconnect is inconclusive");
        check(ReadvVerdict(true, null, null, 2000) == Verdict.Inconclusive, "readv: unseen re-advertise is the host, inconclusive");
        check(ReadvVerdict(false, 9000, 8900, 2000) == Verdict.Skip, "readv: a cycle that never connected is skipped");
        check(ReadvOverall(new[] { Verdict.Pass, Verdict.Fail }, null, null, 2000) == Verdict.Inconclusive,
              "readv overall: FAIL without a measured scanner gap is inconclusive");
        check(ReadvOverall(new[] { Verdict.Pass, Verdict.Fail }, 900, null, 2000) == Verdict.Fail,
              "readv overall: FAIL with a measured gap stands");
        check(ReadvOverall(new[] { Verdict.Skip }, 900, null, 2000) == Verdict.Inconclusive,
              "readv overall: nothing judged is inconclusive");
        check(DataVerdict(true, 9999, 10000) == Verdict.Pass && DataVerdict(true, 10001, 10000) == Verdict.Fail
              && DataVerdict(true, null, 10000) == Verdict.Fail && DataVerdict(false, null, 10000) == Verdict.Skip,
              "data verdict at the 10 s limit; no link, no verdict");

        // Lines as captured in bench-logs/2026-11-27-hrm-m1/soak-console.log:
        // host wall clock, device uptime, ANSI colour, CR.
        string E = "\x1b";
        var dlog = new[]
        {
            "23:34:46 [00:42:33.635,318] " + E + "[0m<inf> hrm: BLE connected" + E + "[0m\r",
            "23:34:56 [00:42:43.459,373] " + E + "[0m<inf> radiant_ble_adv: HRS notifications off" + E + "[0m\r",
            "23:34:56 [00:42:43.459,392] " + E + "[0m<inf> hrm: BLE disconnected (0x13)" + E + "[0m\r",
            "23:34:56 [00:42:43.459,871] " + E + "[0m<inf> radiant_ble_adv: BLE advertising (restart)" + E + "[0m\r",
            "23:35:10 [00:42:58.188,094] " + E + "[0m<inf> radiant_api: noise rf=57 n=242 floor=-105" + E + "[0m\r",
            "23:35:28 [00:43:15.602,326] " + E + "[0m<inf> hrm: BLE connected" + E + "[0m\r",
            "[00:43:19.610,146] <inf> hrm: BLE disconnected (0x13)",
            "[00:43:19.610,630] <inf> radiant_ble_adv: BLE advertising (restart)",
            "garbage line with no stamp: BLE disconnected",
        };
        var ds = ParseDeviceLog(dlog);
        check(ds.Connects == 2 && ds.Disconnects == 2 && ds.Restarts == 2 && ds.Unpaired == 0 && !ds.TrailingOpen,
              "device log: counts, ANSI and host prefix stripped, unstamped line ignored");
        check(ds.ReadvMs.Count == 2 && Math.Abs(ds.ReadvMs[0] - 0.479) < 1e-6 && Math.Abs(ds.ReadvMs[1] - 0.484) < 1e-6,
              "device log: disconnect -> restart 0.479 ms and 0.484 ms");
        check(ds.DiscToConnectMs.Count == 1 && Math.Abs(ds.DiscToConnectMs[0] - 32142.934) < 1e-6,
              "device log: disconnect -> next connect");
        check(DeviceReadvVerdict(ds, 2000) == Verdict.Pass, "device log verdict passes");
        check(ReadvOverall(new[] { Verdict.Fail }, 900, ds, 2000) == Verdict.Pass,
              "device log overrides the Windows brackets");
        var bad = ParseDeviceLog(new[]
        {
            "[00:00:01.000,000] <inf> hrm: BLE disconnected (0x08)",
            "[00:00:05.000,000] <inf> hrm: BLE connected",
            "[00:00:06.000,000] <inf> hrm: BLE disconnected (0x13)",
            "[00:00:09.500,000] <inf> radiant_ble_adv: BLE advertising (restart)",
            "[00:00:10.000,000] <inf> hrm: BLE disconnected (0x13)",
        });
        check(bad.Unpaired == 1 && bad.TrailingOpen && bad.ReadvMs.Count == 1 && bad.ReadvMs[0] == 3500.0,
              "device log: missing restart counted, trailing disconnect flagged");
        check(DeviceReadvVerdict(bad, 2000) == Verdict.Fail, "device log: a missing restart fails");
        check(DeviceReadvVerdict(ParseDeviceLog(new string[0]), 2000) == Verdict.Inconclusive,
              "device log with no pairs is inconclusive");

        check(Worst(new[] { Verdict.Pass, Verdict.Skip }) == Verdict.Pass, "Worst: skip does not fail");
        check(Worst(new[] { Verdict.Pass, Verdict.Inconclusive }) == Verdict.Inconclusive, "Worst: inconclusive");
        check(Worst(new[] { Verdict.Inconclusive, Verdict.Fail }) == Verdict.Fail, "Worst: fail wins");
        check(ExitCodeFor(new[] { Verdict.Inconclusive }, false) == 0
              && ExitCodeFor(new[] { Verdict.Inconclusive }, true) == 1, "--strict fails on inconclusive");

        var t = new DateTime(2026, 1, 1);
        var times = new List<DateTime> { t, t.AddMilliseconds(3), t.AddMilliseconds(1003),
                                         t.AddMilliseconds(2003), t.AddMilliseconds(9003) };
        check(MedianAdvGapMs(times) == 1000, "adv gap ignores scan-response pairs and connection gaps");

        check(CpResult(new byte[] { 0x80, 0x00, 0x01 }, 0x00) == 1, "CP response parsed");
        check(CpResult(new byte[] { 0x80, 0x07, 0x05 }, 0x00) == -1, "CP response for another op rejected");
        check(CpResult(new byte[] { 0x80, 0x00 }, 0x00) == -1, "short CP response rejected");

        // The treadmill's own 18-octet record (apps/treadmill/src/treadmill_ble.c
        // TD_RECORD_LEN): speed, distance, incline+ramp, elevation, HR, elapsed.
        int tf = (1 << 2) | (1 << 3) | (1 << 4) | (1 << 8) | (1 << 10);
        var rec = new byte[] { (byte)tf, (byte)(tf >> 8), 0xF4, 0x01, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 120, 0x2C, 0x01 };
        var p = ParseMachineData(false, rec);
        check(p.Ok && p.Elapsed == 300, "treadmill record: elapsed 300 s at the right offset");
        var shortRec = rec.Take(17).ToArray();
        check(!ParseMachineData(false, shortRec).Ok, "treadmill record one octet short fails");
        var longRec = rec.Concat(new byte[] { 0 }).ToArray();
        check(!ParseMachineData(false, longRec).Ok, "treadmill record with a trailing octet fails");
        check(!ParseMachineData(false, new byte[] { 0x00, 0x20, 0, 0 }).Ok, "RFU flag bit fails");
        check(ParseMachineData(false, new byte[] { 0x01, 0x00 }).Ok, "More Data alone (no speed) parses");

        // Indoor bike: speed, cadence, power, elapsed.
        int bf = (1 << 2) | (1 << 6) | (1 << 11);
        var brec = new byte[] { (byte)bf, (byte)(bf >> 8), 0x10, 0x0E, 0xB4, 0x00, 0xC8, 0x00, 0x05, 0x00 };
        p = ParseMachineData(true, brec);
        check(p.Ok && p.Elapsed == 5, "indoor bike record: elapsed 5 s");

        check(FeatureFlagMismatches(false, (1u << 2) | (1u << 12), (1 << 2) | (1 << 10)).Count == 0,
              "feature/flag agreement passes");
        check(FeatureFlagMismatches(false, 1u << 12, 0).Count == 1, "elapsed claimed but not sent is caught");
        check(FeatureFlagMismatches(true, 0, 1 << 6).Count == 1, "bike power sent but not claimed is caught");

        check(CheckRange(new byte[] { 0, 0, 0x4C, 0x04, 10, 0 }, false) == "", "speed range sane");
        check(CheckRange(new byte[] { 0xEC, 0xFF, 0x96, 0x00, 5, 0 }, true) == "", "signed incline range -2.0..15.0 sane");
        check(CheckRange(new byte[] { 0xEC, 0xFF, 0x96, 0x00, 5, 0 }, false) != "", "same bytes unsigned: min > max caught");
        check(CheckRange(new byte[] { 0, 0, 1, 0, 0, 0 }, false) != "", "zero increment caught");
        check(CheckRange(new byte[] { 0, 0, 1, 0 }, false) != "", "short range caught");

        check(CheckServiceData(new byte[] { 0x26, 0x18, 0x01, 0x01, 0x00 }, false) == "", "treadmill service data");
        check(CheckServiceData(new byte[] { 0x26, 0x18, 0x01, 0x01, 0x00 }, true) != "", "treadmill service data is not a bike");
        check(CheckServiceData(new byte[] { 0x26, 0x18, 0x01, 0x20, 0x00 }, true) == "", "indoor bike service data");
        check(CheckServiceData(new byte[] { 0x26, 0x18, 0x00, 0x01, 0x00 }, false) != "", "not-available flag caught");
        check(CheckServiceData(null, false) != "", "missing service data caught");

        var o = ParseOptions(new[] { "x", "x", "--machine", "bike", "--cycles", "5", "--json", "o.json",
                                     "--skip-unencrypted", "--strict" }, 2);
        check(o.Machine == "bike" && o.Cycles == 5 && o.Json == "o.json" && o.SkipUnencrypted && o.Strict,
              "option parsing");
        o = ParseOptions(new[] { "--device-log", "c.log", "--gap-seconds", "0", "--watch-while-connecting" }, 0);
        check(o.DeviceLog == "c.log" && o.GapSeconds == 0 && o.WatchWhileConnecting, "soak diagnostic options");
        check(new Options().GapSeconds == 15 && !new Options().WatchWhileConnecting,
              "defaults: 15 s gap calibration, watcher paused while connecting");
        bool threw = false;
        try { ParseOptions(new[] { "--cycle", "5" }, 0); }
        catch (ArgumentException) { threw = true; }
        check(threw, "unknown option is an error, not ignored");
        threw = false;
        try { ParseOptions(new[] { "--cycles", "0" }, 0); }
        catch (ArgumentException) { threw = true; }
        check(threw, "--cycles 0 rejected");

        Guid sv, ch;
        string label;
        ResolveNotify("hrs", out sv, out ch, out label);
        check(sv == Sig16(0x180D) && ch == Sig16(0x2A37), "--notify hrs");
        ResolveNotify("bike", out sv, out ch, out label);
        check(sv == Sig16(UUID_FTMS) && ch == Sig16(UUID_INDOOR_BIKE_DATA), "--notify bike");
        ResolveNotify("1816:2a5b", out sv, out ch, out label);
        check(sv == Sig16(0x1816) && ch == Sig16(0x2A5B), "--notify SSSS:CCCC");
        threw = false;
        try { ParseOptions(new[] { "--notify", "hrm" }, 0); }
        catch (ArgumentException) { threw = true; }
        check(threw, "--notify with an unknown name rejected before any radio use");

        check(J("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"", "JSON escaping");

        Console.WriteLine(fails == 0 ? "selftest: all passed" : string.Format("selftest: {0} failed", fails));
        return fails == 0 ? 0 : 1;
    }
}
