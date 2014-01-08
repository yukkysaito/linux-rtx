/*
 * sched_deadline.c		Copyright (C) Shinpei Kato
 *
 * EDF scheduler implementation for RESCH.
 * This implementation is used when SCHED_DEADLINE is enabled.
 */

#include <resch-core.h>
#include "sched.h"

#ifndef CONFIG_AIRS
#define SCHED_FCBS 				0x20000000
#define SCHED_FCBS_NO_CATCH_UP	0x10000000
#define SCHED_EXHAUSTIVE 		0x08000000
#endif

/* prototypes for the kernel functions. */
int sched_setscheduler_ex(struct task_struct *p, int policy,
						  struct sched_param *param,
						  struct sched_param_ex *param_ex);
int sched_wait_interval(int flags,
						const struct timespec __user * rqtp,
						struct timespec __user * rmtp);

/**
 * set the scheduler internally in the Linux kernel.
 */
static int edf_set_scheduler(resch_task_t *rt, int prio)
{
	struct sched_param sp;
	struct sched_param_ex spx;
	struct timespec ts_period, ts_deadline, ts_runtime;

	jiffies_to_timespec(rt->period, &ts_period);
	jiffies_to_timespec(rt->deadline, &ts_deadline);
	jiffies_to_timespec(usecs_to_jiffies(rt->runtime), &ts_runtime);
	sp.sched_priority = 0;
	spx.sched_priority = 0;
	spx.sched_period = ts_period;
	spx.sched_deadline = ts_deadline;
	spx.sched_runtime = ts_runtime;
	spx.sched_flags = 0;
	if (sched_setscheduler_ex(rt->task, SCHED_DEADLINE, &sp, &spx) < 0) {
		printk(KERN_WARNING "RESCH: edf_set_scheduler() failed.\n");
		printk(KERN_WARNING "RESCH: task#%d (process#%d) priority=%d.\n",
			   rt->rid, rt->task->pid, prio);
		return false;
	}

	rt->prio = prio;

	if (task_has_reserve(rt)) {
		rt->task->dl.flags &= ~SCHED_EXHAUSTIVE;
		rt->task->dl.flags |= SCHED_FCBS;
		/* you can additionally set the following flags, if wanted.
		   rt->task->dl.flags |= SCHED_FCBS_NO_CATCH_UP; */
	}
	else {
		rt->task->dl.flags |= SCHED_EXHAUSTIVE;
		rt->task->dl.flags &= ~SCHED_FCBS;
	}

	return true;
}

/**
 * insert the given task into the active queue according to the EDF policy.
 * the active queue must be locked.
 */
static void edf_enqueue_task(resch_task_t *rt, int prio, int cpu)
{
	int idx = prio_index(prio);
	struct prio_array *active = &lo[cpu].active;
	struct list_head *queue = &active->queue[idx];
	resch_task_t *p;

	if (list_empty(queue)) {
		list_add_tail(&rt->active_entry, queue);
		goto out;
	}
	else {
		list_for_each_entry(p, queue, active_entry) {
			if (rt->deadline_time < p->deadline_time) {
				/* insert @rt before @p. */
				list_add_tail(&rt->active_entry, &p->active_entry);
				goto out;
			}
		}
		list_add_tail(&rt->active_entry, queue);
	}
 out:
	__set_bit(idx, active->bitmap);
	active->nr_tasks++;
}

/**
 * remove the given task from the active queue.
 * the active queue must be locked.
 */
static void edf_dequeue_task(resch_task_t *rt, int prio, int cpu)
{
	int idx = prio_index(prio);
	struct prio_array *active = &lo[cpu].active;
	struct list_head *queue = &active->queue[idx];

	list_del_init(&rt->active_entry);
	if (list_empty(queue)) {
		__clear_bit(idx, active->bitmap);
	}
	active->nr_tasks--;
}

static void edf_job_start(resch_task_t *rt)
{
	unsigned long flags;
	int cpu = rt->cpu_id;

	active_queue_lock(cpu, &flags);
	edf_enqueue_task(rt, RESCH_PRIO_EDF, cpu);
	active_queue_unlock(cpu, &flags);
}

/**
 * complete the current job of the given task.
 */
static void edf_job_complete(resch_task_t *rt)
{
	int cpu = rt->cpu_id;
	unsigned long flags;

	active_queue_lock(cpu, &flags);
	edf_dequeue_task(rt, RESCH_PRIO_EDF, cpu);
	active_queue_unlock(cpu, &flags);
}

