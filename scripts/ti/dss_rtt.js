// SPDX-License-Identifier: Apache-2.0
//
// Read a Zephyr image's RTT log out of a CC13x2/CC26x2 over the XDS110.
// Read-only unless RTT_RESET is set.
//
// WHY THIS EXISTS. The CC2652P dongle is a sealed USB stick with exactly one
// wired serial port, and apps/dongle_ti gives that port to the ANT wire
// protocol - nothing else may appear in it. Its second UART goes to pads
// inside the shell. So the firmware's log has nowhere to go but RAM, and
// something has to read RAM.
//
// AND THE USUAL OBJECTION TO RTT DOES NOT APPLY HERE, which is the whole
// point. apps/dongle_ti/boards/cc26x2r1_launchxl.conf turns RTT off with the
// reasoning "SEGGER's RTT needs a J-Link and this bench's TI probe is an
// XDS110". A J-Link is needed to read RTT WITH SEGGER'S TOOLS. RTT itself is a
// ring buffer in RAM with a known layout; any debugger that can read memory
// can read it, and the XDS110 reads memory perfectly well - that is how this
// board's part number, CCFG and entire pin map were recovered.
//
// THE ADDRESS IS PASSED IN, NOT SEARCHED FOR. Scanning 80 KB for the
// "SEGGER RTT" signature works and is what SEGGER's own tools do, but the
// address is sitting in the ELF we just flashed, exact and free:
//
//   arm-zephyr-eabi-nm zephyr.elf | Select-String _SEGGER_RTT
//
// scripts/ti/rtt_dump.ps1 does that lookup and sets the environment. A search
// would also happily find a STALE control block from a previous image if the
// new one had not initialised yet, and report an old boot as a new one.
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   $env:RTT_CB = "0x20000410"
//   $env:RTT_RESET = "1"      # optional: reset and capture a FRESH boot
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_rtt.js

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

// SEGGER_RTT_CB: char acID[16], int MaxNumUpBuffers, int MaxNumDownBuffers,
// then the up-buffer descriptors. Each SEGGER_RTT_BUFFER_UP is
// { const char *sName; char *pBuffer; unsigned SizeOfBuffer; unsigned WrOff;
//   unsigned RdOff; unsigned Flags; } - 24 bytes, no padding on this ABI.
var CB_ACID_LEN      = 16;
var CB_UP_ARRAY_OFF  = 24;
var UP_PBUFFER_OFF   = 4;
var UP_SIZE_OFF      = 8;
var UP_WROFF_OFF     = 12;
var UP_RDOFF_OFF     = 16;

function hex32(v) {
    var s = String(java.lang.Long.toHexString(v & 0xFFFFFFFF));
    while (s.length < 8) { s = "0" + s; }
    return "0x" + s.toUpperCase();
}

var script = ScriptingEnvironment.instance();
script.traceSetConsoleLevel(TraceLevel.INFO);

var ccxml = java.lang.System.getenv("CCXML");
var cbEnv = java.lang.System.getenv("RTT_CB");
if (ccxml === null || cbEnv === null) {
    script.traceWrite("CCXML and RTT_CB must both be set. See this file's header.");
    java.lang.System.exit(2);
}
var cb = parseInt(String(cbEnv), 16);

var debugServer = script.getServer("DebugServer.1");
debugServer.setConfig(ccxml);
var session = debugServer.openSession(".*Cortex_M4_0");
script.setScriptTimeout(60000);
session.target.connect();

// LET IT RUN, ALWAYS, AND DO NOT HALT IT AFTERWARDS. Both halves of that were
// learned the expensive way on this board and neither is optional.
//
// CONNECTING RESETS THE PART. The device's GEL runs a board reset on connect,
// and on CC13x2/CC26x2 that cycles the MCU power domain - which CLEARS SRAM.
// So a script that connects and immediately reads the RTT buffer reads 80 KB
// of zeroes every single time, no matter how healthy the firmware is, and
// reports "the image has not executed a single RTT write". That was measured
// here with scripts/ti/dss_probe.js: SRAM nonzero bytes, 0 of 81920, on an
// image that was in fact fine. It is the most convincing false negative this
// bench has produced.
//
// AND THE HALT AFTERWARDS IS WHAT BREAKS THE DEBUG LINK. An explicit halt()
// on this rig returns "Unable to access the DAP ... try more reliable JTAG
// settings" twenty times and then fails the disconnect - while `dslite
// --list-cores` still enumerates the core happily, because the ICEPick scan
// does not need the DAP. That message names the probe, the wiring and the
// clock, and it is none of them. Memory reads on a RUNNING Cortex-M go through
// the AHB-AP and need no halt at all, so the halt buys nothing and costs the
// session.
var settle = java.lang.System.getenv("RTT_SETTLE");
var ms = (settle === null) ? 3000 : parseInt(String(settle), 10);
script.traceWrite("Running the target for " + ms + " ms, then reading (no halt)...");
session.target.runAsynch();
java.lang.Thread.sleep(ms);

