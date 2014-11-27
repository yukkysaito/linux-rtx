#if 0
#define SCHED_YIELD() yield()
#else
#define SCHED_YIELD() 
#endif

#define GDEV_VSCHED_BAND_SELECT_CHANCES 1

extern struct gdev_device gdev_vds[GDEV_DEVICE_MAX_COUNT];

static int __gdev_is_alone(struct gdev_device *gdev)
{
	struct gdev_device *phys = gdev_phys_get(gdev);
	struct gdev_device *p;
	int alone = 1;

	if (!phys)
		return alone;

	gdev_lock(&phys->sched_com_lock);
	gdev_list_for_each(p, &phys->sched_com_list, list_entry_com) {
		if ((p != gdev) && p->users) {
			alone = 0;
			break;
		}
	}
	gdev_unlock(&phys->sched_com_lock);

	return alone;
}

static void __gdev_vsched_band_yield_chance(void)
{
	struct gdev_time time_wait, time_now;
	gdev_time_stamp(&time_now);
	gdev_time_us(&time_wait, 500); /* 500 us */
	gdev_time_add(&time_wait, &time_wait, &time_now);
	while (gdev_time_lt(&time_now, &time_wait)) {
		SCHED_YIELD();
		gdev_time_stamp(&time_now);
	}
}

static void gdev_vsched_band_schedule_compute(struct gdev_sched_entity *se)
{
	struct gdev_device *gdev = se->gdev;
	struct gdev_device *phys = gdev_phys_get(gdev);

	if (!phys)
		return;

resched:
	/* yielding if necessary. */
	if (gdev_time_lez(&gdev->credit_com) && (gdev->com_bw_used > gdev->com_bw)
		&& !__gdev_is_alone(gdev)) {
		gdev_lock(&phys->sched_com_lock);
		
		if (gdev_current_com_get(phys)== gdev) {
			gdev_current_com_set(phys,NULL);
			gdev_unlock(&phys->sched_com_lock);

			__gdev_vsched_band_yield_chance();

			gdev_lock(&phys->sched_com_lock);
	
			
			if (gdev_current_com_get(gdev)== NULL)
				gdev_current_com_set(phys,gdev);
			gdev_unlock(&phys->sched_com_lock);
		}
		else
			gdev_unlock(&phys->sched_com_lock);
	}

	gdev_lock(&phys->sched_com_lock);

	if (gdev_current_com_get(phys)&& (gdev_current_com_get(phys)!= gdev)) {
		/* insert the scheduling entity to its local priority-ordered list. */
		gdev_lock_nested(&gdev->sched_com_lock);
		__gdev_enqueue_compute(gdev, se);
		gdev_unlock_nested(&gdev->sched_com_lock);
		gdev_unlock(&phys->sched_com_lock);

		RESCH_G_DPRINT("Gdev#%d Ctx#%d Sleep\n", gdev->id, se->ctx);

		se->task = current;
		/* now the corresponding task will be suspended until some other tasks
		   will awaken it upon completions of their compute launches. */
		gdev_sched_sleep(se);

		goto resched;
	}
	else {
		gdev_current_com_set(phys,(void *)gdev);
		gdev_unlock(&phys->sched_com_lock);
		RESCH_G_DPRINT("Gdev#%d Ctx#%d is ok!\n", gdev->id, se->ctx);
	}
}

static struct gdev_device *gdev_vsched_band_select_next_compute(struct gdev_device *gdev)
{
	struct gdev_device *phys = gdev_phys_get(gdev);
	struct gdev_device *next;
	int chances = GDEV_VSCHED_BAND_SELECT_CHANCES;


	if (!phys)
		return gdev;

	RESCH_G_DPRINT("Gdev#%d Complete\n", gdev->id);
retry:
	gdev_lock(&phys->sched_com_lock);

	/* if the credit is exhausted, reinsert the device. */
	if (gdev_time_lez(&gdev->credit_com) && gdev->com_bw_used > gdev->com_bw) {
		gdev_list_del(&gdev->list_entry_com);
		gdev_list_add_tail(&gdev->list_entry_com, &phys->sched_com_list);
	}

	gdev_list_for_each(next, &phys->sched_com_list, list_entry_com) {
	    gdev_lock_nested(&next->sched_com_lock);
		if (!gdev_list_empty(&next->sched_com_list)) {
			gdev_unlock_nested(&next->sched_com_lock);
			RESCH_G_DPRINT("Gdev#%d Selected\n", next->id);
			goto device_switched;
		}
		gdev_unlock_nested(&next->sched_com_lock);
	}
	RESCH_G_DPRINT("Nothing Selected\n");
	next = NULL;
device_switched:
	gdev_current_com_set(phys, (void*)next); /* could be null */
		
	if (next && (next != gdev) && (next->com_bw_used > next->com_bw)) {
		gdev_current_com_set(phys,NULL);
		gdev_unlock(&phys->sched_com_lock);
		__gdev_vsched_band_yield_chance();
		gdev_lock(&phys->sched_com_lock);
		if (gdev_current_com_get(phys) == NULL) {
			gdev_current_com_set(phys,(void*)next);
			chances--;
			if (chances) {
				gdev_unlock(&phys->sched_com_lock);
				RESCH_G_DPRINT("Try again\n");
				goto retry;
			}
			else
				gdev_unlock(&phys->sched_com_lock);
		}
		else
			gdev_unlock(&phys->sched_com_lock);
	}
	else
		gdev_unlock(&phys->sched_com_lock);

	return next;
}

