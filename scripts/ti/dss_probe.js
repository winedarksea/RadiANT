// SPDX-License-Identifier: Apache-2.0
//
// Raw memory probe for a CC13x2/CC26x2 over the XDS110. Read-only.
//
// WHY. scripts/ti/dss_rtt.js reported an uninitialised RTT control block at
// the address the ELF names, with no error - and that single observation is
// consistent with FOUR different situations that need four different fixes:
//
//   1. the image faults before it ever writes to RTT
//   2. the image is fine and simply has not logged
//   3. the address is right but RAM reads back as zeroes for a debug-access
//      reason (wrong page, core held in reset, memory not clocked)
//   4. the image running is not the image whose ELF was consulted
//
// Nothing in "acID is empty" separates those. This does: it reads the flash
// vector table (which proves the image on the part IS the one just built),
// dumps the RAM around the control block, and scans SRAM for the "SEGGER RTT"
// signature wherever it may actually be. Cheap, and it replaces four
// hypotheses with one measurement.
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_probe.js

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var SRAM_BASE = 0x20000000;
var SRAM_LEN  = 80 * 1024;

function hex32(v) {
    var s = String(java.lang.Long.toHexString(v & 0xFFFFFFFF));
    while (s.length < 8) { s = "0" + s; }
    return "0x" + s.toUpperCase();
}

var script = ScriptingEnvironment.instance();
script.traceSetConsoleLevel(TraceLevel.INFO);

var ccxml = java.lang.System.getenv("CCXML");
if (ccxml === null) {
    script.traceWrite("CCXML is not set.");
    java.lang.System.exit(2);
}

var debugServer = script.getServer("DebugServer.1");
debugServer.setConfig(ccxml);
var session = debugServer.openSession(".*Cortex_M4_0");
script.setScriptTimeout(120000);
session.target.connect();

function rd32(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0]
           & 0xFFFFFFFF;
}

script.traceWrite("--- flash vector table -------------------------------------");
script.traceWrite("  0x00000000 initial SP   = " + hex32(rd32(0x00000000)));
script.traceWrite("  0x00000004 reset vector = " + hex32(rd32(0x00000004)));
script.traceWrite("  0x0000000C HardFault    = " + hex32(rd32(0x0000000C)));
script.traceWrite("");

script.traceWrite("--- CCFG (top 88 bytes of a 352 KB flash) ------------------");
script.traceWrite("  BL_CONFIG (0x57FD8)    = " + hex32(rd32(0x57FA8 + 0x30)));
script.traceWrite("");

var cbEnv = java.lang.System.getenv("RTT_CB");
if (cbEnv !== null) {
    var cb = parseInt(String(cbEnv), 16);
    script.traceWrite("--- RAM around the ELF's _SEGGER_RTT " + hex32(cb) + " ----------");
    for (var o = -16; o < 48; o += 4) {
        script.traceWrite("  " + hex32(cb + o) + "  " + hex32(rd32(cb + o)));
    }
    script.traceWrite("");
}

// Scan SRAM for SEGGER's signature. Word-at-a-time would be 20480 round trips;
// readData with a count is one transaction per chunk, which is the difference
// between seconds and many minutes.
script.traceWrite("--- scanning " + (SRAM_LEN / 1024) + " KB of SRAM for \"SEGGER RTT\" ---------");
var CHUNK = 2048;
var found = 0;
for (var base = SRAM_BASE; base < SRAM_BASE + SRAM_LEN; base += CHUNK) {
    var buf = session.memory.readData(Memory.Page.PROGRAM, base, 8, CHUNK, false);
    for (var i = 0; i < CHUNK - 10; i++) {
        if ((buf[i] & 0xFF) === 0x53 && (buf[i + 1] & 0xFF) === 0x45 &&
            (buf[i + 2] & 0xFF) === 0x47 && (buf[i + 3] & 0xFF) === 0x47 &&
            (buf[i + 4] & 0xFF) === 0x45 && (buf[i + 5] & 0xFF) === 0x52) {
            script.traceWrite("  signature at " + hex32(base + i));
            found++;
        }
    }
}
if (found === 0) {
    script.traceWrite("  none. No RTT control block has been initialised anywhere");
    script.traceWrite("  in SRAM, so the image has not executed a single RTT write.");
}

// How much of SRAM is nonzero at all - a blank .data/.bss says the C startup
// never ran, which is a completely different failure from a fault in main().
var nonzero = 0;
for (var b2 = SRAM_BASE; b2 < SRAM_BASE + SRAM_LEN; b2 += CHUNK) {
    var buf2 = session.memory.readData(Memory.Page.PROGRAM, b2, 8, CHUNK, false);
    for (var j = 0; j < CHUNK; j++) {
        if ((buf2[j] & 0xFF) !== 0) { nonzero++; }
    }
}
script.traceWrite("");
script.traceWrite("SRAM nonzero bytes: " + nonzero + " of " + SRAM_LEN +
                  "  (a near-zero count means the C startup never copied .data)");

session.target.disconnect();
debugServer.stop();
