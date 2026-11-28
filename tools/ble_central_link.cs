// SPDX-License-Identifier: Apache-2.0
//
// ble_central_link - the connection plumbing shared by ftms-conformance and
// reconnect-soak: a continuous advertisement tracker, a connection that can be
// torn down for real, and a subscribed characteristic that records what it
// delivered with timestamps.
//
// WINDOWS HAS NO "DISCONNECT" CALL. That one fact shapes this file.
//
// A WinRT central never connects or disconnects explicitly. The OS connects
// when something needs the link (a GATT operation, or a GattSession with
// MaintainConnection = true) and drops it some time after the last thing
// holding it lets go. "Things holding it" are every BluetoothLEDevice,
// GattDeviceService and GattSession object in every process - including the
// runtime-callable wrappers .NET has not finalised yet, and including other
// apps (Zwift, nRF Connect for Desktop, the Settings Bluetooth page). So a
// real disconnect here is:
//
//   1. unhook every event handler (a live delegate keeps the RCW reachable),
//   2. GattSession.MaintainConnection = false,
//   3. Dispose() every GattDeviceService, the GattSession, the
//      BluetoothLEDevice (Dispose is IClosable.Close: it releases the WinRT
//      object now rather than at finalisation),
//   4. GC.Collect + WaitForPendingFinalizers, for any wrapper that escaped,
//
// and then CONFIRMING it from outside, because Windows still closes the link
// lazily (observed elsewhere as anything from immediate to several seconds).
// Confirmation is two independent signals, see reconnect-soak: a fresh status
// probe object reading ConnectionStatus, and - the one that does not depend on
// Windows at all - the peripheral advertising again, which a single-connection
// peripheral only does once its link is gone.
//
// C# 5 only; see ble_central_util.cs.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Foundation;
using Windows.Storage.Streams;

static partial class BleCentral
{
    const ushort UUID_HRS = 0x180D;
    const ushort UUID_HRM = 0x2A37;
    const ushort UUID_FTMS_FEATURE = 0x2ACC;
    const ushort UUID_INDOOR_BIKE_DATA = 0x2AD2;
    const ushort UUID_TRAINING_STATUS = 0x2AD3;
    const ushort UUID_SPEED_RANGE = 0x2AD4;
    const ushort UUID_INCLINE_RANGE = 0x2AD5;
    const ushort UUID_RESISTANCE_RANGE = 0x2AD6;
    const ushort UUID_POWER_RANGE = 0x2AD8;
    const ushort UUID_FTMS_CP = 0x2AD9;
    const ushort UUID_FTMS_STATUS = 0x2ADA;

    static void ResolveNotify(string spec, out Guid svc, out Guid chr, out string label)
    {
        switch (spec)
        {
            case "hrs":
                svc = Sig16(UUID_HRS); chr = Sig16(UUID_HRM);
                label = "Heart Rate Measurement (0x180D/0x2A37)";
                return;
            case "treadmill":
                svc = Sig16(UUID_FTMS); chr = Sig16(UUID_TREADMILL_DATA);
                label = "FTMS Treadmill Data (0x1826/0x2ACD)";
                return;
            case "bike":
                svc = Sig16(UUID_FTMS); chr = Sig16(UUID_INDOOR_BIKE_DATA);
                label = "FTMS Indoor Bike Data (0x1826/0x2AD2)";
                return;
        }
        var parts = spec.Split(':');
        ushort s16, c16;
        if (parts.Length != 2 ||
            !ushort.TryParse(parts[0], System.Globalization.NumberStyles.HexNumber, null, out s16) ||
            !ushort.TryParse(parts[1], System.Globalization.NumberStyles.HexNumber, null, out c16))
        {
            throw new ArgumentException("--notify is hrs, treadmill, bike or SSSS:CCCC (16-bit hex UUIDs)");
        }
        svc = Sig16(s16);
        chr = Sig16(c16);
        label = string.Format("0x{0:X4}/0x{1:X4}", s16, c16);
    }

