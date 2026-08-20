// SPDX-License-Identifier: Apache-2.0
//
// Identify a connected CC13x2/CC26x2 part and dump its CCFG, read-only.
//
// WHY THIS EXISTS RATHER THAN A dslite FLAG. dslite's flash operation list on
// this device is Erase, EraseSectors, ReadPriBle, ReadSecBle, WriteSecBle,
// ReadPriIeee, ReadSecIeee, BlankCheck - there is NO memory read among them.
// The only way to read a word out of a target through TI's stack is
// DebugServer Scripting, which ships inside UniFlash at
// ccs_base/scripting/bin/dss.bat and is a Rhino JavaScript host over the same
// DebugServer dslite drives.
//
// WHAT IT ANSWERS, and why each one is load-bearing for this bench:
//
//   * CC2652P1F vs CC2652P7. Different die, different Zephyr SoC series
//     (cc13x2_cc26x2 vs cc13x2x7_cc26x2x7), 352 KB of flash vs 704 KB, and
//     therefore a CCFG at a different address. Guessing wrong puts the CCFG
//     write in the wrong place, and on a board whose ROM bootloader backdoor
//     is disabled and whose only probe is a borrowed XDS110, a bad CCFG is
//     not cheaply recoverable.
//   * The FACTORY CCFG. Its BL_CONFIG word says whether the ROM bootloader
//     backdoor is enabled and on which DIO - which is the direct measurement
//     of the thing cc2538-bsl's silence only implied.
//
// Configuration comes from the environment rather than argv, because dss.bat's
// argument passing is version-dependent and an env var is not:
//
//   $env:CCXML = "scripts\ti\cc2652p_dongle.ccxml"
//   & <uniflash>\ccs_base\scripting\bin\dss.bat scripts\ti\dss_identify.js

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var FCFG1_USER_ID           = 0x50001294;
var FCFG1_ICEPICK_DEVICE_ID = 0x50001318;
// FLASH_BASE 0x40030000 + FLASH_O_FCFG_B0_SSIZE0 0x2430. B0_NUM_SECTORS is
// bits 27:16; the sector size on this family is 8 KB. This is driverlib's own
// FlashSizeGet() arithmetic, and it is read from the part rather than assumed
// so that the P1-vs-P7 question is answered by the silicon.
var FLASH_FCFG_B0_SSIZE0    = 0x40032430;
var SECTOR_SIZE             = 8192;
var CCFG_LEN                = 88;

// Rhino hands back a java.lang.String from Long.toHexString, whose `length`
// is a PROPERTY here, not a method - calling it raises "length is not a
// function, it is java.lang.Integer". Converting to a native JS string with
// String(...) first sidesteps the whole question.
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
// ".*" matches three devices on this topology - Cortex_M4_0, IcePick_C and
// CS_DAP_0 - and openSession refuses an ambiguous pattern rather than picking.
// The Cortex-M is the only one that can read memory.
var session = debugServer.openSession(".*Cortex_M4_0");
script.setScriptTimeout(30000);
session.target.connect();

function rd(addr) {
    return session.memory.readData(Memory.Page.PROGRAM, addr, 32, 1, false)[0];
}

var userId  = rd(FCFG1_USER_ID);
var iceId   = rd(FCFG1_ICEPICK_DEVICE_ID);
var ssize0  = rd(FLASH_FCFG_B0_SSIZE0);
var sectors = (ssize0 & 0x0FFF0000) >>> 16;
var flashSz = sectors * SECTOR_SIZE;

script.traceWrite("FCFG1.USER_ID            = " + hex32(userId));
script.traceWrite("FCFG1.ICEPICK_DEVICE_ID  = " + hex32(iceId));
script.traceWrite("FLASH.FCFG_B0_SSIZE0     = " + hex32(ssize0));
script.traceWrite("flash sectors            = " + sectors +
                  "  (" + flashSz + " bytes, " + (flashSz / 1024) + " KB)");

// USER_ID bits 31:28 PG_REV, 25:22 PROTOCOL, 21:16 reserved/PKG, 15:12 VER.
// The part number is not encoded here in a single tidy field across the whole
// family, so the flash size is what actually separates the two candidates:
// CC2652P1F is 352 KB, CC2652P7 is 704 KB.
if (flashSz === 352 * 1024) {
    script.traceWrite("=> 352 KB: CC2652P1F class, Zephyr soc series cc13x2_cc26x2");
} else if (flashSz === 704 * 1024) {
    script.traceWrite("=> 704 KB: CC2652P7 class, Zephyr soc series cc13x2x7_cc26x2x7");
} else {
    script.traceWrite("=> UNEXPECTED flash size; do not assume either series");
}

var ccfgAddr = flashSz - CCFG_LEN;
script.traceWrite("");
script.traceWrite("CCFG at " + hex32(ccfgAddr) + ":");
var ccfg = session.memory.readData(Memory.Page.PROGRAM, ccfgAddr, 32,
                                   CCFG_LEN / 4, false);
for (var i = 0; i < ccfg.length; i++) {
    script.traceWrite("  +" + hex32(i * 4).substring(6) + "  " + hex32(ccfg[i]));
}

// CCFG_O_BL_CONFIG is at offset 0x30 within the 88-byte CCFG image (word
// index 12). The field masks are hw_ccfg.h's, READ FROM THE HEADER rather
// than recalled - an earlier pass here guessed BL_PIN_NUMBER at bits 23:16
// and BL_LEVEL at bit 8, and reported "BL_PIN_NUMBER = 254" for a part whose
// pin is 15. A plausible-looking decode of a real register is worse than no
// decode, because nothing about it looks wrong.
//
//   BOOTLOADER_ENABLE  0xFF000000   must be 0xC5
//   BL_LEVEL           0x00010000   1 = backdoor active high, 0 = active low
//   BL_PIN_NUMBER      0x0000FF00   the DIO sampled at reset
//   BL_ENABLE          0x000000FF   must be 0xC5 for the backdoor itself
var blConfig = ccfg[12] & 0xFFFFFFFF;
var blEnable = (blConfig >>> 24) & 0xFF;
var blLevel  = (blConfig >>> 16) & 0x01;
var blPin    = (blConfig >>> 8) & 0xFF;
var bkdoor   = blConfig & 0xFF;
script.traceWrite("");
script.traceWrite("BL_CONFIG = " + hex32(blConfig));
script.traceWrite("  BOOTLOADER_ENABLE = 0x" + java.lang.Integer.toHexString(blEnable) +
                  (blEnable === 0xC5 ? "  (ROM bootloader ENABLED)"
                                     : "  (ROM bootloader DISABLED)"));
script.traceWrite("  BL_PIN_NUMBER     = DIO" + blPin);
script.traceWrite("  BL_LEVEL          = " + blLevel +
                  (blLevel === 1 ? "  (backdoor asserted HIGH at reset)"
                                 : "  (backdoor asserted LOW at reset)"));
script.traceWrite("  BL_ENABLE         = 0x" + java.lang.Integer.toHexString(bkdoor) +
                  (bkdoor === 0xC5 ? "  (backdoor ENABLED)"
                                   : "  (backdoor DISABLED)"));

session.target.disconnect();
debugServer.stop();
