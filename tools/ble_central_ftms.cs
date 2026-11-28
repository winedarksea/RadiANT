// SPDX-License-Identifier: Apache-2.0
//
// ble_central_ftms - `ftms-conformance`: on-the-wire FTMS server checks from
// a real Windows central (plan Phase 5 item 3).
//
// WHAT THIS IS AND IS NOT. It drives a Fitness Machine Service server the way
// a client app does and checks the answers against FTMS v1.0 and the Core
// Spec Supplement (CSS) error codes. Its case list follows the shape of the
// SIG test suite FTMS.TS - discovery and characteristic GGIT, characteristic
// reads and notifications, Control Point procedures (BV) and their error
// handling (BI) - but the case IDs here are this tool's own: they are not
// FTMS.TS test IDs, and passing this is not qualification. It is the check
// that the parts of FTMS this project owns behave on the air the way the
// native_sim CP decision table says they do.
//
// ONE CENTRAL, ONE RUN, IN ORDER. The cases are not independent: the
// unencrypted write must happen before pairing, Request Control before the
// procedures that need it, Reset before the checks that Reset releases
// control. The order below is the dependency order, and a case whose
// precondition failed is reported INCONCLUSIVE or SKIP with the reason
// rather than as a second FAIL caused by the first.
//
// ATT ERROR CODES. FTMS v1.0 SS4.16.1 maps two Control Point failures to ATT
// application errors from CSS Part B SS1.2, not to Response Codes:
// "Client Characteristic Configuration Descriptor Improperly Configured" =
// 0xFD and "Procedure Already In Progress" = 0xFE. (0x80/0x81 are the older
// per-profile codes CSCP/RSCP used; FTMS does not use them.) A write to the
// Control Point without encryption fails in the ATT permission check before
// FTMS sees it: Insufficient Authentication (0x05) on an unbonded link, or
// Insufficient Encryption (0x0F) on a bonded but unencrypted one.
//
// C# 5 only; see ble_central_util.cs.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Foundation;

static partial class BleCentral
{
    const byte OP_REQUEST_CONTROL = 0x00;
    const byte OP_RESET = 0x01;
    const byte OP_SET_TARGET_SPEED = 0x02;
    const byte OP_SET_TARGET_RESISTANCE = 0x04;
    const byte OP_SET_TARGET_POWER = 0x05;
    const byte OP_START_RESUME = 0x07;
    const byte OP_STOP_PAUSE = 0x08;
    const byte OP_RFU = 0x7F; // 0x15..0x7F are RFU in FTMS v1.0 Table 4.15

    const int RES_SUCCESS = 0x01;
    const int RES_NOT_SUPPORTED = 0x02;
    const int RES_INVALID_PARAM = 0x03;
    const int RES_FAILED = 0x04;
    const int RES_NOT_PERMITTED = 0x05;

    const byte STATUS_RESET = 0x01;
    const byte STATUS_TARGET_RESISTANCE_CHANGED = 0x07;
    const byte STATUS_TARGET_POWER_CHANGED = 0x08;

    static string ResultName(int r)
    {
        switch (r)
        {
            case RES_SUCCESS: return "0x01 Success";
            case RES_NOT_SUPPORTED: return "0x02 Op Code Not Supported";
            case RES_INVALID_PARAM: return "0x03 Invalid Parameter";
            case RES_FAILED: return "0x04 Operation Failed";
            case RES_NOT_PERMITTED: return "0x05 Control Not Permitted";
            case -1: return "no response";
            default: return string.Format("0x{0:X2}", r);
        }
    }

    class CaseResult
    {
        public string Id, Title, Detail;
        public Verdict V;
    }

    class ProcResult
    {
        public GattWriteResult W;
        public byte[] Resp;
        public int Result = -1;

        public string Text()
        {
            if (W == null)
            {
                return "not written";
            }
            if (W.Status != GattCommunicationStatus.Success)
            {
                return "write failed: " + Describe(W);
            }
            return ResultName(Result);
        }
    }

    // Everything one FTMS connection needs. Rebuilt after a reconnect.
    class Ftms
    {
        public Link L;
        public GattDeviceService Svc;
        public Dictionary<Guid, GattCharacteristic> Chars = new Dictionary<Guid, GattCharacteristic>();
        public Chan Cp, Status, Data;
    }

    class ConfRun
    {
        public Options O;
        public bool Bike;
        public int CpMs;
        public BluetoothCacheMode Cache;
        public List<CaseResult> Results = new List<CaseResult>();
        public uint Features, Tsf;
        public bool HaveFeatures;

        public void Add(string id, string title, Verdict v, string detail)
        {
            Results.Add(new CaseResult { Id = id, Title = title, V = v, Detail = detail ?? "" });
            Console.WriteLine("{0,-12} {1,-28} {2}{3}", Word(v), id, title,
                              string.IsNullOrEmpty(detail) ? "" : "  -- " + detail);
            Console.Out.Flush();
        }

        // A case body that throws is that case's FAIL, not the run's end: a
        // timeout on one procedure says nothing about the next.
        public async Task Run(string id, string title, Func<Task<Tuple<Verdict, string>>> body)
        {
            Tuple<Verdict, string> r;
            try
            {
                r = await body();
            }
            catch (Exception ex)
            {
                var inner = ex is AggregateException && ex.InnerException != null ? ex.InnerException : ex;
                r = Tuple.Create(Verdict.Fail, "exception: " + inner.Message);
            }
            Add(id, title, r.Item1, r.Item2);
        }
    }

    static Tuple<Verdict, string> R(Verdict v, string fmt, params object[] a)
    {
        return Tuple.Create(v, a.Length == 0 ? fmt : string.Format(fmt, a));
    }

    static async Task<GattWriteResult> WriteCp(Chan cp, byte[] req)
    {
        return await Within(cp.C.WriteValueWithResultAsync(req.AsBuffer(), GattWriteOption.WriteWithResponse),
                            15000, "Control Point write");
    }

