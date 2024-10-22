/* arch/arm/mach-msm/proc_comm.c
 *
 * Copyright (C) 2007-2008 Google, Inc.
 * Author: Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <mach/msm_iomap.h>
#include <mach/system.h>

#include "proc_comm.h"

#if defined(CONFIG_ARCH_MSM7X30) || defined(CONFIG_ARCH_MSM8X60)
#define MSM_TRIG_A2M_INT(n) (writel(1 << n, MSM_GCC_BASE + 0x8))
#endif

#define MSM_A2M_INT(n) (MSM_CSR_BASE + 0x400 + (n) * 4)
#define TIMEOUT		15000000

static inline void notify_other_proc_comm(void)
{
#if defined(CONFIG_ARCH_MSM7X30)
	MSM_TRIG_A2M_INT(6);
#elif defined(CONFIG_ARCH_MSM8X60)
	MSM_TRIG_A2M_INT(5);
#else
	writel(1, MSM_A2M_INT(6));
#endif
}

#define APP_COMMAND 0x00
#define APP_STATUS  0x04
#define APP_DATA1   0x08
#define APP_DATA2   0x0C

#define MDM_COMMAND 0x10
#define MDM_STATUS  0x14
#define MDM_DATA1   0x18
#define MDM_DATA2   0x1C

static DEFINE_SPINLOCK(proc_comm_lock);

void msm_pm_flush_console(void);

/* The higher level SMD support will install this to
 * provide a way to check for and handle modem restart.
 */
int (*msm_check_for_modem_crash)(void);

/* Poll for a state change, checking for possible
 * modem crashes along the way (so we don't wait
 * forever while the ARM9 is blowing up).
 *
 * Return an error in the event of a modem crash and
 * restart so the msm_proc_comm() routine can restart
 * the operation from the beginning.
 */
static int proc_comm_wait_for(void __iomem *addr, unsigned value)
{
#ifdef CONFIG_PROC_COMM_TIMEOUT_RESET
	unsigned long long timeout = TIMEOUT;
#endif

	for (;;) {
		if (readl(addr) == value)
			return 0;

		if (msm_check_for_modem_crash)
			if (msm_check_for_modem_crash())
				return -EAGAIN;
#ifdef CONFIG_PROC_COMM_TIMEOUT_RESET
		udelay(1);
		if (timeout-- == 0) {
			if (msm_hw_reset_hook) {
				pr_err("proc_comm: TIMEOUT. modem has probably crashed. Rebooting...\n");
				dump_stack();
				msm_pm_flush_console();
				msm_hw_reset_hook();

				/* in this case the modem or watchdog should reboot us */
				for (;;)
					;
			} else {
				pr_err("proc_comm: TIMEOUT. modem has probably crashed. Retrying...\n");
			}
			timeout = TIMEOUT;
		}
#endif
	}
}

int msm_proc_comm(unsigned cmd, unsigned *data1, unsigned *data2)
{
	void __iomem *base = MSM_SHARED_RAM_BASE;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&proc_comm_lock, flags);

	for (;;) {
		if (proc_comm_wait_for(base + MDM_STATUS, PCOM_READY))
			continue;

		writel(cmd, base + APP_COMMAND);
		writel(data1 ? *data1 : 0, base + APP_DATA1);
		writel(data2 ? *data2 : 0, base + APP_DATA2);

		/* Make sure the writes complete before notifying the other side */
		dsb();

		notify_other_proc_comm();

		if (proc_comm_wait_for(base + APP_COMMAND, PCOM_CMD_DONE))
			continue;

		if (readl(base + APP_STATUS) != PCOM_CMD_FAIL) {
			if (data1)
				*data1 = readl(base + APP_DATA1);
			if (data2)
				*data2 = readl(base + APP_DATA2);
			ret = 0;
		} else {
			ret = -EIO;
		}
		break;
	}

	writel(PCOM_CMD_IDLE, base + APP_COMMAND);

	/* Make sure the writes complete before returning */
	dsb();

	spin_unlock_irqrestore(&proc_comm_lock, flags);
	return ret;
}

 