/**
 * start the accounting on the reserve of the given task in SCHED_DEADLINE.
 * if the task is properly requested to reserve CPU time through the API,
 * we do nothing here, since it is handled by SCHED_DEADLINE.
 * otherwise, we consider this is a request to forcefully account CPU time.
 * the latter case is useful if the kernel functions want to account CPU
 * time, but do not want to use the CBS policy.
 */
static void edf_start_account(resch_task_t *rt)
{
	/* set the budget explicitly, only if the task is not requested to
	   reserve CPU time through the API. */
	if (!task_has_reserve(rt)) {
		struct timespec ts;
		jiffies_to_timespec(rt->budget, &ts);
		rt->task->dl.runtime = timespec_to_ns(&ts);
	}

	/* set the flag to notify applications when the budget is exhausted. */
	if (rt->xcpu) {
		rt->task->dl.flags |= SCHED_SIG_RORUN;
	}

	/* make sure to use Flexible CBS. */
	rt->task->dl.flags &= ~SCHED_EXHAUSTIVE;
	rt->task->dl.flags |= SCHED_FCBS;
}

/**
 * stop the accounting on the reserve of the given task in SCHED_DEADLINE.
 */
static void edf_stop_account(resch_task_t *rt)
{
	rt->task->dl.flags |= SCHED_EXHAUSTIVE;
	rt->task->dl.flags &= ~SCHED_FCBS;

	if (rt->xcpu) {
		rt->task->dl.flags &= ~SCHED_SIG_RORUN;
		rt->task->dl.flags &= ~DL_RORUN;
	}
}

/**
 * we dont have to do anything for reserve expiration, since it is handled
 * by SCHED_DEADLINE.
 */
static void edf_reserve_expire(resch_task_t *rt)
{
	/* nothing to do. */
}

/**
 * we dont have to do anything for reserve replenishment, since it is handled
 * by SCHED_DEADLINE.
 */
static void edf_reserve_replenish(resch_task_t *rt, unsigned long cputime)
{
	/* nothing to do. */
}

/**
 * migrate @rt to the given CPU. 
 */
static void edf_migrate_task(resch_task_t *rt, int cpu_dst)
{
	unsigned long flags;
	int cpu_src = rt->cpu_id;

	if (cpu_src != cpu_dst) {
		active_queue_double_lock(cpu_src, cpu_dst, &flags);
		if (task_is_active(rt)) {
#ifdef RESCH_PREEMPT_TRACE
			/* trace preemption. */
			preempt_out(rt);
#endif
			/* move off the source CPU. */
			edf_dequeue_task(rt, RESCH_PRIO_EDF, cpu_src);

			/* move on the destination CPU. */
			rt->cpu_id = cpu_dst; 
			edf_enqueue_task(rt, RESCH_PRIO_EDF, cpu_dst);

#ifdef RESCH_PREEMPT_TRACE
			/* trace preemption. */
			preempt_in(rt);
#endif
			active_queue_double_unlock(cpu_src, cpu_dst, &flags);

			__migrate_task(rt, cpu_dst);

			/* restart accounting on the new CPU. */
			if (task_is_accounting(rt)) {
				edf_stop_account(rt);
				edf_start_account(rt);
			}
		}
		else {
			rt->cpu_id = cpu_dst;
			active_queue_double_unlock(cpu_src, cpu_dst, &flags);
			__migrate_task(rt, cpu_dst);
		}
	}
	else {
		__migrate_task(rt, cpu_dst);
	}
}

/**
 * wait until the next period.
 */
static void edf_wait_period(resch_task_t *rt)
{
	struct timespec ts_period;
	if (rt->release_time > jiffies) {
		jiffies_to_timespec(rt->release_time - jiffies, &ts_period);
	}
	else {
		ts_period.tv_sec = 0;
		ts_period.tv_nsec = 0;
	}

	if (rt->task->dl.flags & SCHED_EXHAUSTIVE) {
		rt->task->dl.deadline = cpu_clock(smp_processor_id());
	}
	sched_wait_interval(!TIMER_ABSTIME, &ts_period, NULL);
}

/* EDF scheduling class. */
static const struct resch_sched_class edf_sched_class = {
	.set_scheduler		= edf_set_scheduler,
	.enqueue_task		= edf_enqueue_task,
	.dequeue_task		= edf_dequeue_task,
	.job_start			= edf_job_start,
	.job_complete		= edf_job_complete,
	.start_account		= edf_start_account,
	.stop_account		= edf_stop_account,
	.reserve_expire		= edf_reserve_expire,
	.reserve_replenish	= edf_reserve_replenish,
	.migrate_task		= edf_migrate_task,
	.wait_period		= edf_wait_period,
};
