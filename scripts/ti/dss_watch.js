// SPDX-License-Identifier: Apache-2.0
//
// Sample a few RAM locations out of a RUNNING image twice, and report which
// ones changed. Read-only, and never halts the core.
//
// WHY. "Is the firmware receiving anything?" is not answerable from the host
// end of a silent serial port - an open port that delivers no bytes looks the
// same whether the device never heard the request or heard it and could not
// reply. It IS answerable from the debug port, by watching the receive path's
// own state while the host transmits: if the ring buffer indices or the
// dropped-byte counter move, the bytes are arriving and the fault is on the
// way out. If nothing moves, they are not arriving and the fault is on the way
// in. One reading, and half the search space goes away.
//
// WATCH is a comma-separated list of name=address:bytes, for example
//
//   $env:WATCH = "rx_ring=0x20005aa0:20,rx_dropped=0x20005a88:4"
//   $env:WATCH_GAP = "4000"        # ms between the two samples
//
// Addresses come from the ELF, not from a guess:
//
//   arm-zephyr-eabi-nm -n -S zephyr.elf
//
// NO HALT, AND THE RUN BEFORE THE FIRST SAMPLE IS LOAD-BEARING - see the long
// note in dss_rtt.js. Connecting resets the part, so the first sample must be
// taken after the image has been allowed to re-initialise, and halting this
// rig kills the DAP.

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

function hex8(v) {
    var s = String(java.lang.Integer.toHexString(v & 0xFF));
    while (s.length < 2) { s = "0" + s; }
    return s;
}

var script = ScriptingEnvironment.instance();
script.traceSetConsoleLevel(TraceLevel.INFO);

var ccxml = java.lang.System.getenv("CCXML");
var watch = java.lang.System.getenv("WATCH");
if (ccxml === null || watch === null) {
    script.traceWrite("CCXML and WATCH must both be set. See this file's header.");
    java.lang.System.exit(2);
}

var items = [];
var parts = String(watch).split(",");
for (var i = 0; i < parts.length; i++) {
    var nv = parts[i].split("=");
    var al = nv[1].split(":");
    items.push({ name: nv[0], addr: parseInt(al[0], 16), len: parseInt(al[1], 10) });
}

var debugServer = script.getServer("DebugServer.1");
debugServer.setConfig(ccxml);
var session = debugServer.openSession(".*Cortex_M4_0");
script.setScriptTimeout(120000);
session.target.connect();

var settleEnv = java.lang.System.getenv("WATCH_SETTLE");
var settle = (settleEnv === null) ? 2000 : parseInt(String(settleEnv), 10);
var gapEnv = java.lang.System.getenv("WATCH_GAP");
var gap = (gapEnv === null) ? 4000 : parseInt(String(gapEnv), 10);

script.traceWrite("Running the target " + settle + " ms to settle, then sampling " +
                  "twice " + gap + " ms apart (no halt).");
session.target.runAsynch();
java.lang.Thread.sleep(settle);

function sample() {
    var out = [];
    for (var k = 0; k < items.length; k++) {
        var d = session.memory.readData(Memory.Page.PROGRAM, items[k].addr, 8,
                                        items[k].len, false);
        var s = "";
        for (var j = 0; j < items[k].len; j++) { s += hex8(d[j]) + " "; }
        out.push(s);
    }
    return out;
}

var a = sample();
script.traceWrite("");
script.traceWrite("--- sample 1 ---");
for (var m = 0; m < items.length; m++) {
    script.traceWrite("  " + items[m].name + " = " + a[m]);
}

java.lang.Thread.sleep(gap);
var b = sample();
script.traceWrite("");
script.traceWrite("--- sample 2, " + gap + " ms later ---");
for (var n = 0; n < items.length; n++) {
    script.traceWrite("  " + items[n].name + " = " + b[n]);
}

script.traceWrite("");
var anyChanged = false;
for (var p = 0; p < items.length; p++) {
    var changed = (String(a[p]) !== String(b[p]));
    if (changed) { anyChanged = true; }
    script.traceWrite("  " + items[p].name + ": " + (changed ? "CHANGED" : "unchanged"));
}
script.traceWrite("");
script.traceWrite(anyChanged ? "Something moved."
                             : "Nothing moved in " + gap + " ms.");

session.target.disconnect();
debugServer.stop();
