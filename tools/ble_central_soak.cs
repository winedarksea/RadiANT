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
//             (see OpenLink). Includes Windows' own scheduling of the
//             connection attempt, which a phone's figure would not.
//   data      Connected -> first notification on the chosen characteristic:
//             discovery (cached unless --uncached) + CCCD write + the
//             server's first notify. This is the "data resumes" criterion.
//   readv     The device's re-advertise latency, BRACKETED because Windows
//             cannot say when the link actually dropped (it has no
//             disconnect call; see ble_central_link.cs):
//               upper bound  release -> first advertisement seen
//               lower bound  Windows reports Disconnected -> first adv seen
//             Both include the host scanner's latency, which on this bench is
//             large (it surfaced ~1 in 7 packets of a 1 s advertiser, see
//             Connect() in ble_central.cs); the median gap between sightings
//             while the device advertises is printed as the instrument floor.
//             A readv figure near that floor is the scanner, not the device.
//   teardown  release -> Windows reports Disconnected. Windows' lazy close,
//             reported so it is not mistaken for the device.
//
// The disconnect status comes from a FRESH BluetoothLEDevice probe opened
// after the release. A bare BluetoothLEDevice should not hold a link, but if
// this Windows build lets it, the probe would keep the device connected and
// fake a re-advertise failure - so the probe gives up after 5 s and is
// disposed, after which only the advertisement decides (--no-probe skips it
// entirely). If the device advertises WHILE connected (a multi-connection
// build), re-advertise is meaningless and the cycle line says so.
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
        public double? ConnectMs, DataMs, DataTotalMs, TeardownMs, ReadvUbMs, ReadvLbMs;
        public int AdvWhileConnected;
        public Verdict Readv = Verdict.Fail, Data = Verdict.Fail;
    }

    // Poll a fresh probe until Windows says Disconnected. Returns when it did,
    // or null if it would not say so within `ms`.
    static async Task<DateTime?> WaitDisconnected(ulong addr, int ms)
    {
        BluetoothLEDevice probe = null;
        try
        {
            probe = await Within(BluetoothLEDevice.FromBluetoothAddressAsync(addr), 5000, "status probe");
            if (probe == null)
            {
                return null;
            }
            var end = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < end)
            {
                if (probe.ConnectionStatus == BluetoothConnectionStatus.Disconnected)
                {
                    return DateTime.UtcNow;
                }
                await Task.Delay(20);
            }
            return null;
        }
        catch (Exception)
        {
            return null;
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

    static async Task<int> ReconnectSoak(string needle, Options o)
    {
        Guid svcU, chrU;
        string label;
        ResolveNotify(o.Notify, out svcU, out chrU, out label);
        var cache = o.Uncached ? BluetoothCacheMode.Uncached : BluetoothCacheMode.Cached;
        var cycles = new List<Cycle>();
        var connectedSpans = new List<KeyValuePair<DateTime, DateTime>>();

        var adv = new AdvTracker(needle);
        adv.Start();
        try
        {
            Say("reconnect-soak: {0} cycle(s) against '{1}', notify {2}", o.Cycles, needle, label);
            var first = await adv.WaitWhere(x => x.Name != null &&
                                            x.Name.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0,
                                            o.ScanSeconds * 1000);
            if (first == null)
            {
                throw new Exception("no advertiser named '" + needle + "' within " + o.ScanSeconds + " s");
            }
            ulong addr = first.Addr;
            Say("found '{0}' at {1} (rssi {2})", first.Name, Addr(addr), first.Rssi);

            for (int i = 1; i <= o.Cycles; i++)
            {
                var c = new Cycle { N = i };
                cycles.Add(c);
                Link link = null;
                var t0 = DateTime.UtcNow;
                DateTime? connAt = null;
                try
                {
                    link = await OpenLink(addr, 30000);
                    connAt = link.ConnectedAt.Value;
                    c.ConnectMs = Ms(t0, connAt.Value);
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
                    var got = await ch.Wait(0, o.DataTimeoutS * 1000, null);
                    if (got == null)
                    {
                        throw new Exception("no notification within " + o.DataTimeoutS + " s of subscribing");
                    }
                    var firstAt = ch.At(0).Key;
                    c.DataMs = Ms(connAt.Value, firstAt);
                    c.DataTotalMs = Ms(t0, firstAt);
                }
                catch (Exception ex)
                {
                    var inner = ex is AggregateException && ex.InnerException != null ? ex.InnerException : ex;
                    c.Error = inner.Message;
                }

                var release = DateTime.UtcNow;
                // A sighting in the first second after Connected can be an
                // advertisement from just before it, delivered late by the
                // watcher; only later ones mean "advertises while connected".
                c.AdvWhileConnected = connAt.HasValue
                                    ? adv.Between(connAt.Value.AddSeconds(1), release).Count : 0;
                connectedSpans.Add(new KeyValuePair<DateTime, DateTime>(t0, release));
                if (link != null)
                {
                    link.Close();
                }

                Task<DateTime?> downT = o.NoProbe ? Task.FromResult((DateTime?)null)
                                                  : WaitDisconnected(addr, 5000);
                var advT = adv.WaitAfter(release, o.ReadvTimeoutS * 1000);
                await Task.WhenAll(downT, advT);
                var down = downT.Result;
                var back = advT.Result;
                if (down.HasValue)
                {
                    c.TeardownMs = Ms(release, down.Value);
                }
                if (back != null)
                {
                    addr = back.Addr;
                    c.ReadvUbMs = Ms(release, back.T);
                    if (down.HasValue)
                    {
                        c.ReadvLbMs = Math.Max(0.0, Ms(down.Value, back.T));
                    }
                }
                c.Readv = c.AdvWhileConnected > 0 ? Verdict.Skip
                        : ReadvVerdict(c.ReadvUbMs, c.ReadvLbMs, READV_LIMIT_MS);
                c.Data = DataVerdict(c.DataMs, DATA_LIMIT_MS);
                if (back == null)
                {
                    c.Error = (c.Error == null ? "" : c.Error + "; ") +
                              "not seen advertising within " + o.ReadvTimeoutS + " s of release";
                }

                Say("cycle {0,3}: connect {1}  data {2}  teardown {3}  readv {4}..{5}{6}{7}",
                    i, F(c.ConnectMs), F(c.DataMs), F(c.TeardownMs), F(c.ReadvLbMs), F(c.ReadvUbMs),
                    c.AdvWhileConnected > 0 ? "  (advertised while connected: readv n/a)" : "",
                    c.Error == null ? "" : "  ERROR " + c.Error);
                if (back == null)
                {
                    // It may still be holding a link someone else owns, or be
                    // off. Try again from the last address; the next cycle's
                    // connect timeout bounds the wait.
                    Say("  (continuing; the next connect may fail the same way)");
                }
            }
        }
        finally
        {
            adv.Stop();
        }

        // Instrument floor: sightings outside every connected span.
        var times = adv.Times().Where(t => !connectedSpans.Any(s => t > s.Key && t <= s.Value)).ToList();
        double? gap = MedianAdvGapMs(times);

        var ok = cycles.Where(x => x.Error == null).ToList();
        var sConnect = Summarise(cycles.Where(x => x.ConnectMs.HasValue).Select(x => x.ConnectMs.Value));
        var sData = Summarise(cycles.Where(x => x.DataMs.HasValue).Select(x => x.DataMs.Value));
        var sTotal = Summarise(cycles.Where(x => x.DataTotalMs.HasValue).Select(x => x.DataTotalMs.Value));
        var sTear = Summarise(cycles.Where(x => x.TeardownMs.HasValue).Select(x => x.TeardownMs.Value));
        var sUb = Summarise(cycles.Where(x => x.ReadvUbMs.HasValue).Select(x => x.ReadvUbMs.Value));
        var sLb = Summarise(cycles.Where(x => x.ReadvLbMs.HasValue).Select(x => x.ReadvLbMs.Value));

        Console.WriteLine();
        Say("reconnect-soak summary: {0} cycle(s), {1} clean, {2} with errors", cycles.Count, ok.Count,
            cycles.Count - ok.Count);
        Console.WriteLine("  connect                         {0}", Fmt(sConnect));
        Console.WriteLine("  data after connect              {0}", Fmt(sData));
        Console.WriteLine("  data from connect start         {0}", Fmt(sTotal));
        Console.WriteLine("  teardown (release->Disconnected){0}", Fmt(sTear));
        Console.WriteLine("  re-advertise upper bound        {0}", Fmt(sUb));
        Console.WriteLine("  re-advertise lower bound        {0}", Fmt(sLb));
        Console.WriteLine("  scanner sighting gap (median)   {0}",
                          gap.HasValue ? gap.Value.ToString("F0", CultureInfo.InvariantCulture) +
                          " ms  <- instrument floor for re-advertise" : "n/a");

        var readvVs = cycles.Where(x => x.Readv != Verdict.Skip).Select(x => x.Readv).ToList();
        var readvV = readvVs.Count == 0 ? Verdict.Skip : Worst(readvVs);
        var dataV = Worst(cycles.Select(x => x.Data));
        var errV = cycles.Count == ok.Count ? Verdict.Pass : Verdict.Fail;
        Func<IEnumerable<Cycle>, string> list = cs =>
            string.Join(", ", cs.Take(10).Select(x => "#" + x.N)) + (cs.Count() > 10 ? ", ..." : "");

        Console.WriteLine("{0,-12} re-advertise <= {1:F0} ms after disconnect{2}", Word(readvV), READV_LIMIT_MS,
                          readvV == Verdict.Pass ? "" : "  -- cycles " +
                          list(cycles.Where(x => x.Readv == Verdict.Fail || x.Readv == Verdict.Inconclusive)) +
                          (readvV == Verdict.Inconclusive ? " (bracket straddles the limit: Windows teardown lag)" : "") +
                          (readvV == Verdict.Skip ? " (device advertised while connected)" : ""));
        Console.WriteLine("{0,-12} data <= {1:F0} ms after link restore{2}", Word(dataV), DATA_LIMIT_MS,
                          dataV == Verdict.Pass ? "" : "  -- cycles " + list(cycles.Where(x => x.Data == Verdict.Fail)));
        Console.WriteLine("{0,-12} every cycle clean{1}", Word(errV),
                          errV == Verdict.Pass ? "" : "  -- " + string.Join("; ",
                              cycles.Where(x => x.Error != null).Take(5).Select(x => "#" + x.N + ": " + x.Error)));
        var all = new List<Verdict> { readvV == Verdict.Skip ? Verdict.Inconclusive : readvV, dataV, errV };
        Say("reconnect-soak -> {0}", Word(Worst(all)));

        if (o.Json != null)
        {
            var b = new StringBuilder();
            b.Append("{\n");
            b.AppendFormat("  \"mode\": \"reconnect-soak\",\n  \"name\": {0},\n  \"notify\": {1},\n  \"cycles_requested\": {2},\n",
                           J(needle), J(label), o.Cycles);
            b.AppendFormat("  \"utc\": {0},\n  \"cache\": {1},\n  \"probe\": {2},\n",
                           J(DateTime.UtcNow.ToString("o")), J(o.Uncached ? "uncached" : "cached"),
                           o.NoProbe ? "false" : "true");
            b.AppendFormat("  \"limits_ms\": {{\"readv\": {0}, \"data\": {1}}},\n",
                           J(READV_LIMIT_MS), J(DATA_LIMIT_MS));
            b.AppendFormat("  \"verdict\": {{\"readv\": {0}, \"data\": {1}, \"errors\": {2}, \"overall\": {3}}},\n",
                           J(Word(readvV)), J(Word(dataV)), J(Word(errV)), J(Word(Worst(all))));
            b.AppendFormat("  \"summary\": {{\"connect\": {0}, \"data\": {1}, \"data_total\": {2}, \"teardown\": {3}, " +
                           "\"readv_ub\": {4}, \"readv_lb\": {5}, \"scanner_gap_median_ms\": {6}}},\n",
                           J(sConnect), J(sData), J(sTotal), J(sTear), J(sUb), J(sLb), J(gap));
            b.Append("  \"cycles\": [\n");
            for (int i = 0; i < cycles.Count; i++)
            {
                var c = cycles[i];
                b.AppendFormat("    {{\"n\": {0}, \"connect_ms\": {1}, \"data_ms\": {2}, \"data_total_ms\": {3}, " +
                               "\"teardown_ms\": {4}, \"readv_ub_ms\": {5}, \"readv_lb_ms\": {6}, " +
                               "\"adv_while_connected\": {7}, \"readv\": {8}, \"data\": {9}, \"error\": {10}}}{11}\n",
                               c.N, J(c.ConnectMs), J(c.DataMs), J(c.DataTotalMs), J(c.TeardownMs),
                               J(c.ReadvUbMs), J(c.ReadvLbMs), c.AdvWhileConnected, J(Word(c.Readv)),
                               J(Word(c.Data)), J(c.Error), i + 1 < cycles.Count ? "," : "");
            }
            b.Append("  ]\n}\n");
            File.WriteAllText(o.Json, b.ToString());
            Say("wrote {0}", o.Json);
        }
        return ExitCodeFor(all, o.Strict);
    }

    static string F(double? ms)
    {
        return ms.HasValue ? ms.Value.ToString("F0", CultureInfo.InvariantCulture) + "ms" : "-";
    }
}
