// SPDX-License-Identifier: Apache-2.0
//
// ble_central - a real BLE central on the bench, using the Windows host radio.
//
// WHY THIS EXISTS, AND WHY IT IS NOT PYTHON LIKE EVERYTHING ELSE IN tools/.
//
// P8b puts a SIG Heart Rate Service advertiser on the strap beside its ANT+
// master, and P9 defines a suppression rule that only takes effect once a BLE
// central actually CONNECTS. Neither claim can be checked from the strap's own
// log: a board cannot observe its own advertising, and "advertising started"
// returning 0 says nothing about what is on the air or whether a phone would
// find it. Every other verification path this project has - the dongle, the
// Python tools - speaks ANT, not BLE.
//
// The Windows machine driving the bench already has a BLE radio. WinRT exposes
// it through BluetoothLEAdvertisementWatcher and BluetoothLEDevice, and those
// are reachable from .NET Framework, whose compiler ships with Windows. So this
// is C# not out of preference but because it is the only path to a live BLE
// central here that does not require flashing another board - and the one spare
// board (the nRF52840 Dongle) has no debugger on it.
//
// Built and run by scripts\ble_central.ps1, which locates csc.exe and the
// Windows metadata. See that script for the exact reference set; getting a
// .NET Framework build to see Windows.winmd is the fiddly part, not this file.
//
// Two modes:
//   scan <seconds>                 - active scan, one line per distinct device
//   connect <name-substring> <sec> - connect, subscribe to Heart Rate
//                                    Measurement, print every notification
//
// The connect mode is the P9 instrument. It holds the connection open for the
// requested duration so the ANT+ side can be watched (on the dongle, or on the
// strap's own log) for the whole window during which suppression should apply.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

static class BleCentral
{
    static DateTime t0 = DateTime.UtcNow;

