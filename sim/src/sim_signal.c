#include "sim_signal.h"

/* 2048 ticks a second, 60 seconds a minute: ticks per revolution at r rpm is
 * 2048 * 60 / r. Same shape for the 1/1024 s event time the combined page
 * uses.
 */
#define PERIOD_TICKS_NUMERATOR      122880u   /* 2048 * 60 */
#define EVENT_TIME_TICKS_NUMERATOR  61440u    /* 1024 * 60 */

/* Torque in 1/32 Nm for one revolution delivering P watts at r rpm:
 *
 *   torque_Nm = P / (2*pi*r/60)          so   torque_32 = 32 * 60 * P / (2*pi*r)
 *                                             torque_32 = 305.5775 * P / r
 *
 * Scaled by 1000 so it stays in integers. The receiver recovers
 * P = delta_torque * 2*pi * 64 / delta_period, so an error here shows up
 * directly as a power error and nowhere else - which is what makes it worth
 * writing the derivation down rather than the constant alone.
 */
#define TORQUE_TICKS_NUMERATOR      305577u
#define TORQUE_TICKS_SCALE          1000u

void sim_signal_init(struct sim_signal *sig, int32_t target, int32_t noise,
		     uint32_t seed)
{
	sig->target = target;
	sig->noise = noise;
	/* xorshift32 is dead at zero - it returns zero forever - and seed 0 is
	 * exactly what a Kconfig default or a memset leaves behind.
	 */
	sig->state = (seed != 0u) ? seed : 0x9E3779B9u;
}

static uint32_t xorshift32(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

int32_t sim_signal_next(struct sim_signal *sig)
{
	int32_t value = sig->target;

	if (sig->noise > 0) {
		uint32_t span = (uint32_t)(2 * sig->noise + 1);

		value += (int32_t)(xorshift32(&sig->state) % span) - sig->noise;
	}

	return (value < 0) ? 0 : value;
}

uint32_t sim_revs_advance(struct sim_revs *revs, uint32_t dt_us, uint32_t rpm)
{
	uint32_t completed;

	if (rpm == 0u) {
		return 0u;
	}

	/* rpm * dt_us / 60000000 revolutions, kept in 1/1000000ths so the
	 * remainder carries into the next call rather than being rounded away
	 * every message - at ~4 Hz that rounding would lose most of a
	 * revolution a second, and the cadence a receiver reconstructs would
	 * be wrong by a constant factor rather than by noise.
	 *
	 * rpm is bounded by 255 (it is a byte on the air) and dt_us by one
	 * channel period, so the product stays inside 32 bits.
	 */
	revs->phase_micro += (rpm * dt_us) / 60u;
	completed = revs->phase_micro / 1000000u;
	revs->phase_micro -= completed * 1000000u;

	return completed;
}

uint16_t sim_period_ticks(uint32_t rpm)
{
	if (rpm == 0u) {
		return 0u;
	}

	return (uint16_t)((PERIOD_TICKS_NUMERATOR + rpm / 2u) / rpm);
}

uint16_t sim_event_time_ticks(uint32_t rpm)
{
	if (rpm == 0u) {
		return 0u;
	}

	return (uint16_t)((EVENT_TIME_TICKS_NUMERATOR + rpm / 2u) / rpm);
}

uint16_t sim_torque_ticks(uint32_t watts, uint32_t rpm)
{
	uint64_t numerator;
	uint64_t denominator;

	if (rpm == 0u) {
		return 0u;
	}

	/* 64-bit on purpose: at 2000 W this overflows 32 bits before the
	 * division, and the result would be a small plausible number rather
	 * than an obvious one.
	 */
	numerator = (uint64_t)watts * TORQUE_TICKS_NUMERATOR;
	denominator = (uint64_t)rpm * TORQUE_TICKS_SCALE;

	return (uint16_t)((numerator + denominator / 2u) / denominator);
}
