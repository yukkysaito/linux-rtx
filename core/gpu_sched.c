#include <resch-gpu-core.h>
#include <linux/sched.h>

#if 0
#define SCHED_YIELD() yield()
#else
#define SCHED_YIELD() 
#endif

/* increment the counter for # of contexts accessing the device. */
void gdev_access_start(struct gdev_device *gdev)
{
	struct gdev_device *phys = gdev_phys_get(gdev);
	
retry:
	if (phys) {
		gdev_lock(&phys->global_lock);
		if (phys->blocked) {
			gdev_unlock(&phys->global_lock);
			SCHED_YIELD();
			goto retry;
		}
		phys->accessed++;
		gdev_unlock(&phys->global_lock);
	}
	else {
		gdev_lock(&gdev->global_lock);
		if (gdev->blocked) {
			gdev_unlock(&gdev->global_lock);
			SCHED_YIELD();
			goto retry;
		}
		gdev->accessed++;
		gdev_unlock(&gdev->global_lock);
	}
}

/* decrement the counter for # of contexts accessing the device. */
void gdev_access_end(struct gdev_device *gdev)
{
	struct gdev_device *phys = gdev_phys_get(gdev);
	if (phys) {
		gdev_lock(&phys->global_lock);
		phys->accessed--;
		gdev_unlock(&phys->global_lock);
	}
	else {
		gdev_lock(&gdev->global_lock);
		gdev->accessed--;
		gdev_unlock(&gdev->global_lock);
	}
}



int all_count = 0;
/**
 * schedule compute calls.
 */
void gdev_schedule_compute(struct gdev_sched_entity *se)
{
	struct gdev_device *gdev = se->gdev;

resched:
	/* algorithm-specific virtual device scheduler. */
	gdev_vsched->schedule_compute(se);
	
	/* local compute scheduler. */
	gdev_lock(&gdev->sched_com_lock);
	if ((gdev_current_com_get(gdev) && gdev_current_com_get(gdev) != se) || se->launch_instances >= GDEV_INSTANCES_LIMIT) {
		/* enqueue the scheduling entity to the compute queue. */
	    __gdev_enqueue_compute(gdev, se);
		gdev_unlock(&gdev->sched_com_lock);
		/* now the corresponding task will be suspended until some other tasks
		   will awaken it upon completions of their compute launches. */
		
		se->task = current; 
		
		RESCH_G_PRINT("Go to Sleep ctx#%d\n", se->ctx);
		gdev_sched_sleep(se);

		goto resched;
	}
	else {
		/* now, let's get offloaded to the device! */
		if (se->launch_instances == 0) {
			/* record the start time. */
			gdev_time_stamp(&se->last_tick_com);
		}
		se->launch_instances++;
		gdev_current_com_set(gdev, (void*)se);
		gdev_unlock(&gdev->sched_com_lock);
	}
	
	/* this function call will block any new contexts to be created during
	   the busy period on the GPU. */
	gdev_access_start(gdev);

	RESCH_G_PRINT("Go to Launch ctx#%d\n",se->ctx);
}

/**
 * schedule the next context of compute.
 * invoked upon the completion of preceding contexts.
 */
void gdev_select_next_compute(struct gdev_device *gdev)
{
	struct gdev_sched_entity *se;
	struct gdev_device *next;
	struct gdev_time now, exec;
	int old_instance = 0;
	
	/* now new contexts are allowed to be created as the GPU is idling. */
	gdev_access_end(gdev);
	gdev_lock(&gdev->sched_com_lock);
	se = (struct gdev_sched_entity *)gdev_current_com_get(gdev);
	if (!se) {
		gdev_unlock(&gdev->sched_com_lock);
		RESCH_G_PRINT("Invalid scheduling entity on Gdev#%d:0x%lx\n", gdev->id,gdev);
		return;
	}

	/* record the end time (update on multiple launches too). */
	gdev_time_stamp(&now);
	/* aquire the execution time. */
	gdev_time_sub(&exec, &now, &se->last_tick_com);
	
	se->launch_instances--;
	
	if (se->launch_instances == 0) {
		/* account for the credit. */
		gdev_time_sub(&gdev->credit_com, &gdev->credit_com, &exec);
		/* accumulate the computation time. */
		gdev->com_time += gdev_time_to_us(&exec);

		/* select the next context to be scheduled.
		   now don't reference the previous entity by se. */
		se = gdev_list_container(gdev_list_head(&gdev->sched_com_list));
		
		if(se)
		    old_instance   = se->launch_instances;
		   
		/* setting the next entity here prevents lower-priority contexts 
		   arriving in gdev_schedule_compute() from being dispatched onto
		   the device. note that se = NULL could happen. */
		gdev_current_com_set( gdev, (void*)se);
		gdev_unlock(&gdev->sched_com_lock);

		/* select the next device to be scheduled. */
		next = gdev_vsched->select_next_compute(gdev);
	
		if (!next){
		    return;
		}

		gdev_lock(&next->sched_com_lock);
		/* if the virtual device needs to be switched, change the next
		   scheduling entity to be scheduled also needs to be changed. */
		if (next != gdev) {
		        if ( se ){
			    if(old_instance < se->launch_instances)
				se->launch_instances--;
			}else{
			    se = gdev_current_com_get(gdev);
			    if(se){
				se->launch_instances--;
			    }
			}
		    	gdev_current_com_set( gdev, NULL);
			se = gdev_list_container(gdev_list_head(&next->sched_com_list));
		}

		/* now remove the scheduling entity from the waiting list, and wake 
		   up the corresponding task. */
		if (se) {
		    __gdev_dequeue_compute(se);
		    gdev_unlock(&next->sched_com_lock);

		    if (gdev_sched_wakeup(se) < 0) {
			RESCH_G_PRINT("Failed to wake up context 0x%lx\n", se->ctx);
			RESCH_G_PRINT("Perhaps context %d is already up\n", se->ctx);
		    }

		}
		else
		    gdev_unlock(&next->sched_com_lock);
	}
	else{
	    gdev_unlock(&gdev->sched_com_lock);
	}
}

