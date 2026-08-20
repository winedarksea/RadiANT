// SPDX-License-Identifier: Apache-2.0
//
// Is this CC13x2/CC26x2 actually executing? Read-only.
//
// WHY A SEPARATE SCRIPT FROM dss_rtt.js. An empty RTT buffer is ambiguous in
// the worst possible way: it means EITHER "the image is running and simply has
// not logged anything yet" OR "the image faulted before it ever reached a
// LOG_INF". Those two call for opposite next moves, and on a board whose only
// console is that buffer there is nothing else to break the tie. So this asks
// the question the buffer cannot answer, without depending on the firmware
// having cooperated.
//
// THE TEST IS THAT THE PROGRAM COUNTER MOVES. Halt, read PC, resume, wait,
// halt again. Two different PCs inside the image is execution; the same PC
// twice is a tight loop or a halt; a PC in a fault handler is a crash. Nothing
// here needs the firmware to have logged, blinked or enumerated.
//
// AND IT READS THE FAULT STATUS REGISTERS EITHER WAY, because on ARM they
// survive the fault and they name the cause - a MMFSR/BFSR/UFSR bit is a
// direct answer where a silent console is a guess. They are latched and sticky,
// so a nonzero value may be from an EARLIER boot; that is called out in the
// output rather than being silently reported as current.
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_alive.js

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var SCB_CFSR  = 0xE000ED28;   // MMFSR(7:0) BFSR(15:8) UFSR(31:16)
var SCB_HFSR  = 0xE000ED2C;
var SCB_MMFAR = 0xE000ED34;
var SCB_BFAR  = 0xE000ED38;
var SCB_ICSR  = 0xE000ED04;   // VECTACTIVE(8:0) = exception currently running

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
script.setScriptTimeout(60000);
session.target.connect();

function rd(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0]
           & 0xFFFFFFFF;
}
function pc() {
    return session.expression.evaluate("PC") & 0xFFFFFFFF;
}

// DELIBERATELY NO reset(). A reset on this rig has been seen to drop the cJTAG
// link for the rest of the session ("Unable to access the DAP", twenty times,
// then a failed disconnect) - recoverable by reconnecting, but it destroys the
// very state being measured. Whatever the board is doing now is the question.
session.target.halt();
var pc1 = pc();
session.target.runAsynch();
java.lang.Thread.sleep(1500);
session.target.halt();
var pc2 = pc();
session.target.runAsynch();
java.lang.Thread.sleep(1500);
session.target.halt();
var pc3 = pc();

script.traceWrite("PC samples: " + hex32(pc1) + "  " + hex32(pc2) + "  " + hex32(pc3));
if (pc1 !== pc2 || pc2 !== pc3) {
    script.traceWrite("=> EXECUTING. The program counter moved between halts.");
} else {
    script.traceWrite("=> The PC did not move across three halts 1.5 s apart.");
    script.traceWrite("   That is a tight loop, a WFI the debugger keeps");
    script.traceWrite("   catching at the same instruction, or a stopped core.");
    script.traceWrite("   Look the address up in the ELF before deciding which:");
    script.traceWrite("     arm-zephyr-eabi-nm -n zephyr.elf");
}

var icsr = rd(SCB_ICSR);
var vect = icsr & 0x1FF;
script.traceWrite("");
script.traceWrite("ICSR = " + hex32(icsr) + "  VECTACTIVE = " + vect +
                  (vect === 0 ? "  (thread mode - not in an exception)"
                              : "  (inside exception " + vect + ")"));

var cfsr = rd(SCB_CFSR);
var hfsr = rd(SCB_HFSR);
script.traceWrite("CFSR = " + hex32(cfsr) + "   HFSR = " + hex32(hfsr));
if (cfsr !== 0 || hfsr !== 0) {
    script.traceWrite("  A FAULT HAS BEEN TAKEN AT SOME POINT. These bits are");
    script.traceWrite("  sticky and are NOT cleared by reset of the core alone, so");
    script.traceWrite("  this may be from an earlier boot - confirm by clearing");
    script.traceWrite("  them, power-cycling and re-reading before acting on it.");
    if ((cfsr & 0x80) !== 0)     { script.traceWrite("    MMARVALID: MMFAR = " + hex32(rd(SCB_MMFAR))); }
    if ((cfsr & 0x8000) !== 0)   { script.traceWrite("    BFARVALID: BFAR  = " + hex32(rd(SCB_BFAR))); }
    if ((cfsr & 0x01) !== 0)     { script.traceWrite("    IACCVIOL  - instruction access violation (MPU)"); }
    if ((cfsr & 0x02) !== 0)     { script.traceWrite("    DACCVIOL  - data access violation (MPU)"); }
    if ((cfsr & 0x08) !== 0)     { script.traceWrite("    MUNSTKERR - fault unstacking on exception return"); }
    if ((cfsr & 0x10) !== 0)     { script.traceWrite("    MSTKERR   - fault stacking on exception entry"); }
    if ((cfsr & 0x100) !== 0)    { script.traceWrite("    IBUSERR   - bus fault on an instruction fetch"); }
    if ((cfsr & 0x200) !== 0)    { script.traceWrite("    PRECISERR - precise data bus error"); }
    if ((cfsr & 0x400) !== 0)    { script.traceWrite("    IMPRECISERR - imprecise data bus error"); }
    if ((cfsr & 0x10000) !== 0)  { script.traceWrite("    UNDEFINSTR - undefined instruction"); }
    if ((cfsr & 0x20000) !== 0)  { script.traceWrite("    INVSTATE  - invalid EPSR/Thumb state"); }
    if ((cfsr & 0x1000000) !== 0){ script.traceWrite("    UNALIGNED - unaligned access"); }
    if ((cfsr & 0x2000000) !== 0){ script.traceWrite("    DIVBYZERO"); }
    if ((hfsr & 0x40000000) !== 0) { script.traceWrite("    HFSR.FORCED - escalated from a configurable fault above"); }
    if ((hfsr & 0x00000002) !== 0) { script.traceWrite("    HFSR.VECTTBL - fault reading the vector table"); }
} else {
    script.traceWrite("  No fault has been taken.");
}

// Leave it running. A script that halts a board and walks away turns every
// later "the dongle went silent" into a hunt for a bug that is this script.
session.target.runAsynch();
session.target.disconnect();
debugServer.stop();
