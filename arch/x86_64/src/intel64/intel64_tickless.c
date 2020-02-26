/****************************************************************************
 * arch/x86_64/src/intel64/intel64_tickless.c
 *
 *   Copyright (C) 2014 Gregory Nutt,
 *                 2020 Chung-Fan Yang.
 *   All rights reserved.
 *
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *           Chung-Fan Yang <sonic.tw.tp@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/****************************************************************************
 * Tickless OS Support.
 *
 * When CONFIG_SCHED_TICKLESS is enabled, all support for timer interrupts
 * is suppressed and the platform specific code is expected to provide the
 * following custom functions.
 *
 *   void sim_timer_initialize(void): Initializes the timer facilities.  Called
 *     early in the intialization sequence (by up_intialize()).
 *   int up_timer_gettime(FAR struct timespec *ts):  Returns the current
 *     time from the platform specific time source.
 *   int up_timer_cancel(void):  Cancels the interval timer.
 *   int up_timer_start(FAR const struct timespec *ts): Start (or re-starts)
 *     the interval timer.
 *
 * The RTOS will provide the following interfaces for use by the platform-
 * specific interval timer implementation:
 *
 *   void sched_timer_expiration(void):  Called by the platform-specific
 *     logic when the interval timer expires.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>

#ifdef CONFIG_SCHED_TICKLESS
/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
#define NS_PER_USEC		1000UL
#define NS_PER_MSEC		1000000UL
#define NS_PER_SEC		1000000000UL

#define IA32_TSC_DEADLINE	0x6e0

#define X2APIC_LVTT		0x832
#define LVTT_TSC_DEADLINE	(1 << 18)
#define X2APIC_TMICT		0x838
#define X2APIC_TMCCT		0x839
#define X2APIC_TDCR		0x83e

#define TMR_IRQ IRQ14

#define ROUND_INT_DIV(s, d) (s + (d >> 1)) / d

/****************************************************************************
 * Private Data
 ****************************************************************************/

unsigned long tsc_freq;

static struct timespec g_goal_time_ts;
static uint64_t g_last_stop_time;
static uint64_t g_start_tsc;
static uint32_t g_timer_active;

static irqstate_t g_tmr_sync_count;
static irqstate_t g_tmr_flags;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

void up_mask_tmr(void)
{
  /* Disable TSC Deadline interrupt */
  write_msr(X2APIC_LVTT, TMR_IRQ | LVTT_TSC_DEADLINE | (1 << 16));
  /* Required when using TSC deadline mode. */
  asm volatile("mfence" : : : "memory");
}

void up_unmask_tmr(void)
{
  /* Enable TSC Deadline interrupt */
  write_msr(X2APIC_LVTT, TMR_IRQ | LVTT_TSC_DEADLINE);
  /* Required when using TSC deadline mode. */
  asm volatile("mfence" : : : "memory");
}

#ifndef CONFIG_SCHED_TICKLESS_ALARM
void up_timer_expire(void);
#else
void up_alarm_expire(void);
#endif

void up_timer_initialize(void)
{
  g_last_stop_time = g_start_tsc = rdtsc();

#ifndef CONFIG_SCHED_TICKLESS_ALARM
  (void)irq_attach(TMR_IRQ, (xcpt_t)up_timer_expire, NULL);
#else
  (void)irq_attach(TMR_IRQ, (xcpt_t)up_alarm_expire, NULL);
#endif

  return;
}

static inline uint64_t up_ts2tick(FAR const struct timespec *ts)
{
  return ROUND_INT_DIV((uint64_t)ts->tv_nsec * tsc_freq, NS_PER_SEC) + (uint64_t)ts->tv_sec * tsc_freq;
}

static inline void up_tick2ts(uint64_t tick, FAR struct timespec *ts)
{
  ts->tv_sec  = (tick / tsc_freq);
  ts->tv_nsec = (uint64_t)(ROUND_INT_DIV((tick % tsc_freq) * NSEC_PER_SEC, tsc_freq));
}

static inline void up_tmr_sync_up(void)
{
  if(!g_tmr_sync_count)
    {
      g_tmr_flags = enter_critical_section();
    }
  g_tmr_sync_count++;
}

static inline void up_tmr_sync_down(void)
{
  if(g_tmr_sync_count == 1)
    {
      leave_critical_section(g_tmr_flags);
    }

  if(g_tmr_sync_count > 0)
    {
      g_tmr_sync_count--;
    }
}

/****************************************************************************
 * Name: up_timer_gettime
 *
 * Description:
 *   Return the elapsed time since power-up (or, more correctly, since
 *   sim_timer_initialize() was called).  This function is functionally
 *   equivalent to:
 *
 *      int clock_gettime(clockid_t clockid, FAR struct timespec *ts);
 *
 *   when clockid is CLOCK_MONOTONIC.
 *
 *   This function provides the basis for reporting the current time and
 *   also is used to eliminate error build-up from small erros in interval
 *   time calculations.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the location in which to return the up-time.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   Called from the normal tasking context.  The implementation must
 *   provide whatever mutual exclusion is necessary for correct operation.
 *   This can include disabling interrupts in order to assure atomic register
 *   operations.
 *
 ****************************************************************************/