/**
 * automatically replenish the credit of compute launches.
 */
void gdev_replenish_credit_compute(struct gdev_device *gdev)
{
	gdev_vsched->replenish_compute(gdev);
}

/**
 * schedule memcpy-copy calls.
 */
void gdev_schedule_memory(struct gdev_sched_entity *se)
{
	struct gdev_device *gdev = se->gdev;

#ifndef GDEV_SCHED_MRQ
	gdev_schedule_compute(se);
	return;
#endif

resched:
	/* algorithm-specific virtual device scheduler. */
	gdev_vsched->schedule_memory(se);

	/* local memory scheduler. */
	gdev_lock(&gdev->sched_mem_lock);
	if ((gdev->current_mem && gdev->current_mem != se) || se->memcpy_instances >= GDEV_INSTANCES_LIMIT) {
		/* enqueue the scheduling entity to the memory queue. */
		__gdev_enqueue_memory(gdev, se);
		gdev_unlock(&gdev->sched_mem_lock);

		/* now the corresponding task will be suspended until some other tasks
		   will awaken it upon completions of their memory transfers. */
		gdev_sched_sleep(se);

		goto resched;
	}
	else {
		/* now, let's get offloaded to the device! */
		if (se->memcpy_instances == 0) {
			/* record the start time. */
			gdev_time_stamp(&se->last_tick_mem);
		}
		se->memcpy_instances++;
		gdev->current_mem = (void*)se;
		gdev_unlock(&gdev->sched_mem_lock);
	}

//	gdev_access_start(gdev);
}

/**
 * schedule the next context of memory copy.
 * invoked upon the completion of preceding contexts.
 */
void gdev_select_next_memory(struct gdev_device *gdev)
{
	struct gdev_sched_entity *se;
	struct gdev_device *next;
	struct gdev_time now, exec;

#ifndef GDEV_SCHED_MRQ
	gdev_select_next_compute(gdev);
	return;
#endif

//	gdev_access_end(gdev);

	gdev_lock(&gdev->sched_mem_lock);
	se = (struct gdev_sched_entity *)gdev->current_mem;
	if (!se) {
		gdev_unlock(&gdev->sched_mem_lock);
		RESCH_G_PRINT("Invalid scheduling entity on Gdev#%d\n", gdev->id);
		return;
	}

	/* record the end time (update on multiple launches too). */
	gdev_time_stamp(&now);
	/* aquire the execution time. */
	gdev_time_sub(&exec, &now, &se->last_tick_mem);

	se->memcpy_instances--;
	if (se->memcpy_instances == 0) {
		/* account for the credit. */
		gdev_time_sub(&gdev->credit_mem, &gdev->credit_mem, &exec);
		/* accumulate the memory transfer time. */
		gdev->mem_time += gdev_time_to_us(&exec);

		/* select the next context to be scheduled.
		   now don't reference the previous entity by se. */
		se = gdev_list_container(gdev_list_head(&gdev->sched_mem_list));
		/* setting the next entity here prevents lower-priority contexts 
		   arriving in gdev_schedule_memory() from being dispatched onto
		   the device. note that se = NULL could happen. */
		gdev->current_mem = (void*)se; 
		gdev_unlock(&gdev->sched_mem_lock);

		/* select the next device to be scheduled. */
		next = gdev_vsched->select_next_memory(gdev);
		if (!next)
			return;

		gdev_lock(&next->sched_mem_lock);
		/* if the virtual device needs to be switched, change the next
		   scheduling entity to be scheduled also needs to be changed. */
		if (next != gdev)
			se = gdev_list_container(gdev_list_head(&next->sched_mem_list));

		/* now remove the scheduling entity from the waiting list, and wake 
		   up the corresponding task. */
		if (se) {
			__gdev_dequeue_memory(se);
			gdev_unlock(&next->sched_mem_lock);

			while (gdev_sched_wakeup(se->task) < 0) {
				RESCH_G_PRINT("Failed to wake up context %d\n", se);
			}
		}
		else
			gdev_unlock(&next->sched_mem_lock);
	}
	else
		gdev_unlock(&gdev->sched_mem_lock);
}

/**
 * automatically replenish the credit of memory copies.
 */
void gdev_replenish_credit_memory(struct gdev_device *gdev)
{
#ifdef GDEV_SCHED_MRQ
	gdev_vsched->replenish_memory(gdev);
#endif
}

