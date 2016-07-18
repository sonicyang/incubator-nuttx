/****************************************************************************
 * arch/rgmp/src/nuttx.c
 *
 *   Copyright (C) 2011 Yu Qiang. All rights reserved.
 *   Author: Yu Qiang <yuq825@gmail.com>
 *
 * This file is a part of NuttX:
 *
 *   Copyright (C) 2011, 2014 Gregory Nutt. All rights reserved.
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

#include <rgmp/boot.h>
#include <rgmp/cxx.h>
#include <rgmp/memlayout.h>
#include <rgmp/allocator.h>
#include <rgmp/assert.h>
#include <rgmp/string.h>
#include <rgmp/arch/arch.h>

#include <stdio.h>
#include <stdlib.h>
#include <arch/irq.h>
#include <arch/arch.h>

#include <nuttx/sched.h>
#include <nuttx/kmalloc.h>
#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/sched_note.h>
#include <nuttx/serial/pty.h>
#include <nuttx/syslog/syslog.h>
#include <nuttx/crypto/crypto.h>
#include <nuttx/power/pm.h>

#include "task/task.h"
#include "sched/sched.h"
#include "group/group.h"

struct tcb_s *current_task = NULL;

/* This function is called in non-interrupt context
 * to switch tasks.
 * Assumption: global interrupt is disabled.
 */

static inline void up_switchcontext(struct tcb_s *ctcb, struct tcb_s *ntcb)
{
  // do nothing if two tasks are the same

  if (ctcb == ntcb)
    return;

  // this function can not be called in interrupt

  if (up_interrupt_context()) {
     panic("%s: try to switch context in interrupt\n", __func__);
  }

  // start switch

  current_task = ntcb;
  rgmp_context_switch(ctcb ? &ctcb->xcp.ctx : NULL, &ntcb->xcp.ctx);
}

void up_initialize(void)
{
  extern pidhash_t g_pidhash[];
  extern void vdev_init(void);
  extern void nuttx_arch_init(void);

  /* Initialize the current_task to g_idletcb */

  current_task = g_pidhash[PIDHASH(0)].tcb;

  /* OS memory alloc system is ready */

  use_os_kmalloc = 1;

  /* rgmp vdev init */

  vdev_init();

  nuttx_arch_init();

#ifdef CONFIG_PM
  /* Initialize the power management subsystem.  This MCU-specific function
   * must be called *very* early in the initialization sequence *before* any
   * other device drivers are initialized (since they may attempt to register
   * with the power management subsystem).
   */

  up_pminitialize();
#endif

#if CONFIG_NFILE_DESCRIPTORS > 0 && defined(CONFIG_PSEUDOTERM_SUSV1)
  /* Register the master pseudo-terminal multiplexor device */

  (void)ptmx_register();
#endif

  /* Early initialization of the system logging device.  Some SYSLOG channel
   * can be initialized early in the initialization sequence because they
   * depend on only minimal OS initialization.
   */

  syslog_initialize(SYSLOG_INIT_EARLY);

  /* Register devices */

#if CONFIG_NFILE_DESCRIPTORS > 0

#if defined(CONFIG_DEV_NULL)
  devnull_register();   /* Standard /dev/null */
#endif

#if defined(CONFIG_DEV_URANDOM)
  devurandom_register();   /* Standard /dev/urandom */
#endif

#if defined(CONFIG_DEV_ZERO)
  devzero_register();   /* Standard /dev/zero */
#endif

#if defined(CONFIG_DEV_LOOP)
  loop_register();      /* Standard /dev/loop */
#endif
#endif /* CONFIG_NFILE_DESCRIPTORS */

#if defined(CONFIG_SCHED_INSTRUMENTATION_BUFFER) && \
    defined(CONFIG_DRIVER_NOTE)
  note_register();      /* Non-standard /dev/note */
#endif

#if defined(CONFIG_CRYPTO)
  /* Initialize the HW crypto and /dev/crypto */

  up_cryptoinitialize();
#endif

#if CONFIG_NFILE_DESCRIPTORS > 0 && defined(CONFIG_CRYPTO_CRYPTODEV)
  devcrypto_register();
#endif

#ifdef CONFIG_DEV_RANDOM
  /* Initialize the Random Number Generator (RNG)  */

  devrandom_register();
#endif

  /* Enable interrupt */

  local_irq_enable();
}