int up_timer_gettime(FAR struct timespec *ts)
{
  uint64_t diff = (rdtsc() - g_start_tsc);
  up_tick2ts(diff, ts);
  return OK;
}

#ifndef CONFIG_SCHED_TICKLESS_ALARM

/****************************************************************************
 * Name: up_timer_cancel
 *
 * Description:
 *   Cancel the interval timer and return the time remaining on the timer.
 *   These two steps need to be as nearly atomic as possible.
 *   sched_timer_expiration() will not be called unless the timer is
 *   restarted with up_timer_start().
 *
 *   If, as a race condition, the timer has already expired when this
 *   function is called, then that pending interrupt must be cleared so
 *   that up_timer_start() and the remaining time of zero should be
 *   returned.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Location to return the remaining time.  Zero should be returned
 *        if the timer is not active.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_timer_cancel(FAR struct timespec *ts)
{
  up_tmr_sync_up();

  up_mask_tmr();

  if (ts != NULL)
    {
      if (g_timer_active)
        {
          up_tick2ts(g_goal_time - rdtsc(), ts);
        }
      else
        {
          ts->tv_sec = 0;
          ts->tv_nsec = 0;
        }
    }

  g_timer_active = 0;

  up_tmr_sync_down();

  return OK;
}

/****************************************************************************
 * Name: up_timer_start
 *
 * Description:
 *   Start the interval timer.  sched_timer_expiration() will be
 *   called at the completion of the timeout (unless up_timer_cancel
 *   is called to stop the timing.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the time interval until sched_timer_expiration() is
 *        called.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_timer_start(FAR const struct timespec *ts)
{
  uint64_t ticks;

  up_tmr_sync_up();

  ticks = up_ts2tick(ts) + rdtsc();

  g_timer_active = 1;

  write_msr(IA32_TSC_DEADLINE, ticks);

  g_goal_time = ticks;

  up_unmask_tmr();

  up_tmr_sync_down();
  return OK;
}

/****************************************************************************
 * Name: up_timer_expire
 *
 * Description:
 *   Called as the IRQ handler for alarm expiration.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_timer_expire(void)
{
  g_timer_active = 0;

  up_mask_tmr();
  sched_timer_expiration();

  return;
}

#else /* CONFIG_SCHED_TICKLESS_ALARM */

/****************************************************************************
 * Name: up_timer_cancel
 *
 * Description:
 *   Cancel the interval timer and return the time remaining on the timer.
 *   These two steps need to be as nearly atomic as possible.
 *   sched_timer_expiration() will not be called unless the timer is
 *   restarted with up_timer_start().
 *
 *   If, as a race condition, the timer has already expired when this
 *   function is called, then that pending interrupt must be cleared so
 *   that up_timer_start() and the remaining time of zero should be
 *   returned.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Location to return the remaining time.  Zero should be returned
 *        if the timer is not active.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_alarm_cancel(FAR struct timespec *ts)
{
  up_tmr_sync_up();

  up_mask_tmr();

  if (ts != NULL)
    {
      up_timer_gettime(ts);
    }

  g_timer_active = 0;

  up_tmr_sync_down();

  return OK;
}

/****************************************************************************
 * Name: up_timer_start
 *
 * Description:
 *   Start the interval timer.  sched_timer_expiration() will be
 *   called at the completion of the timeout (unless up_timer_cancel
 *   is called to stop the timing.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the time interval until sched_timer_expiration() is
 *        called.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_alarm_start(FAR const struct timespec *ts)
{
  uint64_t ticks;

  up_tmr_sync_up();

  up_unmask_tmr();

  ticks = up_ts2tick(ts) + g_start_tsc;

  write_msr(IA32_TSC_DEADLINE, ticks);

  g_timer_active = 1;

  g_goal_time_ts.tv_sec = ts->tv_sec;
  g_goal_time_ts.tv_nsec = ts->tv_nsec;

  up_tmr_sync_down();

  tmrinfo("%d.%09d\n", ts->tv_sec, ts->tv_nsec);
  tmrinfo("start\n");

  return OK;
}

/****************************************************************************
 * Name: up_timer_update
 *
 * Description:
 *   Called as the IRQ handler for alarm expiration.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_alarm_expire(void)
{
  struct timespec now;

  up_mask_tmr();
  tmrinfo("expire\n");

  g_timer_active = 0;

  up_timer_gettime(&now);

  nxsched_alarm_expiration(&now);

  return;
}

#endif /* CONFIG_SCHED_TICKLESS_ALARM */
#endif /* CONFIG_SCHED_TICKLESS */