    // One Control Point procedure: the write, then the Response Code
    // indication for that op code. The response is matched by op code, so an
    // unrelated late indication is not mistaken for this one's.
    static async Task<ProcResult> Proc(Chan cp, int ms, params byte[] req)
    {
        var pr = new ProcResult();
        int idx = cp.Count;
        pr.W = await WriteCp(cp, req);
        if (pr.W.Status != GattCommunicationStatus.Success)
        {
            return pr;
        }
        byte op = req[0];
        pr.Resp = await cp.Wait(idx, ms, r => r.Length >= 2 && r[0] == 0x80 && r[1] == op);
        pr.Result = CpResult(pr.Resp, op);
        return pr;
    }

    static async Task<byte[]> ReadChar(GattCharacteristic c, BluetoothCacheMode mode)
    {
        var r = await Within(c.ReadValueAsync(mode), 15000, "read " + ShortUuid(c.Uuid));
        if (r.Status != GattCommunicationStatus.Success)
        {
            throw new Exception(string.Format("read {0} failed: {1}", ShortUuid(c.Uuid), r.Status));
        }
        return ReadBuf(r.Value);
    }

    static GattCharacteristic Find(Ftms f, ushort u)
    {
        GattCharacteristic c;
        return f.Chars.TryGetValue(Sig16(u), out c) ? c : null;
    }

    static async Task<Ftms> OpenFtms(ConfRun run, ulong addr)
    {
        var f = new Ftms();
        f.L = await OpenLink(addr, 30000);
        try
        {
            f.Svc = await GetService(f.L, Sig16(UUID_FTMS), run.Cache);
            var all = await Within(f.Svc.GetCharacteristicsAsync(run.Cache), 15000, "FTMS characteristics");
            if (all.Status != GattCommunicationStatus.Success)
            {
                throw new Exception("FTMS characteristic discovery failed: " + all.Status);
            }
            foreach (var c in all.Characteristics)
            {
                f.Chars[c.Uuid] = c;
            }
            return f;
        }
        catch (Exception)
        {
            f.L.Close();
            throw;
        }
    }

    static Chan AttachChan(Ftms f, GattCharacteristic c)
    {
        var ch = new Chan(c);
        ch.Attach();
        f.L.Chans.Add(ch);
        return ch;
    }

    static async Task<int> FtmsConformance(string needle, Options o)
    {
        var run = new ConfRun
        {
            O = o,
            Bike = o.Machine == "bike",
            CpMs = o.CpTimeoutS * 1000,
            // Always uncached in this mode, whatever --uncached says: a
            // conformance run must see the server's real GATT table, not one
            // Windows remembered from an older image.
            Cache = BluetoothCacheMode.Uncached,
        };

        var adv = new AdvTracker(needle);
        string address = "";
        adv.Start();
        try
        {
            address = await FtmsCases(run, adv, needle);
        }
        catch (Exception ex)
        {
            var inner = ex is AggregateException && ex.InnerException != null ? ex.InnerException : ex;
            run.Add("RUN", "run aborted", Verdict.Fail, inner.Message);
        }
        finally
        {
            adv.Stop();
        }

        var verdicts = run.Results.Select(r => r.V).ToList();
        int pass = verdicts.Count(v => v == Verdict.Pass);
        int fail = verdicts.Count(v => v == Verdict.Fail);
        int inc = verdicts.Count(v => v == Verdict.Inconclusive);
        int skip = verdicts.Count(v => v == Verdict.Skip);
        var overall = Worst(verdicts);
        Say("ftms-conformance ({0}): {1} pass, {2} fail, {3} inconclusive, {4} skip -> {5}",
            o.Machine, pass, fail, inc, skip, Word(overall));

        if (o.Json != null)
        {
            var b = new StringBuilder();
            b.Append("{\n");
            b.AppendFormat("  \"mode\": \"ftms-conformance\",\n  \"machine\": {0},\n  \"name\": {1},\n  \"address\": {2},\n",
                           J(o.Machine), J(needle), J(address));
            b.AppendFormat("  \"utc\": {0},\n  \"verdict\": {1},\n  \"cases\": [\n",
                           J(DateTime.UtcNow.ToString("o")), J(Word(overall)));
            for (int i = 0; i < run.Results.Count; i++)
            {
                var r = run.Results[i];
                b.AppendFormat("    {{\"id\": {0}, \"title\": {1}, \"verdict\": {2}, \"detail\": {3}}}{4}\n",
                               J(r.Id), J(r.Title), J(Word(r.V)), J(r.Detail),
                               i + 1 < run.Results.Count ? "," : "");
            }
            b.Append("  ]\n}\n");
            File.WriteAllText(o.Json, b.ToString());
            Say("wrote {0}", o.Json);
        }
        return ExitCodeFor(verdicts, o.Strict);
    }

