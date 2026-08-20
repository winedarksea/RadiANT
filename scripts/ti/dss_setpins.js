// SPDX-License-Identifier: Apache-2.0
//
// Drive chosen DIOs to chosen levels on a RUNNING CC13x2/CC26x2, then leave
// the target running and walk away. Writes GPIO configuration only.
//
// WHY. This board's RF front end is not identified. The factory image drove
// DIO28/29/30 as outputs, which is what a CC1352P-style antenna switch looks
// like, but "looks like" is not a measurement and getting it wrong yields a
// radio that works badly rather than one that fails - the worst kind of wrong,
// because it looks like distance, or interference, or a marginal sensor.
//
// THE POINT IS THAT NO REBUILD IS NEEDED TO SWEEP IT. radiant's cc26xx backend
// never touches these pins, so whatever this script writes STAYS written while
// the firmware runs. So the loop is: set a combination here, run a scan from
// the host, compare packet counts and RSSI. One flash, eight measurements.
//
// That only holds while the firmware leaves the pins alone. Once the winning
// combination is known it belongs in the board devicetree, and this script
// stops being able to override it - which is the correct end state, not a
// limitation.
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   $env:PINS  = "28=1,29=0,30=0"     # DIO=level, comma separated
//   $env:PINS  = "28=z"               # 'z' returns a pin to high-impedance
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_setpins.js
//
// NOTE THAT CONNECTING RESETS THE BOARD, so the firmware reboots before these
// writes land - which is exactly what is wanted (the pins are set on a freshly
// booted image), but it does mean any host-side session was dropped. Open the
// host tool AFTER this script finishes, not before.

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var IOC_BASE            = 0x40081000;
var GPIO_BASE           = 0x40022000;
var GPIO_O_DOUT31_0     = 0x00000080;
var GPIO_O_DIN31_0      = 0x000000C0;
var GPIO_O_DOE31_0      = 0x000000D0;

// PORT_ID 0 (GPIO) with PULL_CTL = disabled. Same value the factory image had
// on its own GPIO outputs, so this is not inventing a pad configuration.
var IOCFG_GPIO_NOPULL   = 0x00006000;

function hex32(v) {
    var s = String(java.lang.Long.toHexString(v & 0xFFFFFFFF));
    while (s.length < 8) { s = "0" + s; }
    return "0x" + s.toUpperCase();
}

var script = ScriptingEnvironment.instance();
script.traceSetConsoleLevel(TraceLevel.INFO);

var ccxml = java.lang.System.getenv("CCXML");
var pinsEnv = java.lang.System.getenv("PINS");
if (ccxml === null || pinsEnv === null) {
    script.traceWrite("CCXML and PINS must both be set. See this file's header.");
    java.lang.System.exit(2);
}

var wanted = [];
var parts = String(pinsEnv).split(",");
for (var i = 0; i < parts.length; i++) {
    var kv = parts[i].split("=");
    var dio = parseInt(kv[0], 10);
    var lvlStr = String(kv[1]).toLowerCase();
    wanted.push({ dio: dio, hiz: (lvlStr === "z"), level: (lvlStr === "1") ? 1 : 0 });
}

var debugServer = script.getServer("DebugServer.1");
debugServer.setConfig(ccxml);
var session = debugServer.openSession(".*Cortex_M4_0");
script.setScriptTimeout(60000);
session.target.connect();

var settleEnv = java.lang.System.getenv("PINS_SETTLE");
var settle = (settleEnv === null) ? 2500 : parseInt(String(settleEnv), 10);
script.traceWrite("Running the target " + settle + " ms so the image finishes its " +
                  "own init, then writing pins (no halt).");
session.target.runAsynch();
java.lang.Thread.sleep(settle);

function rd(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0]
           & 0xFFFFFFFF;
}
// writeData(page, ADDRESS, VALUE, typeSize) - address second, matching
// readData, and with no trailing boolean. Both details were established by
// being wrong about them: a trailing boolean fails with a Rhino "Can't find
// method" that lists argument types rather than naming the missing overload,
// and swapping value and address does NOT fail cleanly - it reports
// "Error writing memory: Address: 0x6000", the value being used as a
// destination. Neither message says which argument is wrong.
function wr(addr, val) {
    session.memory.writeData(Memory.Page.PROGRAM, addr, val & 0xFFFFFFFF, 32);
}

var doe  = rd(GPIO_BASE + GPIO_O_DOE31_0);
var dout = rd(GPIO_BASE + GPIO_O_DOUT31_0);
script.traceWrite("before: DOE = " + hex32(doe) + "  DOUT = " + hex32(dout));

for (var k = 0; k < wanted.length; k++) {
    var w = wanted[k];
    var bit = (1 << w.dio) >>> 0;

    // Route the pad to GPIO first. If it is currently owned by a peripheral,
    // setting DOE alone would change nothing at all - and the resulting "no
    // effect" reads as evidence that the pin is not the antenna switch, which
    // would be a wrong conclusion drawn from an incomplete write.
    wr(IOC_BASE + w.dio * 4, IOCFG_GPIO_NOPULL);

    if (w.hiz) {
        doe = (doe & ~bit) >>> 0;
    } else {
        doe = (doe | bit) >>> 0;
        dout = w.level ? ((dout | bit) >>> 0) : ((dout & ~bit) >>> 0);
    }
}

// DOUT before DOE, so a pin never drives the wrong level even momentarily.
wr(GPIO_BASE + GPIO_O_DOUT31_0, dout);
wr(GPIO_BASE + GPIO_O_DOE31_0, doe);

var doe2  = rd(GPIO_BASE + GPIO_O_DOE31_0);
var dout2 = rd(GPIO_BASE + GPIO_O_DOUT31_0);
var din2  = rd(GPIO_BASE + GPIO_O_DIN31_0);
script.traceWrite("after:  DOE = " + hex32(doe2) + "  DOUT = " + hex32(dout2));
script.traceWrite("        DIN = " + hex32(din2));
script.traceWrite("");
for (var m = 0; m < wanted.length; m++) {
    var d = wanted[m].dio;
    script.traceWrite("  DIO" + d + ": " +
        (wanted[m].hiz ? "high-Z" : ("driving " + wanted[m].level)) +
        "   (DOE " + ((doe2 >>> d) & 1) + ", reads " + ((din2 >>> d) & 1) + ")");
}

// Leave it RUNNING. A disconnect that left the core halted would make the next
// host-side scan measure a dead radio and blame the pin combination.
session.target.disconnect();
debugServer.stop();