    static void Say(string fmt, params object[] args)
    {
        // A relative timestamp on every line: the whole point of connect mode
        // is correlating what happens here against a strap log and an ANT+
        // capture taken at the same moment, and wall-clock time on three
        // machines' worth of clocks is harder to line up than elapsed seconds
        // from a marker both sides can see.
        Console.WriteLine("[{0,7:F3}] {1}", (DateTime.UtcNow - t0).TotalSeconds,
                          string.Format(fmt, args));
        Console.Out.Flush();
    }

    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("usage: ble_central scan <seconds>");
            Console.Error.WriteLine("       ble_central connect <name-substring> <seconds>");
            Console.Error.WriteLine("       ble_central hold <name-substring> <seconds>");
            return 2;
        }

        try
        {
            switch (args[0])
            {
                case "scan":
                    return Scan(args.Length > 1 ? int.Parse(args[1]) : 10).Result;
                case "connect":
                    if (args.Length < 2)
                    {
                        Console.Error.WriteLine("connect needs a name substring");
                        return 2;
                    }
                    return Connect(args[1], args.Length > 2 ? int.Parse(args[2]) : 30).Result;
                case "hold":
                    if (args.Length < 2)
                    {
                        Console.Error.WriteLine("hold needs a name substring");
                        return 2;
                    }
                    return Hold(args[1], args.Length > 2 ? int.Parse(args[2]) : 60).Result;
                default:
                    Console.Error.WriteLine("unknown mode '{0}'", args[0]);
                    return 2;
            }
        }
        catch (AggregateException ex)
        {
            Console.Error.WriteLine("failed: {0}", ex.InnerException != null
                                    ? ex.InnerException.Message : ex.Message);
            return 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("failed: {0}", ex.Message);
            return 1;
        }
    }

    // An advertising address is not an identity - a peripheral using a random
    // resolvable address changes it - but within one scan window it is stable
    // enough to collapse the hundreds of packets a busy room produces into one
    // line per device, which is what makes the output readable at all.
    class Seen
    {
        public string Name = "";
        public short Rssi;
        public int Count;
        public List<Guid> Services = new List<Guid>();
    }

    static string Addr(ulong a)
    {
        return string.Join(":", BitConverter.GetBytes(a).Take(6).Reverse()
                                .Select(b => b.ToString("X2")));
    }

    static async Task<int> Scan(int seconds)
    {
        var seen = new Dictionary<ulong, Seen>();
        var watcher = new BluetoothLEAdvertisementWatcher();

        // ACTIVE, not passive. A passive scan never sends a scan request, so
        // any name carried only in the scan RESPONSE is invisible - and a
        // peripherals's name is exactly the field most likely to live there
        // once the 31-byte advertising payload gets tight.
        watcher.ScanningMode = BluetoothLEScanningMode.Active;

        watcher.Received += (w, e) =>
        {
            lock (seen)
            {
                Seen s;
                if (!seen.TryGetValue(e.BluetoothAddress, out s))
                {
                    s = new Seen();
                    seen[e.BluetoothAddress] = s;
                }
                s.Count++;
                s.Rssi = e.RawSignalStrengthInDBm;
                if (!string.IsNullOrEmpty(e.Advertisement.LocalName))
                {
                    s.Name = e.Advertisement.LocalName;
                }
                foreach (var u in e.Advertisement.ServiceUuids)
                {
                    if (!s.Services.Contains(u))
                    {
                        s.Services.Add(u);
                    }
                }
            }
        };

        Say("scanning for {0} s (active)", seconds);
        watcher.Start();
        await Task.Delay(seconds * 1000);
        watcher.Stop();

        lock (seen)
        {
            Say("{0} distinct device(s)", seen.Count);
            foreach (var kv in seen.OrderByDescending(k => k.Value.Rssi))
            {
                Console.WriteLine("  {0}  rssi {1,4}  adv {2,4}  name '{3}'  services [{4}]",
                                  Addr(kv.Key), kv.Value.Rssi, kv.Value.Count,
                                  kv.Value.Name,
                                  string.Join(", ", kv.Value.Services.Select(ShortUuid)));
            }
        }
        return 0;
    }

    // 16-bit SIG UUIDs are what a service list is almost always made of, and
    // printing the full 128-bit base form for them buries the one interesting
    // case (a real vendor UUID) in a wall of identical text.
    static string ShortUuid(Guid g)
    {
        string s = g.ToString();
        if (s.EndsWith("-0000-1000-8000-00805f9b34fb", StringComparison.OrdinalIgnoreCase))
        {
            return "0x" + s.Substring(4, 4).ToUpperInvariant();
        }
        return s;
    }

    static async Task<ulong> FindByName(string needle, int seconds)
    {
        var tcs = new TaskCompletionSource<ulong>();
        var watcher = new BluetoothLEAdvertisementWatcher();
        watcher.ScanningMode = BluetoothLEScanningMode.Active;

        watcher.Received += (w, e) =>
        {
            string name = e.Advertisement.LocalName;
            if (!string.IsNullOrEmpty(name) &&
                name.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0)
            {
                Say("found '{0}' at {1} (rssi {2})", name, Addr(e.BluetoothAddress),
                    e.RawSignalStrengthInDBm);
                tcs.TrySetResult(e.BluetoothAddress);
            }
        };

        Say("looking for a device whose advertised name contains '{0}'", needle);
        watcher.Start();
        var done = await Task.WhenAny(tcs.Task, Task.Delay(seconds * 1000));
        watcher.Stop();

        if (done != (Task)tcs.Task)
        {
            throw new Exception(
                "no advertiser named '" + needle + "' within " + seconds + " s. " +
                "If the peripheral is up, check the name is in the ADVERTISING " +
                "payload and not only in the GAP Device Name characteristic - " +
                "a scanner cannot read the latter without connecting first.");
        }
        return tcs.Task.Result;
    }

    static async Task<int> Connect(string needle, int seconds)
    {
        // Discovery gets its own budget rather than eating into the hold time:
        // a 30 s connection that spent 25 s of it looking for the device is not
        // the 30 s measurement anyone asked for.
        //
        // AND THE BUDGET HAS TO BE LARGE. Measured on this bench against a
        // strap advertising every 1009 ms (confirmed at that rate by a phone
        // running nRF Connect, sitting beside the host): the Windows radio
        // surfaced 18 advertisements in 120 s, about one in seven. The host
        // scanner is duty-cycled and there is no API to change that, so a
        // 15 s discovery window finds this peripheral maybe half the time and
        // "not found" here means almost nothing about whether it is on the
        // air. 90 s is the floor; failure after that is worth believing.
        ulong addr = await FindByName(needle, Math.Max(90, seconds));

        var dev = await BluetoothLEDevice.FromBluetoothAddressAsync(addr);
        if (dev == null)
        {
            throw new Exception("FromBluetoothAddressAsync returned null for " + Addr(addr));
        }

        dev.ConnectionStatusChanged += (d, o) => Say("connection status: {0}", d.ConnectionStatus);

        var svc = await dev.GetGattServicesForUuidAsync(GattServiceUuids.HeartRate,
                                                        BluetoothCacheMode.Uncached);
        if (svc.Status != GattCommunicationStatus.Success || svc.Services.Count == 0)
        {
            throw new Exception("no Heart Rate service (0x180D): " + svc.Status);
        }
        Say("Heart Rate service found; GATT is up, so the link is established");

        var chr = await svc.Services[0].GetCharacteristicsForUuidAsync(
            GattCharacteristicUuids.HeartRateMeasurement, BluetoothCacheMode.Uncached);
        if (chr.Status != GattCommunicationStatus.Success || chr.Characteristics.Count == 0)
        {
            throw new Exception("no Heart Rate Measurement characteristic (0x2A37): " + chr.Status);
        }

        int notifications = 0;
        var c = chr.Characteristics[0];
        c.ValueChanged += (ch, e) =>
        {
            var reader = DataReader.FromBuffer(e.CharacteristicValue);
            var raw = new byte[e.CharacteristicValue.Length];
            reader.ReadBytes(raw);

            // Flags bit 0 selects a uint16 value; anything else in the packet
            // (energy, RR intervals) is not what this instrument is for.
            int bpm = raw.Length >= 2
                    ? ((raw[0] & 0x01) != 0 && raw.Length >= 3
                       ? raw[1] | (raw[2] << 8) : raw[1])
                    : -1;
            Interlocked.Increment(ref notifications);
            Say("HRS notify: {0} bpm  (raw {1})", bpm,
                BitConverter.ToString(raw).Replace("-", " "));
        };

        var cccd = await c.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue.Notify);
        if (cccd != GattCommunicationStatus.Success)
        {
            throw new Exception("subscribing to notifications failed: " + cccd);
        }
        Say("subscribed; holding the connection for {0} s", seconds);

        await Task.Delay(seconds * 1000);

        Say("{0} notification(s) in {1} s; disconnecting", notifications, seconds);
        c.Service.Dispose();
        dev.Dispose();

        // A held connection that produced no notifications is a real result,
        // not a tool failure - it is what a strap that has stopped publishing
        // looks like - so it is reported and not turned into a nonzero exit.
        return 0;
    }

    // A 16-bit SIG UUID in its 128-bit form. The base is fixed by the Bluetooth
    // core spec; writing it out is what lets this file name FTMS UUIDs that
    // GattServiceUuids/GattCharacteristicUuids have no members for.
    static Guid Sig16(ushort id)
    {
        return new Guid(string.Format("0000{0:X4}-0000-1000-8000-00805F9B34FB", id));
    }

    const ushort UUID_FTMS = 0x1826;
    const ushort UUID_TREADMILL_DATA = 0x2ACD;

    // ── hold ────────────────────────────────────────────────────────────────
    //
    // WHY THIS EXISTS SEPARATELY FROM connect. `connect` is the P9 instrument
    // and it is HEART-RATE SPECIFIC: it throws "no Heart Rate service (0x180D)"
    // and drops the link within seconds against anything that is not a strap.
    // That made it useless for the one measurement apps/treadmill most needs -
    // FE-C loss while a client is CONNECTED over FTMS - because the connection
    // it was supposed to hold did not survive the service check. Found on the
    // bench 2026-08-17 against `RadiANT Treadmill`, and it is why
    // docs/treadmill-reference-design.md SS7.6 had no number.
    //
    // So: connect, force GATT discovery, subscribe to FTMS Treadmill Data if it
    // is there, and HOLD. Subscribing rather than sitting idle is deliberate -
    // an idle connection and a notifying one are different radio loads, and
    // SS7.6 is about the notifying one.
    static async Task<int> Hold(string needle, int seconds)
    {
        ulong addr = await FindByName(needle, Math.Max(90, seconds));

        var dev = await BluetoothLEDevice.FromBluetoothAddressAsync(addr);
        if (dev == null)
        {
            throw new Exception("FromBluetoothAddressAsync returned null for " + Addr(addr));
        }
        dev.ConnectionStatusChanged += (d, o) => Say("connection status: {0}", d.ConnectionStatus);

        // Uncached, and this is what actually establishes the link: a
        // BluetoothLEDevice on its own does not connect, the first GATT
        // operation does.
        var all = await dev.GetGattServicesAsync(BluetoothCacheMode.Uncached);
        if (all.Status != GattCommunicationStatus.Success)
        {
            throw new Exception("service discovery failed: " + all.Status);
        }
        Say("GATT up: {0} service(s)", all.Services.Count);
        foreach (var s in all.Services)
        {
            Say("  service {0}", s.Uuid);
        }

        int notifications = 0;
        GattCharacteristic td = null;
        var ftms = await dev.GetGattServicesForUuidAsync(Sig16(UUID_FTMS),
                                                        BluetoothCacheMode.Uncached);
        if (ftms.Status == GattCommunicationStatus.Success && ftms.Services.Count > 0)
        {
            var chr = await ftms.Services[0].GetCharacteristicsForUuidAsync(
                Sig16(UUID_TREADMILL_DATA), BluetoothCacheMode.Uncached);
            if (chr.Status == GattCommunicationStatus.Success && chr.Characteristics.Count > 0)
            {
                td = chr.Characteristics[0];
                td.ValueChanged += (ch, e) =>
                {
                    Interlocked.Increment(ref notifications);
                };
                var cccd = await td.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue.Notify);
                if (cccd != GattCommunicationStatus.Success)
                {
                    Say("WARNING: subscribing to Treadmill Data failed: {0} - holding an "
                        + "IDLE connection instead, which is a different radio load", cccd);
                    td = null;
                }
                else
                {
                    Say("subscribed to FTMS Treadmill Data (0x2ACD)");
                }
            }
        }
        if (td == null)
        {
            Say("no FTMS Treadmill Data to subscribe to; holding an idle connection");
        }

        Say("holding the connection for {0} s", seconds);
        // Report progress so a ten-minute run is distinguishable from a hang,
        // and so a DROPPED connection is visible in the transcript rather than
        // only in the final count.
        for (int elapsed = 0; elapsed < seconds; elapsed += 30)
        {
            await Task.Delay(Math.Min(30, seconds - elapsed) * 1000);
            Say("t+{0}s  status={1}  notifications={2}",
                Math.Min(elapsed + 30, seconds), dev.ConnectionStatus, notifications);
        }

        Say("{0} notification(s) in {1} s; disconnecting", notifications, seconds);
        if (td != null)
        {
            td.Service.Dispose();
        }
        foreach (var s in all.Services)
        {
            s.Dispose();
        }
        dev.Dispose();
        return 0;
    }
}