    // Returns the address used. Throws only for failures that leave nothing
    // to test (no device, no FTMS service).
    static async Task<string> FtmsCases(ConfRun run, AdvTracker adv, string needle)
    {
        var o = run.O;
        bool bike = run.Bike;
        Say("ftms-conformance: machine={0}, looking for '{1}' (up to {2} s)", o.Machine, needle, o.ScanSeconds);
        var first = await adv.WaitWhere(x => x.Name != null &&
                                        x.Name.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0,
                                        o.ScanSeconds * 1000);
        if (first == null)
        {
            throw new Exception("no advertiser named '" + needle + "' within " + o.ScanSeconds +
                                " s (the name is in the scan response; the host scanner surfaces ~1 in 7 packets)");
        }
        ulong addr = first.Addr;
        Say("found '{0}' at {1} (rssi {2})", first.Name, Addr(addr), first.Rssi);

        // ── ADV-SVCDATA ─────────────────────────────────────────────────────
        await run.Run("ADV-SVCDATA", "FTMS Service Data: Available + machine type", async () =>
        {
            var hit = await adv.WaitWhere(x => x.Addr == addr && x.FtmsServiceData != null, 15000);
            string err = CheckServiceData(hit == null ? null : hit.FtmsServiceData, bike);
            return err == "" ? R(Verdict.Pass, "service data {0}", Hex(hit.FtmsServiceData))
                             : R(Verdict.Fail, err);
        });

        // ── bond state before the unencrypted case ──────────────────────────
        bool wasPaired;
        bool unpairedForTest = false;
        {
            var pd = await Within(BluetoothLEDevice.FromBluetoothAddressAsync(addr), 10000, "FromBluetoothAddressAsync");
            if (pd == null)
            {
                throw new Exception("FromBluetoothAddressAsync returned null for " + Addr(addr));
            }
            try
            {
                wasPaired = pd.DeviceInformation.Pairing.IsPaired;
                if (wasPaired && !o.SkipUnencrypted)
                {
                    // Windows encrypts a bonded link on its own, so the only
                    // way to put an unencrypted write on the air is to forget
                    // the bond host-side. The peripheral keeps ITS copy; see
                    // the PAIR case for what that costs.
                    var u = await Within(pd.DeviceInformation.Pairing.UnpairAsync(), 30000, "UnpairAsync");
                    Say("host was bonded; unpair: {0}", u.Status);
                    unpairedForTest = u.Status == DeviceUnpairingResultStatus.Unpaired ||
                                      u.Status == DeviceUnpairingResultStatus.AlreadyUnpaired;
                }
            }
            finally
            {
                pd.Dispose();
            }
        }
        bool unencryptedPossible = !wasPaired || unpairedForTest;
        if (unpairedForTest)
        {
            // Unpairing tears the device record down; give Windows a moment
            // before asking for it again, and take the freshest address.
            await Task.Delay(2000);
            var again = await adv.WaitAfter(DateTime.UtcNow.AddSeconds(-1), 20000);
            if (again != null)
            {
                addr = again.Addr;
            }
        }

        Ftms f = await OpenFtms(run, addr);
        try
        {
            await DiscoveryCases(run, f);

            if (f.Cp == null)
            {
                foreach (var id in new[] { "CP-UNENCRYPTED", "PAIR", "CP-NO-CCCD", "CP-NO-CONTROL" })
                {
                    run.Add(id, "", Verdict.Skip, "no Control Point to test");
                }
                return Addr(addr);
            }

            // ── CP-UNENCRYPTED ──────────────────────────────────────────────
            bool unencGranted = false;
            if (!unencryptedPossible)
            {
                run.Add("CP-UNENCRYPTED", "Control Point write unencrypted is rejected", Verdict.Skip,
                        "host already bonded and --skip-unencrypted given");
            }
            else
            {
                await run.Run("CP-UNENCRYPTED", "Control Point write unencrypted is rejected", async () =>
                {
                    f.Cp.C.ProtectionLevel = GattProtectionLevel.Plain;
                    var cc = await WriteCccd(f.Cp.C, GattClientCharacteristicConfigurationDescriptorValue.Indicate);
                    if (cc.Status != GattCommunicationStatus.Success)
                    {
                        return R(Verdict.Inconclusive, "could not enable indications unencrypted: {0}", Describe(cc));
                    }
                    var w = await WriteCp(f.Cp, new[] { OP_REQUEST_CONTROL });
                    bool pairedNow = f.L.Dev.DeviceInformation.Pairing.IsPaired;
                    if (w.Status == GattCommunicationStatus.ProtocolError && w.ProtocolError.HasValue &&
                        (w.ProtocolError.Value == 0x05 || w.ProtocolError.Value == 0x0F ||
                         w.ProtocolError.Value == 0x08 || w.ProtocolError.Value == 0x0C))
                    {
                        return R(Verdict.Pass, "{0}{1}", Describe(w),
                                 pairedNow ? "; NOTE Windows paired afterwards on its own" : "");
                    }
                    if (w.Status == GattCommunicationStatus.AccessDenied)
                    {
                        return R(Verdict.Pass, "AccessDenied (Windows did not surface the ATT code; " +
                                 "it refused to elevate security itself)");
                    }
                    if (w.Status == GattCommunicationStatus.Success)
                    {
                        if (pairedNow)
                        {
                            return R(Verdict.Inconclusive, "write succeeded, but Windows paired on the " +
                                     "security error and retried, so the write that succeeded was encrypted");
                        }
                        unencGranted = true;
                        return R(Verdict.Fail, "the Control Point ACCEPTED an unencrypted write " +
                                 "(FTMS Table 4.1 requires encryption)");
                    }
                    return R(Verdict.Fail, "unexpected: {0}", Describe(w));
                });
            }

            // ── PAIR ────────────────────────────────────────────────────────
            bool paired = false;
            await run.Run("PAIR", "Just Works pairing with encryption", async () =>
            {
                var pairing = f.L.Dev.DeviceInformation.Pairing;
                if (pairing.IsPaired)
                {
                    paired = true;
                    return R(Verdict.Pass, wasPaired && !unpairedForTest
                                           ? "already bonded before the run (pre-existing bond reused)"
                                           : "already paired (Windows paired during the unencrypted case)");
                }
                var custom = pairing.Custom;
                TypedEventHandler<DeviceInformationCustomPairing, DevicePairingRequestedEventArgs> h = (s, e) =>
                {
                    Say("pairing requested: {0}{1}", e.PairingKind,
                        string.IsNullOrEmpty(e.Pin) ? "" : " pin " + e.Pin);
                    if (e.PairingKind == DevicePairingKinds.ConfirmOnly ||
                        e.PairingKind == DevicePairingKinds.ConfirmPinMatch ||
                        e.PairingKind == DevicePairingKinds.DisplayPin)
                    {
                        e.Accept();
                    }
                };
                custom.PairingRequested += h;
                DevicePairingResult pr;
                try
                {
                    pr = await Within(custom.PairAsync(DevicePairingKinds.ConfirmOnly |
                                                       DevicePairingKinds.ConfirmPinMatch |
                                                       DevicePairingKinds.DisplayPin,
                                                       DevicePairingProtectionLevel.Encryption),
                                      60000, "PairAsync");
                }
                finally
                {
                    custom.PairingRequested -= h;
                }
                if (pr.Status == DevicePairingResultStatus.Paired ||
                    pr.Status == DevicePairingResultStatus.AlreadyPaired)
                {
                    paired = true;
                    return R(Verdict.Pass, "{0}, protection {1}", pr.Status, pr.ProtectionLevelUsed);
                }
                return R(Verdict.Fail, "{0}. Is the pairing window open? If the peripheral still holds a " +
                         "bond for this host from an earlier run, Zephyr refuses an unauthenticated re-pair " +
                         "unless CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE=y: clear its bonds or rerun with " +
                         "--skip-unencrypted while bonded", pr.Status);
            });
            if (!paired)
            {
                foreach (var id in new[] { "CP-NO-CCCD", "CP-NO-CONTROL", "CP-REQUEST-CONTROL" })
                {
                    run.Add(id, "", Verdict.Skip, "not run: no encrypted link (PAIR failed)");
                }
                return Addr(addr);
            }
            // Make every Control Point access insist on the encrypted link,
            // so a lost encryption is a failed write and not a silent
            // fall-back to the unencrypted case.
            f.Cp.C.ProtectionLevel = GattProtectionLevel.EncryptionRequired;

            // ── CP-NO-CCCD ──────────────────────────────────────────────────
            await run.Run("CP-NO-CCCD", "CP write without indications -> ATT 0xFD", async () =>
            {
                var cc = await WriteCccd(f.Cp.C, GattClientCharacteristicConfigurationDescriptorValue.None);
                if (cc.Status != GattCommunicationStatus.Success)
                {
                    return R(Verdict.Inconclusive, "could not clear the CCCD: {0}", Describe(cc));
                }
                var w = await WriteCp(f.Cp, new[] { OP_REQUEST_CONTROL });
                if (IsAttError(w, 0xFD))
                {
                    return R(Verdict.Pass, Describe(w));
                }
                if (w.Status == GattCommunicationStatus.Success)
                {
                    // The server queued a Request Control it can never
                    // answer - and may have granted it.
                    unencGranted = true;
                }
                return R(Verdict.Fail, "{0}, expected ATT 0xFD (CSS Part B 1.2)", Describe(w));
            });

            var sub = await WriteCccd(f.Cp.C, GattClientCharacteristicConfigurationDescriptorValue.Indicate);
            if (sub.Status != GattCommunicationStatus.Success)
            {
                throw new Exception("enabling Control Point indications on the encrypted link failed: " + Describe(sub));
            }

            await ControlCases(run, f, unencGranted);

            // ── disconnect mid-command ──────────────────────────────────────
            f = await DisconnectMidCommand(run, f, adv, addr);
        }
        finally
        {
            if (f != null)
            {
                f.L.Close();
            }
        }

        if (o.UnpairAtEnd)
        {
            var pd = await Within(BluetoothLEDevice.FromBluetoothAddressAsync(addr), 10000, "FromBluetoothAddressAsync");
            if (pd != null)
            {
                var u = await Within(pd.DeviceInformation.Pairing.UnpairAsync(), 30000, "UnpairAsync");
                Say("--unpair-at-end: {0}", u.Status);
                pd.Dispose();
            }
        }
        return Addr(addr);
    }