    // WinRT calls can hang for tens of seconds against a peripheral that went
    // away mid-operation. Every awaited call in the new modes goes through
    // this so a stuck operation becomes a failed case, not a hung soak.
    static async Task<T> Within<T>(IAsyncOperation<T> op, int ms, string what)
    {
        var t = op.AsTask();
        if (await Task.WhenAny(t, Task.Delay(ms)) != t)
        {
            try { op.Cancel(); } catch (Exception) { }
            throw new TimeoutException(what + " did not complete within " + ms + " ms");
        }
        return t.Result;
    }

    static byte[] ReadBuf(IBuffer b)
    {
        var raw = new byte[b.Length];
        DataReader.FromBuffer(b).ReadBytes(raw);
        return raw;
    }

    static double Ms(DateTime a, DateTime b)
    {
        return (b - a).TotalMilliseconds;
    }

    // ── advertisement tracker ───────────────────────────────────────────────

    class AdvEvent
    {
        public DateTime T;
        public ulong Addr;
        public string Name;
        public short Rssi;
        public byte[] FtmsServiceData; // AD 0x16 payload starting at the UUID
        public BluetoothLEAdvertisementType Type;
    }

    // One watcher for the whole run, never restarted between cycles: a
    // watcher started after the disconnect would add its own start-up latency
    // to every re-advertise figure. Matching is by name OR by any address the
    // name has been seen at, because the name lives in the SCAN RESPONSE on
    // both apps (trap 7 in treadmill_ble.c) and an ADV_IND alone carries none.
    class AdvTracker
    {
        readonly string needle;
        readonly List<AdvEvent> events = new List<AdvEvent>();
        readonly HashSet<ulong> known = new HashSet<ulong>();
        BluetoothLEAdvertisementWatcher watcher;

        public AdvTracker(string needle)
        {
            this.needle = needle;
        }

        public void Start()
        {
            watcher = new BluetoothLEAdvertisementWatcher();
            watcher.ScanningMode = BluetoothLEScanningMode.Active;
            watcher.Received += OnReceived;
            watcher.Stopped += (w, e) =>
            {
                if (e.Error != BluetoothError.Success)
                {
                    Say("WARNING: advertisement watcher stopped: {0}", e.Error);
                }
            };
            watcher.Start();
        }

        public void Stop()
        {
            if (watcher != null)
            {
                watcher.Received -= OnReceived;
                watcher.Stop();
                watcher = null;
            }
        }

        void OnReceived(BluetoothLEAdvertisementWatcher w, BluetoothLEAdvertisementReceivedEventArgs e)
        {
            string name = e.Advertisement.LocalName;
            bool nameHit = !string.IsNullOrEmpty(name) &&
                           name.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0;
            byte[] sd = null;
            foreach (var sec in e.Advertisement.DataSections)
            {
                if (sec.DataType == 0x16)
                {
                    var d = ReadBuf(sec.Data);
                    if (d.Length >= 2 && d[0] == 0x26 && d[1] == 0x18)
                    {
                        sd = d;
                    }
                }
            }
            lock (events)
            {
                if (nameHit)
                {
                    known.Add(e.BluetoothAddress);
                }
                if (nameHit || known.Contains(e.BluetoothAddress))
                {
                    events.Add(new AdvEvent
                    {
                        T = DateTime.UtcNow,
                        Addr = e.BluetoothAddress,
                        Name = name,
                        Rssi = e.RawSignalStrengthInDBm,
                        FtmsServiceData = sd,
                        Type = e.AdvertisementType,
                    });
                }
            }
        }

        public AdvEvent FirstWhere(Func<AdvEvent, bool> pred)
        {
            lock (events)
            {
                return events.FirstOrDefault(pred);
            }
        }

        public async Task<AdvEvent> WaitWhere(Func<AdvEvent, bool> pred, int ms)
        {
            var end = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < end)
            {
                var hit = FirstWhere(pred);
                if (hit != null)
                {
                    return hit;
                }
                await Task.Delay(10);
            }
            return FirstWhere(pred);
        }

        // The first sighting strictly after `t` - the re-advertise detector.
        public Task<AdvEvent> WaitAfter(DateTime t, int ms)
        {
            return WaitWhere(x => x.T > t, ms);
        }

        public List<AdvEvent> Between(DateTime a, DateTime b)
        {
            lock (events)
            {
                return events.Where(x => x.T > a && x.T <= b).ToList();
            }
        }

