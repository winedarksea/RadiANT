/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AD HOC interrupt-latency probe (2026-11-16, ISR-blocker hunt). Not for
 * commit as is.
 *
 * GPT0A runs a periodic down-counter at 48 MHz with a 997 us period and
 * interrupts at Zephyr priority 0 - the ANT UART's priority. On entry the ISR
 * reads how far the counter has moved past its timeout: that is how long the
 * interrupt was held off, by an irq_lock()/HwiP_disable() span or by another
 * priority-0 ISR. The stub hands over the hardware-stacked frame, so the PC
 * the interrupt returns to is recorded: for a masked span it is the first
 * instruction after the unmask, which names the site.
 *
 * Read with MESG_REQUEST [page, 0xFE] (bridge, ad hoc): page 0 summary,
 * pages 1..8 the per-PC table, page 0xFF resets and answers page 0.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <string.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>
#include <inc/hw_types.h>
#include <inc/hw_memmap.h>
#include <inc/hw_gpt.h>
#include <inc/hw_ints.h>

#define LAT_PERIOD_CYC (48u * 997u)
#define LAT_IRQN (INT_GPT0A - 16)
#define LAT_THR_CYC (48u * 50u)    /* table only spans >= 50 us */
#define LAT_NPC 16

struct lat_pc {
	uint32_t pc;
	uint32_t lr;
	uint32_t max;
	uint32_t n;
	uint32_t ipsr;
};

static volatile uint32_t lat_samples;
static volatile uint32_t lat_max;
static volatile uint32_t lat_min = 0xFFFFFFFFu;
/* <25, 25-50, 50-100, 100-170, 170-250, 250-500, >=500 us */
static volatile uint32_t lat_hist[7];
static struct lat_pc lat_tab[LAT_NPC];

static const uint32_t edges_us[6] = {25, 50, 100, 170, 250, 500};

void lat_probe_c(uint32_t *frame);

__attribute__((naked)) static void lat_isr_stub(void)
{
	__asm volatile(
		"tst lr, #4\n\t"
		"ite eq\n\t"
		"mrseq r0, msp\n\t"
		"mrsne r0, psp\n\t"
		"push {r4, lr}\n\t"
		"bl lat_probe_c\n\t"
		"pop {r4, pc}\n\t");
}

void lat_probe_c(uint32_t *frame)
{
	uint32_t v = HWREG(GPT0_BASE + GPT_O_TAR);
	uint32_t lat = (LAT_PERIOD_CYC - 1u) - v;
	uint32_t us = lat / 48u;
	int b = 6;

	HWREG(GPT0_BASE + GPT_O_ICLR) = 1u;

	lat_samples++;
	if (lat > lat_max) {
		lat_max = lat;
	}
	if (lat < lat_min) {
		lat_min = lat;
	}
	for (int i = 0; i < 6; i++) {
		if (us < edges_us[i]) {
			b = i;
			break;
		}
	}
	lat_hist[b]++;

	if (lat >= LAT_THR_CYC) {
		uint32_t pc = frame[6];
		int slot = -1;
		int low = 0;

		for (int i = 0; i < LAT_NPC; i++) {
			if (lat_tab[i].n != 0u && lat_tab[i].pc == pc) {
				slot = i;
				break;
			}
			if (lat_tab[i].max < lat_tab[low].max) {
				low = i;
			}
		}
		if (slot < 0) {
			if (lat_tab[low].n != 0u && lat_tab[low].max >= lat) {
				return;
			}
			slot = low;
			lat_tab[slot].pc = pc;
			lat_tab[slot].n = 0u;
			lat_tab[slot].max = 0u;
		}
		lat_tab[slot].n++;
		if (lat >= lat_tab[slot].max) {
			lat_tab[slot].max = lat;
			lat_tab[slot].lr = frame[5];
			lat_tab[slot].ipsr = frame[7] & 0x1FFu;
		}
	}
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Fills out (>= 37 bytes); returns length. */
size_t lat_probe_dump(uint8_t page, uint8_t *out)
{
	unsigned int key = irq_lock();
	size_t n;

	if (page == 0xFFu) {
		lat_samples = 0u;
		lat_max = 0u;
		lat_min = 0xFFFFFFFFu;
		memset((void *)lat_hist, 0, sizeof(lat_hist));
		memset(lat_tab, 0, sizeof(lat_tab));
		page = 0u;
	}
	out[0] = page;
	if (page == 0u) {
		put32(&out[1], lat_samples);
		put32(&out[5], lat_max);
		put32(&out[9], lat_min);
		for (int i = 0; i < 6; i++) {
			put32(&out[13 + 4 * i], lat_hist[i]);
		}
		/* hist[6] does not fit with min; folded in by the host as
		 * samples - sum(hist[0..5]). */
		n = 37u;
	} else {
		/* two entries per page: pc, lr, max, n (16 bytes each) +
		 * ipsr bytes at the end */
		int i0 = (page - 1) * 2;

		memset(&out[1], 0, 34);
		for (int k = 0; k < 2 && i0 + k < LAT_NPC; k++) {
			struct lat_pc *e = &lat_tab[i0 + k];

			put32(&out[1 + 16 * k], e->pc);
			put32(&out[5 + 16 * k], e->lr);
			put32(&out[9 + 16 * k], e->max);
			put32(&out[13 + 16 * k], e->n);
			out[33 + k] = (uint8_t)e->ipsr;
		}
		n = 35u;
	}
	irq_unlock(key);
	return n;
}

static int lat_probe_init(void)
{
	Power_setDependency(PowerCC26XX_PERIPH_GPT0);

	HWREG(GPT0_BASE + GPT_O_CTL) = 0u;
	HWREG(GPT0_BASE + GPT_O_CFG) = 0u;
	HWREG(GPT0_BASE + GPT_O_TAMR) = 0x2u; /* periodic, count down */
	HWREG(GPT0_BASE + GPT_O_TAILR) = LAT_PERIOD_CYC - 1u;
	HWREG(GPT0_BASE + GPT_O_ICLR) = 1u;
	HWREG(GPT0_BASE + GPT_O_IMR) = 1u;

	IRQ_DIRECT_CONNECT(LAT_IRQN, 0, lat_isr_stub, 0);
	irq_enable(LAT_IRQN);

	HWREG(GPT0_BASE + GPT_O_CTL) = 1u;
	return 0;
}

SYS_INIT(lat_probe_init, APPLICATION, 99);
