// SPDX-License-Identifier: Apache-2.0
//
// ble_central_soak - `reconnect-soak`: N cycles of connect -> subscribe ->
// first notification -> disconnect, timing each step (plan Phase 5 item 4,
// pass criteria from item 5: re-advertise <= 2 s after disconnect, data
// resumes <= 10 s after the link is restored).
//
// HOW EACH NUMBER IS MEASURED, AND WHAT IT CANNOT SEE.
//
//   connect   FromBluetoothAddressAsync -> ConnectionStatus == Connected,
//             with GattSession.MaintainConnection = true doing the connect
//             (see OpenLink). This is Windows' connection scheduling as much
//             as the device: on 2026-11-27 it took 2-27 s against an HRM that
//             was advertising throughout, and 8 of 50 attempts timed out at
//             30 s - two of those links then came up 0.2 s and 2 s AFTER the
//             tool had given up (a cancelled connect is not cancelled at once).
//   data      Connected -> first notification: discovery (cached unless
//             --uncached) + CCCD write + the server's first notify. This is
//             the "data resumes after link restore" criterion; a cycle that
//             never connected is not judged on it.
//   readv     BRACKETED, because Windows cannot say when the link dropped (it
//             has no disconnect call; see ble_central_link.cs):
//               upper bound  release -> first advertisement seen
//               lower bound  Windows CONFIRMED Disconnected -> first adv seen
//             On 2026-11-27 the upper bound was ~9 s on every clean cycle and
//             the device's own console showed why: Windows holds the link
//             ~8.9 s after the last handle goes (lazy close; it drops the
//             interval to 1 s meanwhile), and the device re-advertised within
//             0.5 ms of the disconnect. So the Windows figure is a Windows
//             figure. -DeviceLog <console capture> gives the device-side
//             disconnect -> re-advertise time, and when given it is the
//             re-advertise verdict; a phone or nRF central is the other
//             authoritative path.
//   teardown  release -> Windows reports Disconnected, only when the probe saw
//             the link go from Connected to Disconnected.
//   gap       median gap between sightings in a calibration window before
//             cycle 1 (--gap-seconds, default 15). Inside the soak the next
//             cycle starts at the FIRST sighting after a release, so there is
//             never a second one to measure a gap from - which is why the
//             first run printed "n/a". Without a gap the scanner's resolution
//             is unknown and a Windows-side re-advertise FAIL is INCONCLUSIVE.
//
// THE WATCHER IS PAUSED WHILE CONNECTING, by default. An active scan and a
// connection initiation share the host controller, and the first run's slow
// and failed connects are consistent with them competing (not proven:
// --watch-while-connecting restores the old behaviour for an A/B, and then the
// cycle records how many sightings arrived during the connect attempt - many
// sightings and no connection means Windows saw the device and did not
// initiate). The watcher restarts after the first notification, before the
// release, so it is up before the device can re-advertise.
//
// The disconnect probe is a FRESH BluetoothLEDevice opened after the release
// and polled until the device is seen advertising again or 15 s pass, so it
// outlives Windows' ~9 s lazy close. If a bare BluetoothLEDevice ever holds a
// link on some Windows build it would delay the re-advertise; --no-probe
// rules that out. Every status the probe and the link saw is recorded per
// cycle in the JSON with wall-clock times, to line up with a device console
// that prefixes host wall-clock time.
//
// C# 5 only; see ble_central_util.cs.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;

static partial class BleCentral
{
    class Cycle
    {
        public int N;
        public string Error;
        public string ErrorKind;   // connect | subscribe | data | other; null when clean
        public bool Linked;        // Windows reported Connected in this cycle
        public bool NotSeen;       // no sighting within --readv-timeout of release
        public bool LateConnect;   // the probe saw a link come up after a connect timeout
        public double? ConnectMs, DataMs, DataTotalMs, TeardownMs, ReadvUbMs, ReadvLbMs;
        public int AdvWhileConnected;
        public int? SightingsDuringConnect; // null when the watcher was paused
        public DateTime T0, Release;
        public DateTime? ConnectedAt, FirstDataAt, SightingAt;
        public string LinkStatus = "", ProbeStatus = "";
        public Verdict Readv = Verdict.Skip, Data = Verdict.Skip;
    }