        public List<DateTime> Times()
        {
            lock (events)
            {
                return events.Select(x => x.T).ToList();
            }
        }
    }

    // ── a connection that can be torn down for real ─────────────────────────

    class Link
    {
        public BluetoothLEDevice Dev;
        public GattSession Session;
        public readonly List<GattDeviceService> Services = new List<GattDeviceService>();
        public readonly List<Chan> Chans = new List<Chan>();
        public DateTime? ConnectedAt;
        public DateTime? DisconnectedAt;
        TypedEventHandler<BluetoothLEDevice, object> statusHandler;

        public void Hook()
        {
            statusHandler = (d, o) =>
            {
                var st = d.ConnectionStatus;
                if (st == BluetoothConnectionStatus.Connected && !ConnectedAt.HasValue)
                {
                    ConnectedAt = DateTime.UtcNow;
                }
                if (st == BluetoothConnectionStatus.Disconnected && ConnectedAt.HasValue)
                {
                    DisconnectedAt = DateTime.UtcNow;
                }
            };
            Dev.ConnectionStatusChanged += statusHandler;
        }

        // Steps 1-4 of the file comment. Never throws: teardown runs on every
        // path, including after a failure that left half of this null.
        public void Close()
        {
            foreach (var c in Chans)
            {
                c.Detach();
            }
            Chans.Clear();
            try
            {
                if (Dev != null && statusHandler != null)
                {
                    Dev.ConnectionStatusChanged -= statusHandler;
                }
            }
            catch (Exception) { }
            try
            {
                if (Session != null)
                {
                    Session.MaintainConnection = false;
                }
            }
            catch (Exception) { }
            foreach (var s in Services)
            {
                try { s.Dispose(); } catch (Exception) { }
            }
            Services.Clear();
            try { if (Session != null) Session.Dispose(); } catch (Exception) { }
            try { if (Dev != null) Dev.Dispose(); } catch (Exception) { }
            Session = null;
            Dev = null;
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
        }
    }

    // Connect by address. MaintainConnection = true makes Windows open the
    // link now instead of at the first GATT operation, so the connect time is
    // the link's and not a discovery round trip's; ConnectedAt comes from the
    // ConnectionStatusChanged event hooked BEFORE that, so it cannot be missed.
    static async Task<Link> OpenLink(ulong addr, int timeoutMs)
    {
        var l = new Link();
        try
        {
            l.Dev = await Within(BluetoothLEDevice.FromBluetoothAddressAsync(addr), 10000,
                                 "FromBluetoothAddressAsync");
            if (l.Dev == null)
            {
                throw new Exception("FromBluetoothAddressAsync returned null for " + Addr(addr));
            }
            l.Hook();
            l.Session = await Within(GattSession.FromDeviceIdAsync(l.Dev.BluetoothDeviceId), 10000,
                                     "GattSession.FromDeviceIdAsync");
            l.Session.MaintainConnection = true;
            var end = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (l.Dev.ConnectionStatus != BluetoothConnectionStatus.Connected && DateTime.UtcNow < end)
            {
                await Task.Delay(10);
            }
            if (l.Dev.ConnectionStatus != BluetoothConnectionStatus.Connected)
            {
                throw new TimeoutException("no connection within " + timeoutMs + " ms");
            }
            if (!l.ConnectedAt.HasValue)
            {
                // Status was already Connected when the handler was hooked:
                // another holder had the link open. Recorded late rather than
                // invented; the soak flags such cycles via its own checks.
                l.ConnectedAt = DateTime.UtcNow;
            }
            return l;
        }
        catch (Exception)
        {
            l.Close();
            throw;
        }
    }

    static async Task<GattDeviceService> GetService(Link l, Guid uuid, BluetoothCacheMode mode)
    {
        var r = await Within(l.Dev.GetGattServicesForUuidAsync(uuid, mode), 15000, "service discovery");
        if (r.Status != GattCommunicationStatus.Success || r.Services.Count == 0)
        {
            throw new Exception(string.Format("no service {0}: {1}", ShortUuid(uuid), r.Status));
        }
        foreach (var s in r.Services.Skip(1))
        {
            s.Dispose();
        }
        l.Services.Add(r.Services[0]);
        return r.Services[0];
    }

    static async Task<GattCharacteristic> GetChar(GattDeviceService s, Guid uuid, BluetoothCacheMode mode)
    {
        var r = await Within(s.GetCharacteristicsForUuidAsync(uuid, mode), 15000, "characteristic discovery");
        if (r.Status != GattCommunicationStatus.Success || r.Characteristics.Count == 0)
        {
            throw new Exception(string.Format("no characteristic {0}: {1}", ShortUuid(uuid), r.Status));
        }
        return r.Characteristics[0];
    }

    // ── a characteristic and everything it delivered ────────────────────────

    class Chan
    {
        public readonly GattCharacteristic C;
        readonly List<KeyValuePair<DateTime, byte[]>> got = new List<KeyValuePair<DateTime, byte[]>>();
        TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs> handler;

        public Chan(GattCharacteristic c)
        {
            C = c;
        }

        // Attach BEFORE writing the CCCD: a bonded peer's CCCD may already be
        // set from the last connection, and anything it sends before our own
        // subscribe would otherwise be invisible - exactly the stale response
        // the disconnect-mid-command case is looking for.
        public void Attach()
        {
            handler = (ch, e) =>
            {
                var raw = ReadBuf(e.CharacteristicValue);
                lock (got)
                {
                    got.Add(new KeyValuePair<DateTime, byte[]>(DateTime.UtcNow, raw));
                }
            };
            C.ValueChanged += handler;
        }

        public void Detach()
        {
            if (handler != null)
            {
                try { C.ValueChanged -= handler; } catch (Exception) { }
                handler = null;
            }
        }

        public int Count
        {
            get { lock (got) { return got.Count; } }
        }

        public KeyValuePair<DateTime, byte[]> At(int i)
        {
            lock (got) { return got[i]; }
        }

        public List<KeyValuePair<DateTime, byte[]>> Since(int i)
        {
            lock (got) { return got.Skip(i).ToList(); }
        }

        // The first value at index >= `from` that satisfies `pred`, waiting up
        // to `ms`. Returns null on timeout.
        public async Task<byte[]> Wait(int from, int ms, Func<byte[], bool> pred)
        {
            var end = DateTime.UtcNow.AddMilliseconds(ms);
            while (true)
            {
                lock (got)
                {
                    for (int i = from; i < got.Count; i++)
                    {
                        if (pred == null || pred(got[i].Value))
                        {
                            return got[i].Value;
                        }
                    }
                }
                if (DateTime.UtcNow >= end)
                {
                    return null;
                }
                await Task.Delay(10);
            }
        }
    }

    static async Task<GattWriteResult> WriteCccd(GattCharacteristic c,
                                                 GattClientCharacteristicConfigurationDescriptorValue v)
    {
        return await Within(c.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(v), 15000,
                            "CCCD write");
    }

    static string Describe(GattWriteResult r)
    {
        if (r == null)
        {
            return "no result";
        }
        if (r.Status == GattCommunicationStatus.ProtocolError && r.ProtocolError.HasValue)
        {
            return string.Format("ATT error 0x{0:X2}{1}", r.ProtocolError.Value, AttErrorName(r.ProtocolError.Value));
        }
        return r.Status.ToString();
    }

    static string AttErrorName(byte e)
    {
        switch (e)
        {
            case 0x03: return " (Write Not Permitted)";
            case 0x05: return " (Insufficient Authentication)";
            case 0x08: return " (Insufficient Authorization)";
            case 0x0C: return " (Insufficient Encryption Key Size)";
            case 0x0D: return " (Invalid Attribute Value Length)";
            case 0x0F: return " (Insufficient Encryption)";
            case 0x80: return " (application error 0x80)";
            case 0xFC: return " (Write Request Rejected)";
            case 0xFD: return " (CCCD Improperly Configured)";
            case 0xFE: return " (Procedure Already In Progress)";
            case 0xFF: return " (Out of Range)";
            default: return "";
        }
    }

    static bool IsAttError(GattWriteResult r, byte code)
    {
        return r != null && r.Status == GattCommunicationStatus.ProtocolError &&
               r.ProtocolError.HasValue && r.ProtocolError.Value == code;
    }
}