void up_idle(void)
{
    arch_hlt();
}

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
    void *boot_freemem = boot_alloc(0, sizeof(int));
    *heap_start = boot_freemem;
    *heap_size = KERNBASE + kmem_size - (uint32_t)boot_freemem;
}

int up_create_stack(struct tcb_s *tcb, size_t stack_size, uint8_t ttype)
{
    uint32_t *stack_alloc_ptr;
    int ret = ERROR;
    size_t *adj_stack_ptr;

    /* Move up to next even word boundary if necessary */

    size_t adj_stack_size = (stack_size + 3) & ~3;
    size_t adj_stack_words = adj_stack_size >> 2;

    /* Allocate the memory for the stack */

#if defined(CONFIG_BUILD_KERNEL) && defined(CONFIG_MM_KERNEL_HEAP)
    /* Use the kernel allocator if this is a kernel thread */

    if (ttype == TCB_FLAG_TTYPE_KERNEL) {
        stack_alloc_ptr = (uint32_t *)kmm_malloc(stack_size);
    } else
#endif
    {
        stack_alloc_ptr = (uint32_t*)kumm_malloc(adj_stack_size);
    }
    if (stack_alloc_ptr) {
        /* This is the address of the last word in the allocation */

        adj_stack_ptr = &stack_alloc_ptr[adj_stack_words - 1];

        /* Save the values in the TCB */

        tcb->adj_stack_size  = adj_stack_size;
        tcb->stack_alloc_ptr = stack_alloc_ptr;
        tcb->adj_stack_ptr   = (void *)((unsigned int)adj_stack_ptr & ~7);
        ret = OK;
    }
    return ret;
}

int up_use_stack(struct tcb_s *tcb, void *stack, size_t stack_size)
{
    /* Move up to next even word boundary if necessary */

    size_t adj_stack_size = stack_size & ~3;
    size_t adj_stack_words = adj_stack_size >> 2;

    /* This is the address of the last word in the allocation */

    size_t *adj_stack_ptr = &((size_t*)stack)[adj_stack_words - 1];

    /* Save the values in the TCB */

    tcb->adj_stack_size  = adj_stack_size;
    tcb->stack_alloc_ptr = stack;
    tcb->adj_stack_ptr   = (void *)((unsigned int)adj_stack_ptr & ~7);
    return OK;
}

FAR void *up_stack_frame(FAR struct tcb_s *tcb, size_t frame_size)
{
  uintptr_t topaddr;

  /* Align the frame_size */

  frame_size = (frame_size + 3) & ~3;

  /* Is there already a stack allocated? Is it big enough? */

  if (!tcb->stack_alloc_ptr || tcb->adj_stack_size <= frame_size) {
      return NULL;
  }

  /* Save the adjusted stack values in the struct tcb_s */

  topaddr              = (uintptr_t)tcb->adj_stack_ptr - frame_size;
  tcb->adj_stack_ptr   = (FAR void *)topaddr;
  tcb->adj_stack_size -= frame_size;

  /* Reset the initial state */

  up_initial_state(tcb);

  /* And return a pointer to the allocated memory region */

  return (FAR void *)(topaddr + sizeof(uint32_t));
}

void up_release_stack(struct tcb_s *dtcb, uint8_t ttype)
{
  /* Is there a stack allocated? */

  if (dtcb->stack_alloc_ptr) {
#if defined(CONFIG_BUILD_KERNEL) && defined(CONFIG_MM_KERNEL_HEAP)
      /* Use the kernel allocator if this is a kernel thread */

      if (ttype == TCB_FLAG_TTYPE_KERNEL) {
          kmm_free(dtcb->stack_alloc_ptr);
      } else
#endif
      {
        /* Use the user-space allocator if this is a task or pthread */

        kumm_free(dtcb->stack_alloc_ptr);
      }
  }

  /* Mark the stack freed */

  dtcb->stack_alloc_ptr = NULL;
  dtcb->adj_stack_size  = 0;
  dtcb->adj_stack_ptr   = NULL;
}