function rd32(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0]
           & 0xFFFFFFFF;
}

// The signature is the check that the control block is real. Zephyr's RTT
// writes "SEGGER RTT" into acID at init, so an uninitialised or wrong address
// gives zeroes or garbage - and reporting "0 bytes of log" for a wrong address
// would be indistinguishable from a genuinely silent image, which is the
// single most expensive confusion available on a board with no console.
var idBytes = session.memory.readData(Memory.Page.PROGRAM, cb, 8, CB_ACID_LEN, false);
var id = "";
for (var i = 0; i < CB_ACID_LEN; i++) {
    var c = idBytes[i] & 0xFF;
    if (c === 0) { break; }
    id += String.fromCharCode(c);
}

script.traceWrite("RTT control block at " + hex32(cb) + ", acID = \"" + id + "\"");
if (id.indexOf("SEGGER RTT") !== 0) {
    script.traceWrite("");
    script.traceWrite("THAT IS NOT AN INITIALISED RTT CONTROL BLOCK.");
    script.traceWrite("Either RTT_CB is the wrong address for the image actually");
    script.traceWrite("running, or the image has not reached RTT init - which on a");
    script.traceWrite("board that faults before then is the same silence. Check the");
    script.traceWrite("address against the ELF that was flashed, then reset and");
    script.traceWrite("retry with RTT_RESET set before concluding the image is dead.");
    session.target.disconnect();
    debugServer.stop();
    java.lang.System.exit(1);
}

var up      = cb + CB_UP_ARRAY_OFF;
var pBuffer = rd32(up + UP_PBUFFER_OFF);
var size    = rd32(up + UP_SIZE_OFF);
var wrOff   = rd32(up + UP_WROFF_OFF);
var rdOff   = rd32(up + UP_RDOFF_OFF);

script.traceWrite("up[0]: buffer " + hex32(pBuffer) + "  size " + size +
                  "  WrOff " + wrOff + "  RdOff " + rdOff);
script.traceWrite("");

// Nothing has ever drained this buffer, so RdOff is 0 and WrOff is how far the
// firmware got. CONFIG_SEGGER_RTT_MODE_NO_BLOCK_SKIP means writes are DROPPED
// once it is full rather than overwriting - so what is captured is the FIRST
// kilobyte of output, not the most recent. For a boot banner that is the right
// end of the log; for a fault that happens later it is the wrong one, and the
// fix then is to raise CONFIG_SEGGER_RTT_BUFFER_SIZE_UP rather than to read
// this differently.
var n = (wrOff >= rdOff) ? (wrOff - rdOff) : (size - rdOff + wrOff);
if (n === 0) {
    script.traceWrite("(buffer empty - the image initialised RTT and logged nothing)");
} else {
    var out = "";
    var start = rdOff;
    var remaining = n;
    while (remaining > 0) {
        var chunk = Math.min(remaining, size - start);
        var data = session.memory.readData(Memory.Page.PROGRAM, pBuffer + start,
                                           8, chunk, false);
        for (var j = 0; j < chunk; j++) {
            out += String.fromCharCode(data[j] & 0xFF);
        }
        start = (start + chunk) % size;
        remaining -= chunk;
    }
    var lines = out.split("\n");
    for (var k = 0; k < lines.length; k++) {
        script.traceWrite(lines[k].replace(/\r/g, ""));
    }
    script.traceWrite("");
    script.traceWrite("(" + n + " bytes" + (wrOff >= size - 4 ? ", buffer FULL - later output was dropped" : "") + ")");
}

session.target.disconnect();
debugServer.stop();