    class ProbeResult
    {
        public DateTime? FirstRead;
        public bool FirstReadConnected;
        public List<KeyValuePair<DateTime, string>> Transitions = new List<KeyValuePair<DateTime, string>>();
    }

    // Poll a fresh probe, recording every status change, until `stop` says so
    // or `ms` pass.
    static async Task<ProbeResult> ProbeStatus(ulong addr, int ms, Func<bool> stop)
    {
        var r = new ProbeResult();
        BluetoothLEDevice probe = null;
        try
        {
            probe = await Within(BluetoothLEDevice.FromBluetoothAddressAsync(addr), 5000, "status probe");
            if (probe == null)
            {
                return r;
            }
            var end = DateTime.UtcNow.AddMilliseconds(ms);
            string last = null;
            while (DateTime.UtcNow < end)
            {
                var st = probe.ConnectionStatus;
                var now = DateTime.UtcNow;
                if (!r.FirstRead.HasValue)
                {
                    r.FirstRead = now;
                    r.FirstReadConnected = st == BluetoothConnectionStatus.Connected;
                }
                if (st.ToString() != last)
                {
                    last = st.ToString();
                    r.Transitions.Add(new KeyValuePair<DateTime, string>(now, last));
                }
                if (stop())
                {
                    break;
                }
                await Task.Delay(20);
            }
            return r;
        }
        catch (Exception)
        {
            return r;
        }
        finally
        {
            if (probe != null)
            {
                probe.Dispose();
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
        }
    }

    static string Local(DateTime utc)
    {
        return utc.ToLocalTime().ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture);
    }

    static string Local(DateTime? utc)
    {
        return utc.HasValue ? Local(utc.Value) : null;
    }

    static string Trace(IEnumerable<KeyValuePair<DateTime, string>> t)
    {
        return string.Join(" ", t.Select(x => Local(x.Key) + "=" + x.Value));
    }