/****************************************************************************
 * Name: up_block_task
 *
 * Description:
 *   The currently executing task at the head of
 *   the ready to run list must be stopped.  Save its context
 *   and move it to the inactive list specified by task_state.
 *
 *   This function is called only from the NuttX scheduling
 *   logic.  Interrupts will always be disabled when this
 *   function is called.
 *
 * Inputs:
 *   tcb: Refers to a task in the ready-to-run list (normally
 *     the task at the head of the list).  It most be
 *     stopped, its context saved and moved into one of the
 *     waiting task lists.  It it was the task at the head
 *     of the ready-to-run list, then a context to the new
 *     ready to run task must be performed.
 *   task_state: Specifies which waiting task list should be
 *     hold the blocked task TCB.
 *
 ****************************************************************************/

void up_block_task(struct tcb_s *tcb, tstate_t task_state)
{
  /* Verify that the context switch can be performed */

  if ((tcb->task_state < FIRST_READY_TO_RUN_STATE) ||
      (tcb->task_state > LAST_READY_TO_RUN_STATE))
    {
      _warn("%s: task sched error\n", __func__);
      return;
    }
  else
    {
      struct tcb_s *rtcb = current_task;
      bool switch_needed;

      /* Remove the tcb task from the ready-to-run list.  If we
       * are blocking the task at the head of the task list (the
       * most likely case), then a context switch to the next
       * ready-to-run task is needed. In this case, it should
       * also be true that rtcb == tcb.
       */

      switch_needed = sched_removereadytorun(tcb);

      /* Add the task to the specified blocked task list */

      sched_addblocked(tcb, (tstate_t)task_state);

      /* Now, perform the context switch if one is needed */

      if (switch_needed)
        {
          struct tcb_s *nexttcb;

          /* Update scheduler parameters */

          sched_suspend_scheduler(rtcb);

          /* this part should not be executed in interrupt context */

          if (up_interrupt_context())
            {
              panic("%s: %d\n", __func__, __LINE__);
            }

          /* If there are any pending tasks, then add them to the ready-to-run
           * task list now. It should be the up_realease_pending() called from
           * sched_unlock() to do this for disable preemption. But it block
           * itself, so it's OK.
           */

          if (g_pendingtasks.head)
            {
              _warn("Disable preemption failed for task block itself\n");
              sched_mergepending();
            }

          nexttcb = this_task();

#ifdef CONFIG_ARCH_ADDRENV
          /* Make sure that the address environment for the previously
           * running task is closed down gracefully (data caches dump,
           * MMU flushed) and set up the address environment for the new
           * thread at the head of the ready-to-run list.
           */

          (void)group_addrenv(nexttcb);
#endif
          /* Reset scheduler parameters */

          sched_resume_scheduler(nexttcb);

          /* context switch */

          up_switchcontext(rtcb, nexttcb);
        }
    }
}

/****************************************************************************
 * Name: up_unblock_task
 *
 * Description:
 *   A task is currently in an inactive task list
 *   but has been prepped to execute.  Move the TCB to the
 *   ready-to-run list, restore its context, and start execution.
 *
 * Inputs:
 *   tcb: Refers to the tcb to be unblocked.  This tcb is
 *     in one of the waiting tasks lists.  It must be moved to
 *     the ready-to-run list and, if it is the highest priority
 *     ready to run taks, executed.
 *
 ****************************************************************************/

