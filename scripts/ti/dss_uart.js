// SPDX-License-Identifier: Apache-2.0
//
// Read a CC13x2/CC26x2's UART0 peripheral state and its pin routing out of a
// RUNNING image, over the XDS110. Read-only, and never halts the core.
//
// WHY THIS EXISTS. A serial port that says nothing has at least five causes
// that all present identically from the host: the pins are not routed, they
// are routed to the wrong DIO, they are routed the wrong way round (TX/RX
// swapped), the peripheral is not enabled, or the baud divisor is wrong. From
// the host end every one of those is the same symptom - an open port that
// never delivers a byte - and guessing between them is how afternoons die.
//
// From the debug port they are five DIFFERENT readings, and this script takes
// them. IOCFG says where the signals are routed and which way; UART0.CTL says
// whether the peripheral, its transmitter and its receiver are on; IBRD/FBRD
// say what baud it actually settled on, computed back into bits per second so
// it can be compared with the host's number directly rather than by hand.
//
// IT NEVER READS UART_O_DR, deliberately - that register POPS A BYTE off the
// receive FIFO. Reading it to see "what has arrived" consumes the evidence and
// steals a byte from the firmware, so this reports FR/RXFE instead.
//
// NO HALT, AND THE RUN BEFORE THE READ IS LOAD-BEARING - both for the reasons
// written up at length in dss_rtt.js: connecting resets the part (so an
// immediate read catches every register at its reset default, which looks
// exactly like firmware that never configured anything), and an explicit
// halt() on this rig kills the DAP with an error that blames the wiring.
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_uart.js

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var IOC_BASE       = 0x40081000;
var GPIO_BASE      = 0x40022000;
var GPIO_O_DOE31_0 = 0x000000D0;
var GPIO_O_DIN31_0 = 0x000000C0;
var IOC_PORT_ID_M  = 0x0000003F;
var IOC_IE         = 0x20000000;

var UART0_BASE   = 0x40001000;
var UART_O_FR    = 0x018;
var UART_O_IBRD  = 0x024;
var UART_O_FBRD  = 0x028;
var UART_O_LCRH  = 0x02C;
var UART_O_CTL   = 0x030;
var UART_O_IMSC  = 0x038;

// PRCM - is the serial power domain even on? If it is not, the UART registers
// read as garbage or fault, and every conclusion drawn from them is worthless.
//
// ONLY PDSTAT0 IS READ HERE, AND THAT IS DELIBERATE. An earlier version of
// this script also printed a "UART0 run clock" line from a guessed
// PRCM_O_UARTCLKGR offset, and it confidently reported the clock OFF on a UART
// that was demonstrably running - a fabricated symptom that sent one round of
// investigation at the wrong subsystem. The reads below are their own proof
// that the peripheral is clocked: a gated UART does not return a sane CTL and
// a correct baud divisor. Do not add a register here without checking its
// offset against driverlib's prcm.h - a wrong offset does not read as an
// error, it reads as a plausible number.
var PRCM_BASE          = 0x40082000;
var PRCM_O_PDSTAT0     = 0x00000140;   // bit 2 = SERIAL_ON

var PORTS = {
    0x00: "GPIO",
    0x0F: "UART0_RX", 0x10: "UART0_TX", 0x11: "UART0_CTS", 0x12: "UART0_RTS",
    0x13: "UART1_RX", 0x14: "UART1_TX"
};

function hex32(v) {
    var s = String(java.lang.Long.toHexString(v & 0xFFFFFFFF));
    while (s.length < 8) { s = "0" + s; }
    return "0x" + s.toUpperCase();
}

var script = ScriptingEnvironment.instance();
script.traceSetConsoleLevel(TraceLevel.INFO);

var ccxml = java.lang.System.getenv("CCXML");
if (ccxml === null) {
    script.traceWrite("CCXML environment variable is not set.");
    java.lang.System.exit(2);
}

var debugServer = script.getServer("DebugServer.1");
debugServer.setConfig(ccxml);
var session = debugServer.openSession(".*Cortex_M4_0");
script.setScriptTimeout(60000);
session.target.connect();

var settle = java.lang.System.getenv("UART_SETTLE");
var ms = (settle === null) ? 3000 : parseInt(String(settle), 10);
script.traceWrite("Running the target for " + ms + " ms, then reading (no halt)...");
session.target.runAsynch();
java.lang.Thread.sleep(ms);

function rd(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0]
           & 0xFFFFFFFF;
}

// ── Power and clock ─────────────────────────────────────────────────────────
var pdstat = rd(PRCM_BASE + PRCM_O_PDSTAT0);
script.traceWrite("PRCM.PDSTAT0      = " + hex32(pdstat) +
                  "   SERIAL power domain: " + (((pdstat >>> 2) & 1) ? "ON" : "OFF"));
script.traceWrite("");