    // Service, characteristics, reads and notifications: everything that
    // needs no encryption and no control.
    static async Task DiscoveryCases(ConfRun run, Ftms f)
    {
        bool bike = run.Bike;
        ushort dataU = bike ? UUID_INDOOR_BIKE_DATA : UUID_TREADMILL_DATA;

        await run.Run("DISC-SVC", "FTMS service + mandatory characteristics/properties", () =>
        {
            var errs = new List<string>();
            Action<ushort, string, GattCharacteristicProperties[]> need = (u, what, props) =>
            {
                var c = Find(f, u);
                if (c == null)
                {
                    errs.Add(string.Format("{0} (0x{1:X4}) missing", what, u));
                    return;
                }
                foreach (var p in props)
                {
                    if ((c.CharacteristicProperties & p) == 0)
                    {
                        errs.Add(string.Format("{0} lacks {1}", what, p));
                    }
                }
            };
            need(UUID_FTMS_FEATURE, "Fitness Machine Feature", new[] { GattCharacteristicProperties.Read });
            need(dataU, bike ? "Indoor Bike Data" : "Treadmill Data", new[] { GattCharacteristicProperties.Notify });
            need(UUID_FTMS_CP, "Fitness Machine Control Point",
                 new[] { GattCharacteristicProperties.Write, GattCharacteristicProperties.Indicate });
            need(UUID_FTMS_STATUS, "Fitness Machine Status", new[] { GattCharacteristicProperties.Notify });
            if (Find(f, UUID_TRAINING_STATUS) != null &&
                (Find(f, UUID_TRAINING_STATUS).CharacteristicProperties & GattCharacteristicProperties.Read) == 0)
            {
                errs.Add("Training Status lacks Read");
            }
            string other = Find(f, bike ? UUID_TREADMILL_DATA : UUID_INDOOR_BIKE_DATA) != null
                         ? string.Format("; NOTE also exposes {0} Data", bike ? "Treadmill" : "Indoor Bike") : "";
            return Task.FromResult(errs.Count == 0
                ? R(Verdict.Pass, "{0} characteristic(s){1}", f.Chars.Count, other)
                : R(Verdict.Fail, string.Join("; ", errs)));
        });

        var cpC = Find(f, UUID_FTMS_CP);
        if (cpC != null)
        {
            f.Cp = AttachChan(f, cpC);
        }

        await run.Run("FEATURE-READ", "Fitness Machine Feature reads as 8 octets", async () =>
        {
            var c = Find(f, UUID_FTMS_FEATURE);
            if (c == null)
            {
                return R(Verdict.Fail, "characteristic missing");
            }
            var d = await ReadChar(c, run.Cache);
            if (d.Length != 8)
            {
                return R(Verdict.Fail, "{0} octets: {1}", d.Length, Hex(d));
            }
            run.Features = BitConverter.ToUInt32(d, 0);
            run.Tsf = BitConverter.ToUInt32(d, 4);
            run.HaveFeatures = true;
            return R(Verdict.Pass, "features 0x{0:X8}, target settings 0x{1:X8}", run.Features, run.Tsf);
        });

        await run.Run("RANGES", "a sane Supported Range per target-setting bit", async () =>
        {
            if (!run.HaveFeatures)
            {
                return R(Verdict.Skip, "no Feature value to derive the required set from");
            }
            // TSF bit -> range characteristic, signedness of min/max.
            var map = new[]
            {
                Tuple.Create(0, UUID_SPEED_RANGE, false, "speed"),
                Tuple.Create(1, UUID_INCLINE_RANGE, true, "inclination"),
                Tuple.Create(2, UUID_RESISTANCE_RANGE, true, "resistance"),
                Tuple.Create(3, UUID_POWER_RANGE, true, "power"),
            };
            var errs = new List<string>();
            var notes = new List<string>();
            foreach (var m in map)
            {
                bool claimed = (run.Tsf & (1u << m.Item1)) != 0;
                var c = Find(f, m.Item2);
                if (claimed && c == null)
                {
                    errs.Add(string.Format("TSF {0} set but Supported {1} Range (0x{2:X4}) missing",
                                           m.Item1, m.Item4, m.Item2));
                    continue;
                }
                if (c == null)
                {
                    continue;
                }
                var d = await ReadChar(c, run.Cache);
                string bad = CheckRange(d, m.Item3);
                if (bad != "")
                {
                    errs.Add(string.Format("{0} range: {1}", m.Item4, bad));
                }
                else
                {
                    notes.Add(string.Format("{0} {1}", m.Item4, Hex(d)));
                }
                if (!claimed)
                {
                    notes.Add(string.Format("{0} range present without TSF bit {1}", m.Item4, m.Item1));
                }
            }
            if (errs.Count > 0)
            {
                return R(Verdict.Fail, string.Join("; ", errs));
            }
            return R(Verdict.Pass, notes.Count == 0 ? "no target settings claimed" : string.Join("; ", notes));
        });

        await run.Run("TRAINING-STATUS", "Training Status reads (optional characteristic)", async () =>
        {
            var c = Find(f, UUID_TRAINING_STATUS);
            if (c == null)
            {
                return R(Verdict.Skip, "not exposed (optional)");
            }
            var d = await ReadChar(c, run.Cache);
            if (d.Length < 2)
            {
                return R(Verdict.Fail, "{0} octets, expected flags + status", d.Length);
            }
            return R(Verdict.Pass, "flags 0x{0:X2}, status 0x{1:X2}", d[0], d[1]);
        });

        var statusC = Find(f, UUID_FTMS_STATUS);
        if (statusC != null)
        {
            f.Status = AttachChan(f, statusC);
            var cc = await WriteCccd(statusC, GattClientCharacteristicConfigurationDescriptorValue.Notify);
            if (cc.Status != GattCommunicationStatus.Success)
            {
                Say("WARNING: subscribing to Fitness Machine Status failed: {0}", Describe(cc));
            }
        }

        var parsed = new List<DataParse>();
        await run.Run("DATA-NOTIFY", "machine data notifies; every record parses", async () =>
        {
            var c = Find(f, dataU);
            if (c == null)
            {
                return R(Verdict.Fail, "data characteristic missing");
            }
            f.Data = AttachChan(f, c);
            var cc = await WriteCccd(c, GattClientCharacteristicConfigurationDescriptorValue.Notify);
            if (cc.Status != GattCommunicationStatus.Success)
            {
                return R(Verdict.Fail, "subscribe failed: {0}", Describe(cc));
            }
            var firstRec = await f.Data.Wait(0, 5000, null);
            if (firstRec == null)
            {
                return R(Verdict.Fail, "no notification within 5 s of subscribing");
            }
            await Task.Delay(3000);
            var bad = new List<string>();
            foreach (var kv in f.Data.Since(0))
            {
                var p = ParseMachineData(bike, kv.Value);
                parsed.Add(p);
                if (!p.Ok)
                {
                    bad.Add(p.Error + " [" + Hex(kv.Value) + "]");
                }
            }
            if (bad.Count > 0)
            {
                return R(Verdict.Fail, "{0}/{1} record(s) malformed: {2}", bad.Count, parsed.Count, bad[0]);
            }
            return R(Verdict.Pass, "{0} record(s) in ~3 s, e.g. {1}", parsed.Count, Hex(firstRec));
        });

        await run.Run("DATA-FEATURES", "data-record fields agree with Feature bits", () =>
        {
            if (!run.HaveFeatures || parsed.Count == 0 || parsed.Any(p => !p.Ok))
            {
                return Task.FromResult(R(Verdict.Skip, "needs FEATURE-READ and DATA-NOTIFY to pass"));
            }
            int union = parsed.Aggregate(0, (a, p) => a | p.Flags);
            var bad = FeatureFlagMismatches(bike, run.Features, union);
            return Task.FromResult(bad.Count == 0
                ? R(Verdict.Pass, "flags union 0x{0:X4}", union)
                : R(Verdict.Fail, string.Join("; ", bad)));
        });
    }