void up_unblock_task(struct tcb_s *tcb)
{
  /* Verify that the context switch can be performed */

  if ((tcb->task_state < FIRST_BLOCKED_STATE) ||
      (tcb->task_state > LAST_BLOCKED_STATE))
    {
      _warn("%s: task sched error\n", __func__);
      return;
    }
  else
    {
      struct tcb_s *rtcb = current_task;

      /* Remove the task from the blocked task list */

      sched_removeblocked(tcb);

      /* Add the task in the correct location in the prioritized
       * ready-to-run task list.
       */

      if (sched_addreadytorun(tcb) && !up_interrupt_context())
        {
          /* The currently active task has changed! */
          /* Update scheduler parameters */

          sched_suspend_scheduler(rtcb);

          /* Are we in an interrupt handler? */

          struct tcb_s *nexttcb = this_task();

#ifdef CONFIG_ARCH_ADDRENV
          /* Make sure that the address environment for the previously
           * running task is closed down gracefully (data caches dump,
           * MMU flushed) and set up the address environment for the new
           * thread at the head of the ready-to-run list.

          (void)group_addrenv(nexttcb);
#endif
          /* Update scheduler parameters */

          sched_resume_scheduler(nexttcb);

          /* context switch */

          up_switchcontext(rtcb, nexttcb);
        }
    }
}

/* This function is called from sched_unlock() which will check not
 * in interrupt context and disable interrupt.
 */

void up_release_pending(void)
{
  struct tcb_s *rtcb = current_task;

  /* Merge the g_pendingtasks list into the ready-to-run task list */

  if (sched_mergepending())
    {
      struct tcb_s *nexttcb = this_task();

      /* The currently active task has changed!  We will need to switch
       * contexts.
       *
       * Update scheduler parameters.
       */

      sched_suspend_scheduler(rtcb);

#ifdef CONFIG_ARCH_ADDRENV
      /* Make sure that the address environment for the previously
       * running task is closed down gracefully (data caches dump,
       * MMU flushed) and set up the address environment for the new
       * thread at the head of the ready-to-run list.
       */

      (void)group_addrenv(nexttcb);
#endif
      /* Update scheduler parameters */

      sched_resume_scheduler(nexttcb);

      /* context switch */

      up_switchcontext(rtcb, nexttcb);
    }
}

void up_reprioritize_rtr(struct tcb_s *tcb, uint8_t priority)
{
  /* Verify that the caller is sane */

  if (tcb->task_state < FIRST_READY_TO_RUN_STATE ||
      tcb->task_state > LAST_READY_TO_RUN_STATE
#if SCHED_PRIORITY_MIN > UINT8_MIN
      || priority < SCHED_PRIORITY_MIN
#endif
#if SCHED_PRIORITY_MAX < UINT8_MAX
      || priority > SCHED_PRIORITY_MAX
#endif
      )
    {
      _warn("%s: task sched error\n", __func__);
      return;
    }
  else
    {
      struct tcb_s *rtcb = current_task;
      bool switch_needed;

      /* Remove the tcb task from the ready-to-run list.
       * sched_removereadytorun will return true if we just
       * remove the head of the ready to run list.
       */

      switch_needed = sched_removereadytorun(tcb);

      /* Setup up the new task priority */

      tcb->sched_priority = (uint8_t)priority;

      /* Return the task to the specified blocked task list.
       * sched_addreadytorun will return true if the task was
       * added to the new list.  We will need to perform a context
       * switch only if the EXCLUSIVE or of the two calls is non-zero
       * (i.e., one and only one the calls changes the head of the
       * ready-to-run list).
       */

      switch_needed ^= sched_addreadytorun(tcb);

      /* Now, perform the context switch if one is needed */

      if (switch_needed && !up_interrupt_context())
        {
          struct tcb_s *nexttcb;

          /* If there are any pending tasks, then add them to the ready-to-run
           * task list now. It should be the up_realease_pending() called from
           * sched_unlock() to do this for disable preemption. But it block
           * itself, so it's OK.
           */

          if (g_pendingtasks.head)
            {
              _warn("Disable preemption failed for reprioritize task\n");
              sched_mergepending();
            }

          /* Update scheduler parameters */

          sched_suspend_scheduler(rtcb);

          /* Get the TCB of the new task to run */

          nexttcb = this_task();

#ifdef CONFIG_ARCH_ADDRENV
          /* Make sure that the address environment for the previously
           * running task is closed down gracefully (data caches dump,
           * MMU flushed) and set up the address environment for the new
           * thread at the head of the ready-to-run list.
           */

          (void)group_addrenv(nexttcb);
#endif
          /* Update scheduler parameters */

          sched_resume_scheduler(nexttcb);

          /* context switch */

          up_switchcontext(rtcb, nexttcb);
        }
    }
}

