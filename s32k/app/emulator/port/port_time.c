/*
 * Zephyr side of the Arduino layer. Kept in C so that Zephyr headers never meet the Arduino ones.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>

uint64_t port_uptime_ms(void)
{
	return (uint64_t)k_uptime_get();
}

uint32_t port_micros(void)
{
	return k_cyc_to_us_floor32(k_cycle_get_32());
}

void port_delay_ms(uint32_t ms)
{
	k_msleep(ms);
}

void port_delay_us(uint32_t us)
{
	k_busy_wait(us);
}

/* Free bytes in the heap the drivers are created in */
uint32_t port_free_heap(void)
{
	extern int malloc_runtime_stats_get(struct sys_memory_stats *stats);
	struct sys_memory_stats stats;

	if (malloc_runtime_stats_get(&stats) != 0) {
		return 0;
	}
	return stats.free_bytes;
}
