/****************************************************************************
 * sched/task_exithook.c
 *
 *   Copyright (C) 2011-2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/sched.h>
#include <nuttx/fs/fs.h>

#include "os_internal.h"
#include "sig_internal.h"

/****************************************************************************
 * Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Type Declarations
 ****************************************************************************/

/****************************************************************************
 * Global Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: task_atexit
 *
 * Description:
 *   Call any registerd atexit function(s)
 *
 ****************************************************************************/

#if defined(CONFIG_SCHED_ATEXIT) && !defined(CONFIG_SCHED_ONEXIT)
static inline void task_atexit(FAR _TCB *tcb)
{
#if defined(CONFIG_SCHED_ATEXIT_MAX) && CONFIG_SCHED_ATEXIT_MAX > 1
  int index;

  /* Call each atexit function in reverse order of registration  atexit()
   * functions are registered from lower to higher arry indices; they must
   * be called in the reverse order of registration when task exists, i.e.,
   * from higher to lower indices.
   */

  for (index = CONFIG_SCHED_ATEXIT_MAX-1; index >= 0; index--)
    {
      if (tcb->atexitfunc[index])
        {
          /* Call the atexit function */

          (*tcb->atexitfunc[index])();

          /* Nullify the atexit function.  task_exithook may be called more then
           * once in most task exit scenarios.  Nullifying the atext function
           * pointer will assure that the callback is performed only once.
           */

          tcb->atexitfunc[index] = NULL;
        }
    }

#else
  if (tcb->atexitfunc)
    {
      /* Call the atexit function */

      (*tcb->atexitfunc)();

      /* Nullify the atexit function.  task_exithook may be called more then
       * once in most task exit scenarios.  Nullifying the atext function
       * pointer will assure that the callback is performed only once.
       */

      tcb->atexitfunc = NULL;
    }
#endif
}
#else
#  define task_atexit(tcb)
#endif

/****************************************************************************
 * Name: task_onexit
 *
 * Description:
 *   Call any registerd on)exit function(s)
 *
 ****************************************************************************/
 
#ifdef CONFIG_SCHED_ONEXIT
static inline void task_onexit(FAR _TCB *tcb, int status)
{
#if defined(CONFIG_SCHED_ONEXIT_MAX) && CONFIG_SCHED_ONEXIT_MAX > 1
  int index;

  /* Call each on_exit function in reverse order of registration.  on_exit()
   * functions are registered from lower to higher arry indices; they must
   * be called in the reverse order of registration when task exists, i.e.,
   * from higher to lower indices.
   */

  for (index = CONFIG_SCHED_ONEXIT_MAX-1; index >= 0; index--)
    {
      if (tcb->onexitfunc[index])
        {
          /* Call the on_exit function */

          (*tcb->onexitfunc[index])(status, tcb->onexitarg[index]);

          /* Nullify the on_exit function.  task_exithook may be called more then
           * once in most task exit scenarios.  Nullifying the atext function
           * pointer will assure that the callback is performed only once.
           */

          tcb->onexitfunc[index] = NULL;
        }
    }
#else
  if (tcb->onexitfunc)
    {
      /* Call the on_exit function */

      (*tcb->onexitfunc)(status, tcb->onexitarg);

      /* Nullify the on_exit function.  task_exithook may be called more then
       * once in most task exit scenarios.  Nullifying the on_exit function
       * pointer will assure that the callback is performed only once.
       */

      tcb->onexitfunc = NULL;
    }
#endif
}
#else
#  define task_onexit(tcb,status)
#endif

/****************************************************************************
 * Name: task_sigchild
 *
 * Description:
 *   Send the SIGCHILD signal to the parent thread
 *
 ****************************************************************************/