    static async Task<int> ReconnectSoak(string needle, Options o)
    {
        Guid svcU, chrU;
        string label;
        ResolveNotify(o.Notify, out svcU, out chrU, out label);
        DeviceLogStats devCheck = null;
        if (o.DeviceLog != null && !File.Exists(o.DeviceLog))
        {
            throw new ArgumentException("--device-log: no file " + o.DeviceLog);
        }
        var cache = o.Uncached ? BluetoothCacheMode.Uncached : BluetoothCacheMode.Cached;
        var cycles = new List<Cycle>();
        List<DateTime> gapTimes;

        var adv = new AdvTracker(needle);
        adv.Start();
        try
        {
            Say("reconnect-soak: {0} cycle(s) against '{1}', notify {2}, watcher {3} during connect",
                o.Cycles, needle, label, o.WatchWhileConnecting ? "ON" : "paused");
            var first = await adv.WaitWhere(x => x.Name != null &&
                                            x.Name.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0,
                                            o.ScanSeconds * 1000);
            if (first == null)
            {
                throw new Exception("no advertiser named '" + needle + "' within " + o.ScanSeconds + " s");
            }
            ulong addr = first.Addr;
            Say("found '{0}' at {1} (rssi {2})", first.Name, Addr(addr), first.Rssi);

            var calStart = DateTime.UtcNow;
            if (o.GapSeconds > 0)
            {
                Say("calibrating the scanner for {0} s (sightings of an advertising, unconnected device)", o.GapSeconds);
                await Task.Delay(o.GapSeconds * 1000);
            }
            gapTimes = adv.Between(calStart, DateTime.UtcNow).Select(x => x.T).ToList();
            var lastCal = adv.Between(calStart, DateTime.UtcNow).LastOrDefault();
            if (lastCal != null)
            {
                addr = lastCal.Addr;
            }

            for (int i = 1; i <= o.Cycles; i++)
            {
                var c = new Cycle { N = i };
                cycles.Add(c);
                Link link = null;
                c.T0 = DateTime.UtcNow;
                if (!o.WatchWhileConnecting)
                {
                    adv.Stop();
                }
                try
                {
                    try
                    {
                        link = await OpenLink(addr, 30000);
                    }
                    catch (Exception)
                    {
                        c.ErrorKind = "connect";
                        throw;
                    }
                    c.Linked = true;
                    c.ConnectedAt = link.ConnectedAt.Value;
                    c.ConnectMs = Ms(c.T0, c.ConnectedAt.Value);
                    c.ErrorKind = "subscribe";
                    var svc = await GetService(link, svcU, cache);
                    var chr = await GetChar(svc, chrU, cache);
                    var ch = new Chan(chr);
                    ch.Attach();
                    link.Chans.Add(ch);
                    var cc = await WriteCccd(chr, GattClientCharacteristicConfigurationDescriptorValue.Notify);
                    if (cc.Status != GattCommunicationStatus.Success)
                    {
                        throw new Exception("subscribe failed: " + Describe(cc));
                    }
                    c.ErrorKind = "data";
                    var got = await ch.Wait(0, o.DataTimeoutS * 1000, null);
                    if (got == null)
                    {
                        throw new Exception("no notification within " + o.DataTimeoutS + " s of subscribing");
                    }
                    c.ErrorKind = null;
                    c.FirstDataAt = ch.At(0).Key;
                    c.DataMs = Ms(c.ConnectedAt.Value, c.FirstDataAt.Value);
                    c.DataTotalMs = Ms(c.T0, c.FirstDataAt.Value);
                }
                catch (Exception ex)
                {
                    var inner = ex is AggregateException && ex.InnerException != null ? ex.InnerException : ex;
                    c.Error = inner.Message;
                    if (c.ErrorKind == null)
                    {
                        c.ErrorKind = "other";
                    }
                }

                // Watcher back up BEFORE the release: the device cannot
                // re-advertise until Windows lets the link go, which is
                // after this.
                if (o.WatchWhileConnecting)
                {
                    c.SightingsDuringConnect = adv.Between(c.T0, c.ConnectedAt ?? DateTime.UtcNow).Count;
                }
                else
                {
                    adv.Start();
                }

                c.Release = DateTime.UtcNow;
                // A sighting in the first second after Connected can be an
                // advertisement from just before it, delivered late by the
                // watcher; only later ones mean "advertises while connected".
                // (Only meaningful while the watcher ran during the link.)
                c.AdvWhileConnected = c.ConnectedAt.HasValue && o.WatchWhileConnecting
                                    ? adv.Between(c.ConnectedAt.Value.AddSeconds(1), c.Release).Count : 0;
                if (link != null)
                {
                    lock (link.Transitions)
                    {
                        c.LinkStatus = Trace(link.Transitions);
                    }
                    link.Close();
                }

                var release = c.Release;
                Task<ProbeResult> probeT = o.NoProbe
                    ? Task.FromResult(new ProbeResult())
                    : ProbeStatus(addr, 15000, () => adv.FirstWhere(x => x.T > release) != null);
                var advT = adv.WaitAfter(release, o.ReadvTimeoutS * 1000);
                await Task.WhenAll(probeT, advT);
                var probe = probeT.Result;
                var back = advT.Result;
                c.ProbeStatus = Trace(probe.Transitions);
                c.LateConnect = !c.Linked && probe.Transitions.Any(x => x.Value == "Connected");

                if (back != null)
                {
                    addr = back.Addr;
                    c.SightingAt = back.T;
                    c.ReadvUbMs = Ms(release, back.T);
                }
                else
                {
                    c.NotSeen = true;
                }

                // Disconnect CONFIRMED only for a link this cycle had: either
                // the probe watched it go Connected -> Disconnected, or its
                // very first read already said Disconnected (the drop was
                // before that read, so sighting - first read is still a
                // valid lower bound).
                DateTime? down = null;
                if (c.Linked && probe.FirstRead.HasValue)
                {
                    bool sawConnected = false;
                    foreach (var t in probe.Transitions)
                    {
                        if (t.Value == "Connected")
                        {
                            sawConnected = true;
                        }
                        else if (t.Value == "Disconnected" && sawConnected)
                        {
                            down = t.Key;
                            c.TeardownMs = Ms(release, t.Key);
                        }
                    }
                    if (!down.HasValue && !probe.FirstReadConnected)
                    {
                        down = probe.FirstRead;
                    }
                }
                if (down.HasValue && back != null)
                {
                    c.ReadvLbMs = Math.Max(0.0, Ms(down.Value, back.T));
                }

                c.Readv = c.AdvWhileConnected > 0 ? Verdict.Skip
                        : ReadvVerdict(c.Linked, c.ReadvUbMs, c.ReadvLbMs, READV_LIMIT_MS);
                c.Data = DataVerdict(c.Linked, c.DataMs, DATA_LIMIT_MS);

                Say("cycle {0,3} @{1}: connect {2}  data {3}  teardown {4}  readv {5}..{6}{7}{8}{9}{10}{11}",
                    i, Local(release), F(c.ConnectMs), F(c.DataMs), F(c.TeardownMs), F(c.ReadvLbMs), F(c.ReadvUbMs),
                    c.SightingsDuringConnect.HasValue ? "  sightings-while-connecting " + c.SightingsDuringConnect : "",
                    c.AdvWhileConnected > 0 ? "  (advertised while connected: readv n/a)" : "",
                    c.LateConnect ? "  LATE-CONNECT (link came up after the timeout)" : "",
                    c.NotSeen ? "  NOT-SEEN within " + o.ReadvTimeoutS + " s" : "",
                    c.Error == null ? "" : "  ERROR(" + c.ErrorKind + ") " + c.Error);
            }
        }
        finally
        {
            adv.Stop();
        }

        if (o.DeviceLog != null)
        {
            // Shared read: the console logger may still have the file open.
            using (var fs = new FileStream(o.DeviceLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var rd = new StreamReader(fs, Encoding.UTF8))
            {
                var lines = new List<string>();
                string l;
                while ((l = rd.ReadLine()) != null)
                {
                    lines.Add(l);
                }
                devCheck = ParseDeviceLog(lines);
            }
        }

        double? gap = MedianAdvGapMs(gapTimes);

        var linked = cycles.Where(x => x.Linked).ToList();
        var sConnect = Summarise(cycles.Where(x => x.ConnectMs.HasValue).Select(x => x.ConnectMs.Value));
        var sData = Summarise(cycles.Where(x => x.DataMs.HasValue).Select(x => x.DataMs.Value));
        var sTotal = Summarise(cycles.Where(x => x.DataTotalMs.HasValue).Select(x => x.DataTotalMs.Value));
        var sTear = Summarise(cycles.Where(x => x.TeardownMs.HasValue).Select(x => x.TeardownMs.Value));
        var sUb = Summarise(linked.Where(x => x.ReadvUbMs.HasValue).Select(x => x.ReadvUbMs.Value));
        var sLb = Summarise(linked.Where(x => x.ReadvLbMs.HasValue).Select(x => x.ReadvLbMs.Value));
        Summary sDev = devCheck == null ? null : Summarise(devCheck.ReadvMs);
        Summary sDevConn = devCheck == null ? null : Summarise(devCheck.DiscToConnectMs);

        Console.WriteLine();
        Say("reconnect-soak summary: {0} cycle(s), {1} connected, {2} connect failure(s), {3} late connect(s), " +
            "{4} not seen re-advertising, {5} other error(s)",
            cycles.Count, linked.Count, cycles.Count(x => x.ErrorKind == "connect"), cycles.Count(x => x.LateConnect),
            cycles.Count(x => x.NotSeen), cycles.Count(x => x.ErrorKind != null && x.ErrorKind != "connect"));
        Console.WriteLine("  connect (Windows)               {0}", Fmt(sConnect));
        Console.WriteLine("  data after connect              {0}", Fmt(sData));
        Console.WriteLine("  data from connect start         {0}", Fmt(sTotal));
        Console.WriteLine("  teardown (release->Disconnected){0}", Fmt(sTear));
        Console.WriteLine("  re-advertise upper bound (Win)  {0}", Fmt(sUb));
        Console.WriteLine("  re-advertise lower bound (Win)  {0}", Fmt(sLb));
        Console.WriteLine("  scanner sighting gap (median)   {0}",
                          gap.HasValue ? gap.Value.ToString("F0", CultureInfo.InvariantCulture) +
                          string.Format(" ms over {0} sighting(s) in the calibration window  <- instrument floor",
                                        gapTimes.Count)
                          : string.Format("n/a ({0} sighting(s) in the calibration window)", gapTimes.Count));
        if (devCheck != null)
        {
            Console.WriteLine("  DEVICE LOG {0}: {1} connect(s), {2} disconnect(s), {3} restart(s), {4} without restart{5}",
                              o.DeviceLog, devCheck.Connects, devCheck.Disconnects, devCheck.Restarts, devCheck.Unpaired,
                              devCheck.TrailingOpen ? ", capture ends mid-disconnect" : "");
            Console.WriteLine("  re-advertise (device log)       {0}", FmtFine(sDev));
            Console.WriteLine("  disconnect -> next connect (dev){0}", Fmt(sDevConn));
        }

        var readvV = ReadvOverall(cycles.Select(x => x.Readv), gap, devCheck, READV_LIMIT_MS);
        var dataVs = cycles.Where(x => x.Data != Verdict.Skip).Select(x => x.Data).ToList();
        var dataV = dataVs.Count == 0 ? Verdict.Inconclusive : Worst(dataVs);
        // Errors after the link was up are the device's or the tool's; connect
        // timeouts and unseen re-advertising are the Windows host's until a
        // device log or another central says otherwise.
        Verdict errV = cycles.Any(x => x.ErrorKind != null && x.ErrorKind != "connect") ? Verdict.Fail
                     : cycles.Any(x => x.ErrorKind == "connect" || x.NotSeen) ? Verdict.Inconclusive
                     : Verdict.Pass;
        Func<IEnumerable<Cycle>, string> list = cs =>
            string.Join(", ", cs.Take(10).Select(x => "#" + x.N)) + (cs.Count() > 10 ? ", ..." : "");

        string readvNote;
        if (devCheck != null)
        {
            readvNote = "  -- from the DEVICE LOG (authoritative)" +
                        (sDev == null ? ": no disconnect/restart pairs in it" : ", max " + FmtMs(sDev.Max)) +
                        (devCheck.Unpaired > 0 ? string.Format("; {0} disconnect(s) never re-advertised", devCheck.Unpaired) : "");
        }
        else if (readvV == Verdict.Pass)
        {
            readvNote = "";
        }
        else
        {
            readvNote = "  -- Windows-side bracket, cycles " +
                        list(cycles.Where(x => x.Readv == Verdict.Fail || x.Readv == Verdict.Inconclusive)) +
                        (gap.HasValue ? "" : "; scanner gap unmeasured") +
                        ". Windows' lazy close dominates this figure: give -DeviceLog for the device's own";
        }
        Console.WriteLine("{0,-12} re-advertise <= {1:F0} ms after disconnect{2}", Word(readvV), READV_LIMIT_MS, readvNote);
        Console.WriteLine("{0,-12} data <= {1:F0} ms after link restore{2}", Word(dataV), DATA_LIMIT_MS,
                          dataV == Verdict.Pass ? "" : "  -- cycles " + list(cycles.Where(x => x.Data == Verdict.Fail)));
        Console.WriteLine("{0,-12} every cycle clean{1}", Word(errV),
                          errV == Verdict.Pass ? "" : "  -- " + string.Join("; ",
                              cycles.Where(x => x.Error != null || x.NotSeen).Take(5).Select(x => "#" + x.N + ": " +
                                  (x.Error ?? "not seen re-advertising"))) +
                          (errV == Verdict.Inconclusive ? " (host-side: connect timeouts / scanner blind)" : ""));
        var all = new List<Verdict> { readvV, dataV, errV };
        var overall = Worst(all.Select(v => v == Verdict.Skip ? Verdict.Inconclusive : v));
        Say("reconnect-soak -> {0}", Word(overall));

        if (o.Json != null)
        {
            var b = new StringBuilder();
            b.Append("{\n");
            b.AppendFormat("  \"mode\": \"reconnect-soak\",\n  \"name\": {0},\n  \"notify\": {1},\n  \"cycles_requested\": {2},\n",
                           J(needle), J(label), o.Cycles);
            b.AppendFormat("  \"utc\": {0},\n  \"cache\": {1},\n  \"probe\": {2},\n  \"watcher_during_connect\": {3},\n",
                           J(DateTime.UtcNow.ToString("o")), J(o.Uncached ? "uncached" : "cached"),
                           o.NoProbe ? "false" : "true", o.WatchWhileConnecting ? "true" : "false");
            b.AppendFormat("  \"limits_ms\": {{\"readv\": {0}, \"data\": {1}}},\n",
                           J(READV_LIMIT_MS), J(DATA_LIMIT_MS));
            b.AppendFormat("  \"verdict\": {{\"readv\": {0}, \"readv_source\": {1}, \"data\": {2}, \"errors\": {3}, \"overall\": {4}}},\n",
                           J(Word(readvV)), J(devCheck != null ? "device_log" : "windows"),
                           J(Word(dataV)), J(Word(errV)), J(Word(overall)));
            b.AppendFormat("  \"summary\": {{\"connect\": {0}, \"data\": {1}, \"data_total\": {2}, \"teardown\": {3}, " +
                           "\"readv_ub\": {4}, \"readv_lb\": {5}, \"scanner_gap_median_ms\": {6}, \"calibration_sightings\": {7}}},\n",
                           J(sConnect), J(sData), J(sTotal), J(sTear), J(sUb), J(sLb), J(gap), gapTimes.Count);
            if (devCheck != null)
            {
                b.AppendFormat("  \"device_log\": {{\"file\": {0}, \"connects\": {1}, \"disconnects\": {2}, \"restarts\": {3}, " +
                               "\"unpaired\": {4}, \"trailing_open\": {5}, \"readv\": {6}, \"disconnect_to_connect\": {7}}},\n",
                               J(o.DeviceLog), devCheck.Connects, devCheck.Disconnects, devCheck.Restarts,
                               devCheck.Unpaired, devCheck.TrailingOpen ? "true" : "false",
                               JFine(sDev), J(sDevConn));
            }
            b.Append("  \"cycles\": [\n");
            for (int i = 0; i < cycles.Count; i++)
            {
                var c = cycles[i];
                b.AppendFormat("    {{\"n\": {0}, \"linked\": {1}, \"connect_ms\": {2}, \"data_ms\": {3}, \"data_total_ms\": {4}, " +
                               "\"teardown_ms\": {5}, \"readv_ub_ms\": {6}, \"readv_lb_ms\": {7}, \"adv_while_connected\": {8}, " +
                               "\"sightings_while_connecting\": {9}, \"late_connect\": {10}, \"not_seen\": {11}, " +
                               "\"readv\": {12}, \"data\": {13}, \"error_kind\": {14}, \"error\": {15}, ",
                               c.N, c.Linked ? "true" : "false", J(c.ConnectMs), J(c.DataMs), J(c.DataTotalMs),
                               J(c.TeardownMs), J(c.ReadvUbMs), J(c.ReadvLbMs), c.AdvWhileConnected,
                               c.SightingsDuringConnect.HasValue ? c.SightingsDuringConnect.Value.ToString() : "null",
                               c.LateConnect ? "true" : "false", c.NotSeen ? "true" : "false",
                               J(Word(c.Readv)), J(Word(c.Data)), J(c.ErrorKind), J(c.Error));
                b.AppendFormat("\"local\": {{\"start\": {0}, \"connected\": {1}, \"first_data\": {2}, \"release\": {3}, " +
                               "\"sighting\": {4}}}, \"link_status\": {5}, \"probe_status\": {6}}}{7}\n",
                               J(Local(c.T0)), J(Local(c.ConnectedAt)), J(Local(c.FirstDataAt)), J(Local(c.Release)),
                               J(Local(c.SightingAt)), J(c.LinkStatus), J(c.ProbeStatus),
                               i + 1 < cycles.Count ? "," : "");
            }
            b.Append("  ]\n}\n");
            File.WriteAllText(o.Json, b.ToString());
            Say("wrote {0}", o.Json);
        }
        return ExitCodeFor(all, o.Strict);
    }

    // `device-log <file>`: the -DeviceLog analysis on its own, offline, for a
    // console captured during an earlier soak. No radio.
    static int DeviceLogOnly(string path)
    {
        DeviceLogStats d;
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (var rd = new StreamReader(fs, Encoding.UTF8))
        {
            var lines = new List<string>();
            string l;
            while ((l = rd.ReadLine()) != null)
            {
                lines.Add(l);
            }
            d = ParseDeviceLog(lines);
        }
        Console.WriteLine("device log {0}: {1} connect(s), {2} disconnect(s), {3} restart(s), {4} without restart{5}",
                          path, d.Connects, d.Disconnects, d.Restarts, d.Unpaired,
                          d.TrailingOpen ? ", capture ends mid-disconnect" : "");
        Console.WriteLine("  re-advertise (device)           {0}", FmtFine(Summarise(d.ReadvMs)));
        Console.WriteLine("  disconnect -> next connect      {0}", Fmt(Summarise(d.DiscToConnectMs)));
        var v = DeviceReadvVerdict(d, READV_LIMIT_MS);
        Console.WriteLine("{0,-12} re-advertise <= {1:F0} ms after disconnect (device log)", Word(v), READV_LIMIT_MS);
        return v == Verdict.Fail ? 1 : 0;
    }

    static string F(double? ms)
    {
        return ms.HasValue ? ms.Value.ToString("F0", CultureInfo.InvariantCulture) + "ms" : "-";
    }

    static string FmtMs(double ms)
    {
        return ms.ToString("F3", CultureInfo.InvariantCulture) + " ms";
    }

    // Device-side figures are sub-millisecond; the soak's F0 format would
    // print them all as 0.
    static string FmtFine(Summary s)
    {
        if (s == null)
        {
            return "n=0";
        }
        return string.Format(CultureInfo.InvariantCulture,
                             "min {0,9:F3}  median {1,9:F3}  max {2,9:F3} ms  (n={3})", s.Min, s.Median, s.Max, s.N);
    }

    static string JFine(Summary s)
    {
        if (s == null)
        {
            return "null";
        }
        return string.Format(CultureInfo.InvariantCulture,
                             "{{\"n\": {0}, \"min_ms\": {1:F3}, \"median_ms\": {2:F3}, \"max_ms\": {3:F3}}}",
                             s.N, s.Min, s.Median, s.Max);
    }
}
