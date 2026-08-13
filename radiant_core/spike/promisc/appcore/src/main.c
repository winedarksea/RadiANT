/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Spike B part 2 - the nRF5340 application-core companion to radiant_core/spike/promisc.
 *
 * Clean-room: written from the nRF5340 product spec's GPIO.PIN_CNF.MCUSEL and
 * RESET.NETWORK.FORCEOFF sections, the public Nordic MDK headers, and
 * Zephyr's CONFIG_SOC_NRF53_CPUNET_ENABLE Kconfig. Nothing derives from
 * sdk-ant or an ANT+ profile document.
 *
 * The nRF5340's RADIO lives on the network core, so the promiscuous capture
 * must run there, and two things - both the application core's job - stand
 * between that image and a usable console:
 *  1. RESET.NETWORK.FORCEOFF holds the network core in reset until an
 *     app-core image clears it; CONFIG_SOC_NRF53_CPUNET_ENABLE=y does that,
 *     set explicitly in prj.conf rather than relying on a default.
 *  2. Every GPIO belongs to the app core until PIN_CNF[n].MCUSEL is set to
 *     NetworkMCU. The network core's uart0 (P1.00/P1.01) is wired to the
 *     DK's second VCOM; without that write the capture program has nowhere
 *     to print. Spike B part 1 stalled here and wrongly concluded the
 *     network core "cannot print".
 *
 * Why not nrf/samples/nrf5340/empty_app_core: it does both jobs but grants
 * *all* GPIOs to the network core, then powers off app-core RAM and spins in
 * WFI with interrupts disabled. That takes the app core's own console with
 * it (no second opinion if the network core stays silent) and leaves a
 * harder state for a debugger to interrupt on a board that gets reflashed
 * repeatedly. This grants only the two console pins and otherwise stays out
 * of the way, keeping a heartbeat on the app core's own VCOM.
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