// ── The peripheral ──────────────────────────────────────────────────────────
var ctl  = rd(UART0_BASE + UART_O_CTL);
var ibrd = rd(UART0_BASE + UART_O_IBRD);
var fbrd = rd(UART0_BASE + UART_O_FBRD);
var lcrh = rd(UART0_BASE + UART_O_LCRH);
var fr   = rd(UART0_BASE + UART_O_FR);
var imsc = rd(UART0_BASE + UART_O_IMSC);

script.traceWrite("UART0.CTL         = " + hex32(ctl) +
                  "   UARTEN " + (ctl & 1 ? "1" : "0") +
                  "  TXE " + ((ctl >>> 8) & 1) +
                  "  RXE " + ((ctl >>> 9) & 1) +
                  "  RTSEN " + ((ctl >>> 14) & 1) +
                  "  CTSEN " + ((ctl >>> 15) & 1));
script.traceWrite("UART0.LCRH        = " + hex32(lcrh) +
                  "   WLEN " + (5 + ((lcrh >>> 5) & 3)) +
                  "  FEN " + ((lcrh >>> 4) & 1) +
                  "  STP2 " + ((lcrh >>> 3) & 1) +
                  "  PEN " + ((lcrh >>> 1) & 1));
script.traceWrite("UART0.IMSC        = " + hex32(imsc) +
                  "   RXIM " + ((imsc >>> 4) & 1) +
                  "  TXIM " + ((imsc >>> 5) & 1) +
                  "  RTIM " + ((imsc >>> 6) & 1));
script.traceWrite("UART0.FR          = " + hex32(fr) +
                  "   RXFE " + ((fr >>> 4) & 1) +
                  "  TXFF " + ((fr >>> 5) & 1) +
                  "  RXFF " + ((fr >>> 6) & 1) +
                  "  TXFE " + ((fr >>> 7) & 1) +
                  "  BUSY " + ((fr >>> 3) & 1));

// Baud back out of the divisor, so it can be compared with the host's setting
// as a number rather than as a hex divisor nobody can read at a glance.
// CC13x2/CC26x2 clocks the UART from 48 MHz: baud = clk / (16 * divisor),
// divisor = IBRD + FBRD/64.
if (ibrd !== 0 || fbrd !== 0) {
    var div  = ibrd + (fbrd / 64.0);
    var baud = 48000000.0 / (16.0 * div);
    script.traceWrite("UART0.IBRD/FBRD   = " + ibrd + " / " + fbrd +
                      "   -> " + Math.round(baud) + " baud (from a 48 MHz clock)");
} else {
    script.traceWrite("UART0.IBRD/FBRD   = 0 / 0   -> NO BAUD SET. The driver never " +
                      "configured this UART.");
}
script.traceWrite("");

// ── Where the signals go ────────────────────────────────────────────────────
var doe = rd(GPIO_BASE + GPIO_O_DOE31_0);
var din = rd(GPIO_BASE + GPIO_O_DIN31_0);
script.traceWrite("GPIO.DOE31_0      = " + hex32(doe));
script.traceWrite("GPIO.DIN31_0      = " + hex32(din));
script.traceWrite("");
script.traceWrite(" DIO  IOCFG       PORT        dir  level");
script.traceWrite(" ---  ----------  ----------  ---  -----");

var uartRx = -1, uartTx = -1;
for (var dio = 0; dio < 32; dio++) {
    var cfg  = rd(IOC_BASE + dio * 4);
    var port = cfg & IOC_PORT_ID_M;
    var name = PORTS[port];
    if (name === undefined) {
        name = "port 0x" + String(java.lang.Integer.toHexString(port));
    }

    var isOut = (doe >>> dio) & 1;
    var lvl   = (din >>> dio) & 1;
    var dir   = isOut ? "out" : (((cfg & IOC_IE) !== 0) ? "in " : "-  ");

    if (port === 0x0F) { uartRx = dio; }
    if (port === 0x10) { uartTx = dio; }

    if (port !== 0x00 || isOut || ((cfg & IOC_IE) !== 0)) {
        var d = (dio < 10 ? " " : "") + dio;
        script.traceWrite("  " + d + "  " + hex32(cfg) + "  " +
                          name + Array(12 - String(name).length).join(" ") +
                          dir + "  " + lvl);
    }
}

script.traceWrite("");
script.traceWrite("UART0 routed to: RX = DIO" + uartRx + ", TX = DIO" + uartTx);

// The one reading that is worth calling out on its own. An idle UART receive
// line sits HIGH; a receive pin reading 0 with nothing being sent means it is
// not connected to an idle transmitter at all - a floating or grounded pin,
// or the wrong DIO.
if (uartRx >= 0) {
    var rxLvl = (din >>> uartRx) & 1;
    script.traceWrite("RX line (DIO" + uartRx + ") idles " + (rxLvl ? "HIGH - a transmitter is holding it, as it should" :
        "LOW - NOTHING IS DRIVING IT. An idle UART line is high; this pin is floating, grounded, or not the one the bridge is on."));
}

session.target.disconnect();
debugServer.stop();
