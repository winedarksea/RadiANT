/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Spike B part 2 - the nRF5340 application-core companion to radiant_core/spike/promisc.
 *
 * Provenance: clean-room. Written from the nRF5340 product specification's
 * GPIO.PIN_CNF.MCUSEL and RESET.NETWORK.FORCEOFF sections, the public Nordic MDK
 * headers shipped with NCS, and Zephyr's public Kconfig
 * (CONFIG_SOC_NRF53_CPUNET_ENABLE). Nothing here derives from sdk-ant or from an
 * adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * Why this exists at all
 * ---------------------------------------------------------------------------
 * The nRF5340's RADIO lives on the network core, so the promiscuous capture must
 * run there. Two things stand between that image and a usable console, and both
 * are the application core's job:
 *
 *  1. The network core is held in reset by RESET.NETWORK.FORCEOFF until an
 *     application-core image clears it. Zephyr does that for us when
 *     CONFIG_SOC_NRF53_CPUNET_ENABLE=y, which is why prj.conf sets it explicitly
 *     rather than relying on a default that could change.
 *
 *  2. Every GPIO belongs to the application core until it hands the pin over by
 *     writing PIN_CNF[n].MCUSEL = NetworkMCU. The network core's uart0 is
 *     P1.00/P1.01, wired to the DK's second VCOM, so without that write the
 *     capture program runs and has nowhere to print. Spike B part 1 stalled
 *     exactly here and concluded, wrongly, that the network core "cannot print".
 *
 * ---------------------------------------------------------------------------
 * Why not nrf/samples/nrf5340/empty_app_core
 * ---------------------------------------------------------------------------
 * That sample does both jobs and is the documented answer, but it grants *all*
 * GPIOs to the network core and then powers off application-core RAM and spins
 * in WFI with interrupts disabled. Two costs, and on a board that has to be
 * reflashed several times in one sitting both of them bite:
 *
 *  - Granting P0.20/P0.22 away takes the *application* core's own console with
 *    it, so if the network core stays silent there is no second opinion about
 *    why. Here the application core keeps its VCOM and prints a heartbeat, which
 *    turns "which core is dead" from an inference into a reading.
 *  - RAM off plus interrupts off is a harder state for a debugger to interrupt.
 *    It is recoverable, but it is a recovery rather than a connect, and this
 *    board is on a strict "leave it working" obligation.
 *
 * So this grants exactly the two pins the network-core console needs and
 * otherwise stays out of the way.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#include <nrfx.h>

/* uart0 on the nRF5340 network core, as the DK routes it to VCOM1. Named rather
 * than spelled 0 and 1 inline so the reason a pin is in this list survives. */
#define NETCORE_CONSOLE_TX_PIN  0u   /* P1.00 */
#define NETCORE_CONSOLE_RX_PIN  1u   /* P1.01 */

/* PRE_KERNEL_1 and before the network core is released. Zephyr clears
 * NETWORK.FORCEOFF from its own PRE_KERNEL_1 hook, and a pin granted after the
 * network core has already configured its UART is granted too late: the
 * peripheral latches nothing and the port stays silent. Priority 0 puts this
 * ahead of that. */
static int netcore_console_pins(void)
{
	NRF_P1_S->PIN_CNF[NETCORE_CONSOLE_TX_PIN] =
		(GPIO_PIN_CNF_MCUSEL_NetworkMCU << GPIO_PIN_CNF_MCUSEL_Pos);
	NRF_P1_S->PIN_CNF[NETCORE_CONSOLE_RX_PIN] =
		(GPIO_PIN_CNF_MCUSEL_NetworkMCU << GPIO_PIN_CNF_MCUSEL_Pos);

	return 0;
}

SYS_INIT(netcore_console_pins, PRE_KERNEL_1, 0);

int main(void)
{
	uint32_t beat = 0;

	printk("\n[appcore] Spike B part 2 companion: P1.00/P1.01 -> NetworkMCU, "
	       "NETWORK.FORCEOFF cleared by CONFIG_SOC_NRF53_CPUNET_ENABLE\n");
	printk("[appcore] P1.PIN_CNF[0]=0x%08X P1.PIN_CNF[1]=0x%08X "
	       "RESET.NETWORK.FORCEOFF=%u\n",
	       (unsigned int)NRF_P1_S->PIN_CNF[NETCORE_CONSOLE_TX_PIN],
	       (unsigned int)NRF_P1_S->PIN_CNF[NETCORE_CONSOLE_RX_PIN],
	       (unsigned int)NRF_RESET_S->NETWORK.FORCEOFF);
	printk("[appcore] the capture is on the OTHER VCOM; this one only says "
	       "the application core is alive\n");

	/* A heartbeat rather than a sleep. If the network-core VCOM is silent,
	 * the first question is whether the board is running at all, and a
	 * heartbeat answers it without a debugger. */
	while (1) {
		k_sleep(K_SECONDS(10));
		printk("[appcore] alive %u (FORCEOFF=%u)\n", ++beat,
		       (unsigned int)NRF_RESET_S->NETWORK.FORCEOFF);
	}

	return 0;
}