    // Everything that needs the encrypted, indication-subscribed CP.
    static async Task ControlCases(ConfRun run, Ftms f, bool unencGranted)
    {
        bool bike = run.Bike;
        int ms = run.CpMs;

        await run.Run("CP-NO-CONTROL", "procedure before Request Control -> 0x05", async () =>
        {
            if (unencGranted)
            {
                return R(Verdict.Inconclusive, "an earlier Request Control that should have been refused " +
                         "(CP-UNENCRYPTED or CP-NO-CCCD) was accepted, so control may already be held");
            }
            var r = await Proc(f.Cp, ms, OP_RESET);
            return r.Result == RES_NOT_PERMITTED ? R(Verdict.Pass, r.Text())
                                                 : R(Verdict.Fail, "{0}, expected 0x05", r.Text());
        });

        bool haveControl = false;
        await run.Run("CP-REQUEST-CONTROL", "Request Control -> 0x01 Success", async () =>
        {
            var r = await Proc(f.Cp, ms, OP_REQUEST_CONTROL);
            haveControl = r.Result == RES_SUCCESS;
            if (haveControl && r.Resp.Length != 3)
            {
                return R(Verdict.Fail, "response is {0} octets ({1}); Request Control has no parameter",
                         r.Resp.Length, Hex(r.Resp));
            }
            return haveControl ? R(Verdict.Pass, r.Text()) : R(Verdict.Fail, r.Text());
        });

        await run.Run("CP-UNSUPPORTED-OP", "RFU op code 0x7F -> 0x02", async () =>
        {
            var r = await Proc(f.Cp, ms, OP_RFU);
            return r.Result == RES_NOT_SUPPORTED ? R(Verdict.Pass, r.Text())
                                                 : R(Verdict.Fail, "{0}, expected 0x02", r.Text());
        });

        await run.Run("CP-INVALID-PARAM", "Stop/Pause parameter 0x03 -> 0x03", async () =>
        {
            if (!haveControl)
            {
                return R(Verdict.Skip, "no control");
            }
            var r = await Proc(f.Cp, ms, OP_STOP_PAUSE, 0x03);
            return r.Result == RES_INVALID_PARAM ? R(Verdict.Pass, r.Text())
                                                 : R(Verdict.Fail, "{0}, expected 0x03", r.Text());
        });

        // Two writes issued back to back. WinRT queues them and the second
        // ATT Write Request goes out as soon as the first's Write Response is
        // in - before the first procedure's indication has been confirmed if
        // the server is not faster than one connection event. Whether that
        // race is won is up to the server's timing, hence several attempts
        // and INCONCLUSIVE rather than FAIL when none lands inside.
        await run.Run("CP-IN-PROGRESS", "second write while pending -> ATT 0xFE", async () =>
        {
            if (!haveControl)
            {
                return R(Verdict.Skip, "no control");
            }
            for (int attempt = 1; attempt <= 5; attempt++)
            {
                int idx = f.Cp.Count;
                var t1 = WriteCp(f.Cp, new[] { OP_REQUEST_CONTROL });
                var t2 = WriteCp(f.Cp, new[] { OP_REQUEST_CONTROL });
                await Task.WhenAll(t1, t2);
                await Task.Delay(Math.Min(ms, 3000));
                int answers = f.Cp.Since(idx).Count(r => CpResult(r.Value, OP_REQUEST_CONTROL) >= 0);
                var w1 = t1.Result;
                var w2 = t2.Result;
                if (w1.Status != GattCommunicationStatus.Success)
                {
                    return R(Verdict.Fail, "first write failed: {0}", Describe(w1));
                }
                if (IsAttError(w2, 0xFE))
                {
                    return answers == 1
                        ? R(Verdict.Pass, "{0} on attempt {1}; exactly one response indicated", Describe(w2), attempt)
                        : R(Verdict.Fail, "{0}, but {1} responses were indicated (the rejected write was answered)",
                            Describe(w2), answers);
                }
                if (w2.Status != GattCommunicationStatus.Success)
                {
                    return R(Verdict.Fail, "second write: {0}, expected ATT 0xFE{1}", Describe(w2),
                             IsAttError(w2, 0x80) ? " (0x80 is the CSCP/RSCP code; FTMS uses CSS 0xFE)" : "");
                }
                if (answers != 2)
                {
                    return R(Verdict.Fail, "both writes accepted but {0} response(s) indicated", answers);
                }
            }
            return R(Verdict.Inconclusive, "server finished each procedure before the next write arrived " +
                     "(5 attempts); the busy window was never hit from this host");
        });

        if (!bike)
        {
            await run.Run("CP-STOP-PASSES", "Set Target Speed 0 -> 0x01 (stop needs no auth)", async () =>
            {
                if (!haveControl)
                {
                    return R(Verdict.Skip, "no control");
                }
                var r = await Proc(f.Cp, ms, OP_SET_TARGET_SPEED, 0x00, 0x00);
                return r.Result == RES_SUCCESS ? R(Verdict.Pass, r.Text()) : R(Verdict.Fail, r.Text());
            });

            await run.Run("CP-DENIED-MOTION", "Start / Speed>0 with remote start off -> 0x05", async () =>
            {
                if (!haveControl)
                {
                    return R(Verdict.Skip, "no control");
                }
                string pre = "";
                var ts = Find(f, UUID_TRAINING_STATUS);
                if (ts != null)
                {
                    var d = await ReadChar(ts, BluetoothCacheMode.Uncached);
                    if (d.Length >= 2 && d[1] != 0x01)
                    {
                        pre = string.Format("; NOTE Training Status was 0x{0:X2}, not Idle, before the test", d[1]);
                    }
                }
                await Proc(f.Cp, ms, OP_STOP_PAUSE, 0x01); // to a non-running state; result ignored
                var start = await Proc(f.Cp, ms, OP_START_RESUME);
                var speed = await Proc(f.Cp, ms, OP_SET_TARGET_SPEED, 0xF4, 0x01); // 5.00 km/h
                if (start.Result == RES_SUCCESS || speed.Result == RES_SUCCESS)
                {
                    // Something moved, or would have: put it back before anything else.
                    await Proc(f.Cp, ms, OP_SET_TARGET_SPEED, 0x00, 0x00);
                    await Proc(f.Cp, ms, OP_STOP_PAUSE, 0x01);
                }
                if (start.Result == RES_NOT_PERMITTED && speed.Result == RES_NOT_PERMITTED)
                {
                    return R(Verdict.Pass, "Start {0}, Speed 5.00 km/h {1}{2}", start.Text(), speed.Text(), pre);
                }
                return R(Verdict.Fail, "Start {0}, Speed 5.00 km/h {1}; expected 0x05 for both. Built with " +
                         "TREADMILL_REMOTE_START=y, or remote motion allowed on the console?{2}",
                         start.Text(), speed.Text(), pre);
            });
        }
        else
        {
            await run.Run("CP-SET-TARGET", "Set Target Power/Resistance -> 0x01 + status", async () =>
            {
                if (!haveControl)
                {
                    return R(Verdict.Skip, "no control");
                }
                byte op;
                byte statusOp;
                byte[] req;
                if ((run.Tsf & (1u << 3)) != 0)
                {
                    int w = 100;
                    var rc = Find(f, UUID_POWER_RANGE);
                    if (rc != null)
                    {
                        var d = await ReadChar(rc, run.Cache);
                        if (d.Length == 6)
                        {
                            w = Math.Max((short)(d[0] | d[1] << 8), Math.Min((short)(d[2] | d[3] << 8), w));
                        }
                    }
                    op = OP_SET_TARGET_POWER;
                    statusOp = STATUS_TARGET_POWER_CHANGED;
                    req = new[] { op, (byte)w, (byte)(w >> 8) };
                }
                else if ((run.Tsf & (1u << 2)) != 0)
                {
                    op = OP_SET_TARGET_RESISTANCE;
                    statusOp = STATUS_TARGET_RESISTANCE_CHANGED;
                    req = new byte[] { op, 10 }; // 1.0, unitless, 0.1 resolution (uint8 in v1.0)
                }
                else
                {
                    return R(Verdict.Skip, "neither power (TSF 3) nor resistance (TSF 2) target supported");
                }
                int idxS = f.Status == null ? 0 : f.Status.Count;
                var r = await Proc(f.Cp, ms, req);
                if (r.Result != RES_SUCCESS)
                {
                    return R(Verdict.Fail, "{0} [{1}]", r.Text(), Hex(req));
                }
                var st = f.Status == null ? null
                       : await f.Status.Wait(idxS, 3000, x => x.Length >= 1 && x[0] == statusOp);
                return st != null ? R(Verdict.Pass, "{0}; status {1}", r.Text(), Hex(st))
                                  : R(Verdict.Fail, "{0} but no status 0x{1:X2} to the initiator within 3 s",
                                      r.Text(), statusOp);
            });
            run.Add("CP-DENIED-MOTION", "motion authorization", Verdict.Skip,
                    "a trainer is passive: no motion authorization (plan Phase 4 item 2)");
        }

        // ── Reset: response, status to the initiator, elapsed time, control ─
        int? preElapsed = null;
        if (f.Data != null)
        {
            foreach (var kv in f.Data.Since(0))
            {
                var p = ParseMachineData(bike, kv.Value);
                if (p.Ok && p.Elapsed >= 0)
                {
                    preElapsed = p.Elapsed;
                }
            }
        }
        int idxStatus = f.Status == null ? 0 : f.Status.Count;
        int idxData = f.Data == null ? 0 : f.Data.Count;
        ProcResult reset = null;
        DateTime resetAt = DateTime.UtcNow;
        await run.Run("CP-RESET", "Reset -> 0x01 Success", async () =>
        {
            if (!haveControl)
            {
                return R(Verdict.Skip, "no control");
            }
            reset = await Proc(f.Cp, ms, OP_RESET);
            resetAt = DateTime.UtcNow;
            if (reset.Result == RES_SUCCESS)
            {
                return R(Verdict.Pass, reset.Text());
            }
            // Plan Phase 3 item 4: the incline-to-0 half of a treadmill Reset
            // is a motion command, and a denied half answers 0x04.
            return R(Verdict.Fail, "{0}{1}", reset.Text(),
                     !bike && reset.Result == RES_FAILED
                     ? " - on a treadmill this is what a denied incline-to-0 half looks like; allow remote " +
                       "motion on the console/simulator (or level the deck) and rerun"
                     : "");
        });
        bool resetOk = reset != null && reset.Result == RES_SUCCESS;

        await run.Run("STATUS-TO-INITIATOR", "Reset status 0x01 reaches the initiating client", async () =>
        {
            if (!resetOk)
            {
                return R(Verdict.Skip, "Reset did not succeed");
            }
            if (f.Status == null)
            {
                return R(Verdict.Fail, "no Fitness Machine Status characteristic");
            }
            var st = await f.Status.Wait(idxStatus, 3000, x => x.Length >= 1 && x[0] == STATUS_RESET);
            return st != null ? R(Verdict.Pass, "status {0}", Hex(st))
                              : R(Verdict.Fail, "no Reset (0x01) status notification within 3 s");
        });

        await run.Run("RESET-ELAPSED", "Reset zeros the FTMS elapsed time", async () =>
        {
            if (!resetOk)
            {
                return R(Verdict.Skip, "Reset did not succeed");
            }
            if (f.Data == null)
            {
                return R(Verdict.Skip, "no machine data subscription");
            }
            var end = DateTime.UtcNow.AddSeconds(6);
            while (DateTime.UtcNow < end)
            {
                foreach (var kv in f.Data.Since(idxData))
                {
                    if (kv.Key <= resetAt)
                    {
                        continue;
                    }
                    var p = ParseMachineData(bike, kv.Value);
                    if (!p.Ok || p.Elapsed < 0)
                    {
                        continue;
                    }
                    int limit = (int)Math.Ceiling(Ms(resetAt, kv.Key) / 1000.0) + 2;
                    if (p.Elapsed > limit)
                    {
                        return R(Verdict.Fail, "elapsed {0} s, {1:F1} s after Reset (was {2})", p.Elapsed,
                                 Ms(resetAt, kv.Key) / 1000.0, preElapsed.HasValue ? preElapsed + " s" : "unknown");
                    }
                    if (!preElapsed.HasValue || preElapsed.Value <= limit + 2)
                    {
                        return R(Verdict.Inconclusive, "elapsed {0} s after Reset, but it was only {1} before: " +
                                 "a reset is indistinguishable from a fresh session. Run the machine a while first",
                                 p.Elapsed, preElapsed.HasValue ? preElapsed + " s" : "unknown");
                    }
                    return R(Verdict.Pass, "elapsed {0} s -> {1} s", preElapsed.Value, p.Elapsed);
                }
                await Task.Delay(50);
            }
            bool any = f.Data.Since(0).Any(kv => { var p = ParseMachineData(bike, kv.Value); return p.Ok && p.Elapsed >= 0; });
            return any ? R(Verdict.Fail, "no record carrying Elapsed Time within 6 s of Reset")
                       : R(Verdict.Skip, "the machine data carries no Elapsed Time field");
        });

        await run.Run("RESET-RELEASES-CONTROL", "after Reset, procedure w/o Request Control -> 0x05", async () =>
        {
            if (!resetOk)
            {
                return R(Verdict.Skip, "Reset did not succeed");
            }
            var r = await Proc(f.Cp, ms, OP_RESET);
            return r.Result == RES_NOT_PERMITTED ? R(Verdict.Pass, r.Text())
                                                 : R(Verdict.Fail, "{0}, expected 0x05 (FTMS 4.16.2: Reset revokes control)",
                                                     r.Text());
        });
    }