#ifdef CONFIG_SCHED_HAVE_PARENT
static inline void task_sigchild(FAR _TCB *tcb, int status)
{
  FAR _TCB *ptcb;
  siginfo_t info;

  /* Only exiting tasks should generate SIGCHLD. pthreads use other
   * mechansims.
   */

  if ((tcb->flags & TCB_FLAG_TTYPE_MASK) == TCB_FLAG_TTYPE_TASK)
    {
      /* Keep things stationary through the following */

      sched_lock();

      /* Get the TCB of the receiving task */

      ptcb = sched_gettcb(tcb->parent);
      if (!ptcb)
        {
          /* The parent no longer exists... bail */

          sched_unlock();
          return;
        }

#ifdef CONFIG_SCHED_CHILD_STATUS
      /* Check if the parent task has suppressed retention of child exit
       * status information.  Only 'tasks' report exit status, not pthreads.
       * pthreads have a different mechanism.
       */

      if ((ptcb->flags & TCB_FLAG_NOCLDWAIT) == 0)
        {
          FAR struct child_status_s *child;

          /* No.. Find the exit status entry for this task in the parent TCB */

          child = task_findchild(ptcb, getpid());
          DEBUGASSERT(child);
          if (child)
            {
              /* Mark that the child has exit'ed */

              child->ch_flags |= CHILD_FLAG_EXITED;

              /* Save the exit status */

              child->ch_status = status;
            }
        }
#else
      /* Decrement the number of children from this parent */

      DEBUGASSERT(ptcb->nchildren > 0);
      ptcb->nchildren--;
#endif

      /* Set the parent to an impossible PID.  We do this because under
       * certain conditions, task_exithook() can be called multiple times.
       * If this function is called again, sched_gettcb() will fail on the
       * invalid parent PID above, nchildren will be decremented once and
       * all will be well.
       */

      tcb->parent = INVALID_PROCESS_ID;

      /* Create the siginfo structure.  We don't actually know the cause.
       * That is a bug. Let's just say that the child task just exit-ted
       * for now.
       */

      info.si_signo           = SIGCHLD;
      info.si_code            = CLD_EXITED;
      info.si_value.sival_ptr = NULL;
      info.si_pid             = tcb->pid;
      info.si_status          = status;

      /* Send the signal.  We need to use this internal interface so that we
       * can provide the correct si_code value with the signal.
       */

      (void)sig_received(ptcb, &info);
      sched_unlock();
    }
}
#else
#  define task_sigchild(tcb,status)
#endif

/****************************************************************************
 * Name: task_exitwakeup
 *
 * Description:
 *   Wakeup any tasks waiting for this task to exit
 *
 ****************************************************************************/

#if defined(CONFIG_SCHED_WAITPID) && !defined(CONFIG_SCHED_HAVE_PARENT)
static inline void task_exitwakeup(FAR _TCB *tcb, int status)
{
  /* Wakeup any tasks waiting for this task to exit */

  while (tcb->exitsem.semcount < 0)
    {
      /* "If more than one thread is suspended in waitpid() awaiting
       *  termination of the same process, exactly one thread will return
       *  the process status at the time of the target process termination." 
       *  Hmmm.. what do we return to the others?
       */

      if (tcb->stat_loc)
        {
          *tcb->stat_loc = status << 8;
           tcb->stat_loc = NULL;
        }

      /* Wake up the thread */

      sem_post(&tcb->exitsem);
    }
}
#else
#  define task_exitwakeup(tcb, status)
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: task_hook
 *
 * Description:
 *   This function implements some of the internal logic of exit() and
 *   task_delete().  This function performs some cleanup and other actions
 *   required when a task exists:
 *
 *   - All open streams are flushed and closed.
 *   - All functions registered with atexit() and on_exit() are called, in
 *     the reverse order of their registration.
 *
 *   When called from exit(), the tcb still resides at the head of the ready-
 *   to-run list.  The following logic is safe because we will not be
 *   returning from the exit() call.
 *
 *   When called from task_delete() we are operating on a different thread;
 *   on the thread that called task_delete().  In this case, task_delete
 *   will have already removed the tcb from the ready-to-run list to prevent
 *   any further action on this task.
 *
 ****************************************************************************/

void task_exithook(FAR _TCB *tcb, int status)
{
  /* If exit function(s) were registered, call them now before we do any un-
   * initialization.  NOTE:  In the case of task_delete(), the exit function
   * will *not* be called on the thread execution of the task being deleted!
   */

  task_atexit(tcb);

  /* Call any registered on_exit function(s) */

  task_onexit(tcb, status);

  /* Send SIGCHLD to the parent of the exit-ing task */

  task_sigchild(tcb, status);

  /* Wakeup any tasks waiting for this task to exit */

  task_exitwakeup(tcb, status);

  /* Flush all streams (File descriptors will be closed when
   * the TCB is deallocated).
   */

#if CONFIG_NFILE_STREAMS > 0
  (void)lib_flushall(tcb->streams);
#endif

  /* Discard any un-reaped child status (no zombies here!) */

#if defined(CONFIG_SCHED_HAVE_PARENT) && defined(CONFIG_SCHED_CHILD_STATUS)
  task_removechildren(tcb);
#endif

  /* Free all file-related resources now.  This gets called again
   * just be be certain when the TCB is delallocated. However, we
   * really need to close files as soon as possible while we still
   * have a functioning task.
   */

 (void)sched_releasefiles(tcb);
 
  /* Deallocate anything left in the TCB's queues */

#ifndef CONFIG_DISABLE_SIGNALS
  sig_cleanup(tcb); /* Deallocate Signal lists */
#endif
}