static void gdev_vsched_band_replenish_compute(struct gdev_device *gdev)
{
	struct gdev_time credit, threshold;

	gdev_time_us(&credit, gdev->period * gdev->com_bw / 100);
	gdev_time_add(&gdev->credit_com, &gdev->credit_com, &credit);
	/* when the credit exceeds the threshold, all credits taken away. */
	gdev_time_us(&threshold, GDEV_CREDIT_INACTIVE_THRESHOLD);
	if (gdev_time_gt(&gdev->credit_com, &threshold))
		gdev_time_us(&gdev->credit_com, 0);
	/* when the credit exceeds the threshold in negative, even it. */
	threshold.neg = 1;
	if (gdev_time_lt(&gdev->credit_com, &threshold))
		gdev_time_us(&gdev->credit_com, 0);
}

static void gdev_vsched_band_schedule_memory(struct gdev_sched_entity *se)
{
	struct gdev_device *gdev = se->gdev;
	struct gdev_device *phys = gdev_phys_get(gdev);

	if (!phys)
		return;

resched:
	if (gdev_time_lez(&gdev->credit_mem) && gdev->mem_bw_used > gdev->mem_bw
		&& !__gdev_is_alone(gdev)) {
		gdev_lock(&phys->sched_mem_lock);
		if (phys->current_mem == gdev) {
			phys->current_mem = NULL;
			gdev_unlock(&phys->sched_mem_lock);

			__gdev_vsched_band_yield_chance();

			gdev_lock(&phys->sched_mem_lock);
			if (phys->current_mem == NULL)
				phys->current_mem = gdev;
			gdev_unlock(&phys->sched_mem_lock);
		}
		else
			gdev_unlock(&phys->sched_mem_lock);
	}

	gdev_lock(&phys->sched_mem_lock);
	if (phys->current_mem && phys->current_mem != gdev) {
		/* insert the scheduling entity to its local priority-ordered list. */
		gdev_lock_nested(&gdev->sched_mem_lock);
		__gdev_enqueue_memory(gdev, se);
		gdev_unlock_nested(&gdev->sched_mem_lock);
		gdev_unlock(&phys->sched_mem_lock);

		/* now the corresponding task will be suspended until some other tasks
		   will awaken it upon completions of their memory transfers. */
		gdev_sched_sleep(se);

		goto resched;
	}
	else {
		phys->current_mem = (void *)gdev;
		gdev_unlock(&phys->sched_mem_lock);
	}
}

static struct gdev_device *gdev_vsched_band_select_next_memory(struct gdev_device *gdev)
{
	struct gdev_device *phys = gdev_phys_get(gdev);
	struct gdev_device *next;
	int chances = GDEV_VSCHED_BAND_SELECT_CHANCES;

	if (!phys)
		return gdev;

retry:
	gdev_lock(&phys->sched_mem_lock);

	/* if the credit is exhausted, reinsert the device. */
	if (gdev_time_lez(&gdev->credit_mem) && gdev->mem_bw_used > gdev->mem_bw) {
		gdev_list_del(&gdev->list_entry_mem);
		gdev_list_add_tail(&gdev->list_entry_mem, &phys->sched_mem_list);
	}

	gdev_list_for_each(next, &phys->sched_mem_list, list_entry_mem) {
		gdev_lock_nested(&next->sched_mem_lock);
		if (!gdev_list_empty(&next->sched_mem_list)) {
			gdev_unlock_nested(&next->sched_mem_lock);
			goto device_switched;
		}
		gdev_unlock_nested(&next->sched_mem_lock);
	}
	next = NULL;
device_switched:
	phys->current_mem = (void*)next; /* could be null */

	if (next && next != gdev && next->mem_bw_used > next->mem_bw) {
		phys->current_mem = NULL;
		gdev_unlock(&phys->sched_mem_lock);
		__gdev_vsched_band_yield_chance();
		gdev_lock(&phys->sched_mem_lock);
		if (phys->current_mem == NULL) {
			phys->current_mem = (void*)next;
			chances--;
			if (chances) {
				gdev_unlock(&phys->sched_mem_lock);
				goto retry;
			}
			else
				gdev_unlock(&phys->sched_mem_lock);
		}
		else
			gdev_unlock(&phys->sched_mem_lock);
	}
	else
		gdev_unlock(&phys->sched_mem_lock);

	return next;
}

static void gdev_vsched_band_replenish_memory(struct gdev_device *gdev)
{
	struct gdev_time credit, threshold;

	gdev_time_us(&credit, gdev->period * gdev->mem_bw / 100);
	gdev_time_add(&gdev->credit_mem, &gdev->credit_mem, &credit);
	/* when the credit exceeds the threshold, all credits taken away. */
	gdev_time_us(&threshold, GDEV_CREDIT_INACTIVE_THRESHOLD);
	if (gdev_time_gt(&gdev->credit_mem, &threshold))
		gdev_time_us(&gdev->credit_mem, 0);
	/* when the credit exceeds the threshold in negative, even it. */
	threshold.neg = 1;
	if (gdev_time_lt(&gdev->credit_mem, &threshold))
		gdev_time_us(&gdev->credit_mem, 0);
}

/**
 * Bandwidth-aware non-preemptive device (Band) scheduler implementation
 */
struct gdev_vsched_policy gdev_vsched_band = {
	.schedule_compute = gdev_vsched_band_schedule_compute,
	.select_next_compute = gdev_vsched_band_select_next_compute,
	.replenish_compute = gdev_vsched_band_replenish_compute,
	.schedule_memory = gdev_vsched_band_schedule_memory,
	.select_next_memory = gdev_vsched_band_select_next_memory,
	.replenish_memory = gdev_vsched_band_replenish_memory,
};