    // Write a procedure and drop the link before its response, then come back
    // and check (a) nothing from the old procedure arrives on the new link and
    // (b) control did not survive the disconnect. (b) is checked with a
    // procedure that needs control and does NOT first Request Control: with
    // one connection, Zephyr can hand the new link the same bt_conn object,
    // so a stale `controller == conn` would pass a Request Control check
    // while still being a leak.
    //
    // LIMIT: Windows cannot drop a link on demand (ble_central_link.cs), so
    // the old link usually lives long enough for a fast server to answer on
    // it. When that is visible (the response arrived before the handler came
    // off) the no-stale half is INCONCLUSIVE, not PASS. Even a PASS cannot
    // prove the firmware's "client gone before the procedure ran" branch was
    // taken; only the device log can show that.
    static async Task<Ftms> DisconnectMidCommand(ConfRun run, Ftms f, AdvTracker adv, ulong addr)
    {
        int ms = run.CpMs;
        var rc = await Proc(f.Cp, ms, OP_REQUEST_CONTROL);
        if (rc.Result != RES_SUCCESS)
        {
            run.Add("DISCONNECT-NO-STALE", "disconnect mid-command: no stale response", Verdict.Inconclusive,
                    "could not take control first: " + rc.Text());
            run.Add("DISCONNECT-RELEASES-CONTROL", "disconnect mid-command releases control", Verdict.Inconclusive,
                    "could not take control first");
            return f;
        }

        byte[] req = run.Bike
                   ? ((run.Tsf & (1u << 3)) != 0 ? new byte[] { OP_SET_TARGET_POWER, 100, 0 }
                                                 : new byte[] { OP_STOP_PAUSE, 0x01 })
                   : new byte[] { OP_SET_TARGET_SPEED, 0x00, 0x00 };
        var oldCp = f.Cp;
        int idx = oldCp.Count;
        var w = await WriteCp(oldCp, req);
        var dropAt = DateTime.UtcNow;
        f.L.Close();
        // Counted AFTER the release: an indication that landed between the
        // write response and the handler coming off still means the server
        // answered on the old link, and nothing was left to go stale.
        bool answeredFirst = oldCp.Count > idx;
        f = null;
        Say("mid-command disconnect: wrote {0} ({1}), link released{2}", Hex(req), Describe(w),
            answeredFirst ? " - but the response had already arrived" : "");

        var back = await adv.WaitAfter(dropAt, run.O.ReadvTimeoutS * 1000);
        if (back == null)
        {
            run.Add("DISCONNECT-NO-STALE", "disconnect mid-command: no stale response", Verdict.Fail,
                    "the device never advertised again within " + run.O.ReadvTimeoutS + " s");
            run.Add("DISCONNECT-RELEASES-CONTROL", "disconnect mid-command releases control", Verdict.Skip,
                    "no reconnection");
            return null;
        }
        Say("re-advertising {0:F0} ms after release", Ms(dropAt, back.T));

        f = await OpenFtms(run, back.Addr);
        var cpC = Find(f, UUID_FTMS_CP);
        if (cpC == null)
        {
            throw new Exception("Control Point missing after reconnect");
        }
        cpC.ProtectionLevel = GattProtectionLevel.EncryptionRequired;
        f.Cp = AttachChan(f, cpC);
        var sub = await WriteCccd(cpC, GattClientCharacteristicConfigurationDescriptorValue.Indicate);
        if (sub.Status != GattCommunicationStatus.Success)
        {
            throw new Exception("re-subscribing to the Control Point failed: " + Describe(sub));
        }
        await Task.Delay(3000);
        var stale = f.Cp.Since(0);
        if (stale.Count > 0)
        {
            run.Add("DISCONNECT-NO-STALE", "disconnect mid-command: no stale response", Verdict.Fail,
                    string.Format("{0} unsolicited indication(s) on the new link, first {1}",
                                  stale.Count, Hex(stale[0].Value)));
        }
        else if (answeredFirst || w.Status != GattCommunicationStatus.Success)
        {
            run.Add("DISCONNECT-NO-STALE", "disconnect mid-command: no stale response", Verdict.Inconclusive,
                    "nothing stale arrived, but the server answered before the link dropped, so no " +
                    "response was left pending to go stale");
        }
        else
        {
            run.Add("DISCONNECT-NO-STALE", "disconnect mid-command: no stale response", Verdict.Pass,
                    "no indication in 3 s on the new link");
        }

        await run.Run("DISCONNECT-RELEASES-CONTROL", "disconnect mid-command releases control", async () =>
        {
            var r = await Proc(f.Cp, ms, OP_RESET);
            if (r.Result != RES_NOT_PERMITTED)
            {
                return R(Verdict.Fail, "Reset without Request Control: {0}, expected 0x05 - control survived " +
                         "the disconnect", r.Text());
            }
            var again = await Proc(f.Cp, ms, OP_REQUEST_CONTROL);
            return again.Result == RES_SUCCESS
                ? R(Verdict.Pass, "Reset w/o control {0}; Request Control {1}", r.Text(), again.Text())
                : R(Verdict.Fail, "Request Control after reconnect: {0}", again.Text());
        });
        return f;
    }
}