void _exit(int status)
{
    struct tcb_s* tcb;

    /* Destroy the task at the head of the ready to run list. */

    (void)task_exit();

    /* Now, perform the context switch to the new ready-to-run task at the
     * head of the list.
     */

    tcb = this_task();

#ifdef CONFIG_ARCH_ADDRENV
    /* Make sure that the address environment for the previously running
     * task is closed down gracefully (data caches dump, MMU flushed) and
     * set up the address environment for the new thread at the head of
     * the ready-to-run list.
     */

    (void)group_addrenv(tcb);
#endif

    /* Then switch contexts */

    up_switchcontext(NULL, tcb);
}

void up_assert(const uint8_t *filename, int line)
{
    fprintf(stderr, "Assertion failed at file:%s line: %d\n", filename, line);

#ifdef CONFIG_BOARD_CRASHDUMP
    board_crashdump(up_getsp(), this_task(), filename, line);
#endif

    // in interrupt context or idle task means kernel error
    // which will stop the OS
    // if in user space just terminate the task
    if (up_interrupt_context() || current_task->pid == 0) {
        panic("%s: %d\n", __func__, __LINE__);
    }
    else {
        exit(EXIT_FAILURE);
    }
}

#ifndef CONFIG_DISABLE_SIGNALS

void up_schedule_sigaction(struct tcb_s *tcb, sig_deliver_t sigdeliver)
{
    /* Refuse to handle nested signal actions */
    if (!tcb->xcp.sigdeliver) {
        int flags;

        /* Make sure that interrupts are disabled */
        local_irq_save(flags);

        // First, handle some special cases when the signal is
        // being delivered to the currently executing task.
        if (tcb == current_task) {
            // CASE 1:  We are not in an interrupt handler and
            // a task is signalling itself for some reason.
            if (!up_interrupt_context()) {
                // In this case just deliver the signal now.
                sigdeliver(tcb);
            }
            // CASE 2:  We are in an interrupt handler AND the
            // interrupted task is the same as the one that
            // must receive the signal.
            else {
                tcb->xcp.sigdeliver = sigdeliver;
            }
        }

        // Otherwise, we are (1) signaling a task is not running
        // from an interrupt handler or (2) we are not in an
        // interrupt handler and the running task is signalling
        // some non-running task.
        else {
            tcb->xcp.sigdeliver = sigdeliver;
            push_xcptcontext(&tcb->xcp);
        }

        local_irq_restore(flags);
    }
}

#endif /* !CONFIG_DISABLE_SIGNALS */


bool up_interrupt_context(void)
{
    if (nest_irq)
        return true;
    return false;
}

#ifndef CONFIG_ARCH_NOINTC
void up_disable_irq(int irq)
{

}

void up_enable_irq(int irq)
{

}
#endif

#ifdef CONFIG_ARCH_IRQPRIO
int up_prioritize_irq(int irq, int priority)
{

}
#endif

void up_sigdeliver(struct Trapframe *tf)
{
    sig_deliver_t sigdeliver;

    pop_xcptcontext(&current_task->xcp);
    sigdeliver = current_task->xcp.sigdeliver;
    current_task->xcp.sigdeliver = NULL;
    local_irq_enable();
    sigdeliver(current_task);
    local_irq_disable();
}

#if defined(CONFIG_HAVE_CXX) && defined(CONFIG_HAVE_CXXINITIALIZE)

void up_cxxinitialize(void)
{
    rgmp_cxx_init();
}

#endif








