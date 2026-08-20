// SPDX-License-Identifier: Apache-2.0
//
// Recover an undocumented CC13x2/CC26x2 board's pin map from the running
// factory firmware. Read-only.
//
// WHY. This bench has a CC2652P + CP2102N USB dongle with no schematic and no
// silkscreen worth reading, and the board port needs to know which DIO carries
// the UART the CP2102N is on. That is not guessable - vendors put UART0
// anywhere - and getting it wrong produces a board that builds, boots and says
// absolutely nothing, which is indistinguishable from a dozen other faults.
//
// THE TRICK IS THAT THE FACTORY FIRMWARE ALREADY KNOWS. Every pin the shipped
// Zigbee image uses, it configured itself, in the IOC. So: let the part run
// long enough to finish its own init, halt it, and read the 32 IOCFG registers
// back. Whichever DIOs report PORT_ID = MCU_UART0_RX / MCU_UART0_TX are the
// ones physically wired to the bridge.
//
// LETTING IT RUN IS LOAD-BEARING. A debugger connect halts the core, and on
// this part a connect can land before main() has configured anything - at
// which point every IOCFG reads its reset default and the whole table is
// zeroes that look like a real answer. runAsynch, sleep, halt is what makes
// the reading mean something.
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_pinmap.js

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var IOC_BASE        = 0x40081000;   // IOCFG0..IOCFG31, one 32-bit word each
var GPIO_BASE       = 0x40022000;
var GPIO_O_DOE31_0  = 0x000000D0;   // output-enable bitmask, one bit per DIO
var GPIO_O_DIN31_0  = 0x000000C0;   // input data, one bit per DIO
var IOC_PORT_ID_M   = 0x0000003F;
var IOC_IE          = 0x20000000;   // input enable

// driverlib ioc.h. Only the ports this board could plausibly use are named;
// anything else is reported by number rather than guessed at.
var PORTS = {
    0x00: "GPIO",
    0x09: "SSI0_RX",  0x0A: "SSI0_TX",  0x0B: "SSI0_FSS", 0x0C: "SSI0_CLK",
    0x0D: "I2C_SDA",  0x0E: "I2C_SCL",
    0x0F: "UART0_RX", 0x10: "UART0_TX", 0x11: "UART0_CTS", 0x12: "UART0_RTS",
    0x13: "UART1_RX", 0x14: "UART1_TX",
    0x2F: "RFC_GPO0", 0x30: "RFC_GPO1", 0x31: "RFC_GPO2", 0x32: "RFC_GPO3"
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

// Reset into the application, let it initialise, then stop it where it stands.
session.target.reset();
session.target.runAsynch();
java.lang.Thread.sleep(3000);
session.target.halt();

function rd(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0]
           & 0xFFFFFFFF;
}

var doe = rd(GPIO_BASE + GPIO_O_DOE31_0);
var din = rd(GPIO_BASE + GPIO_O_DIN31_0);
script.traceWrite("GPIO.DOE31_0 = " + hex32(doe) + "   (1 = driven output)");
script.traceWrite("GPIO.DIN31_0 = " + hex32(din) + "   (level now)");
script.traceWrite("");
script.traceWrite(" DIO  IOCFG       PORT           dir  level");
script.traceWrite(" ---  ----------  -------------  ---  -----");

var uartRx = -1, uartTx = -1;
for (var dio = 0; dio < 32; dio++) {
    var cfg  = rd(IOC_BASE + dio * 4);
    var port = cfg & IOC_PORT_ID_M;
    var name = PORTS[port];
    if (name === undefined) { name = "port 0x" + String(java.lang.Integer.toHexString(port)); }

    var isOut = (doe >>> dio) & 1;
    var lvl   = (din >>> dio) & 1;
    var dir   = isOut ? "out" : (((cfg & IOC_IE) !== 0) ? "in " : "-  ");

    if (port === 0x0F) { uartRx = dio; }
    if (port === 0x10) { uartTx = dio; }

    // A default-configured pin is noise; only report pins the firmware
    // actually touched, plus every non-GPIO assignment.
    if (port !== 0x00 || isOut || ((cfg & IOC_IE) !== 0)) {
        var d = (dio < 10 ? " " : "") + dio;
        script.traceWrite("  " + d + "  " + hex32(cfg) + "  " +
                          name + Array(15 - String(name).length).join(" ") +
                          dir + "  " + lvl);
    }
}

script.traceWrite("");
if (uartRx >= 0 || uartTx >= 0) {
    script.traceWrite("UART0 is on DIO" + uartRx + " (RX) / DIO" + uartTx + " (TX).");
    script.traceWrite("Those are the pins the CP2102N is wired to.");
} else {
    script.traceWrite("NO UART0 assignment found. Either the factory image does");
    script.traceWrite("not use a UART at all, or it had not reached its pin init");
    script.traceWrite("in the 3 s this script allowed - raise the sleep and retry");
    script.traceWrite("before concluding anything about the wiring.");
}

session.target.disconnect();
debugServer.stop();
