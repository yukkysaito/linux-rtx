/*
 * rt_task.c
 *
 * Sample program that executes one periodic real-time task.
 * Each job consumes 1 second, if it is not preempted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <resch/api.h>
#include "tvops.h"

#define LOOP_COUNT 20

static struct timespec ns_to_timespec(unsigned long ns)
{
	struct timespec ts;
	ts.tv_sec = ns / 1000000000;
	ts.tv_nsec = (ns - ts.tv_sec * 1000000000LL)  ;
	return ts;
}


static struct timespec ms_to_timespec(unsigned long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms - ts.tv_sec * 1000) * 1000000LL;
	return ts;
}

int cuda_test_madd(unsigned int n, char *path);



int main(int argc, char* argv[])
{
	int i;
	unsigned long prio;
	struct timespec period, runtime, timeout, deadline;
	struct timeval tv1, tv2, tv3;
	int dmiss_count = 0;

	if (argc != 4) {
		printf("Error: invalid option\n");
		printf("usage: rt_task period runtime deadline\n");
		exit(EXIT_FAILURE);
	}
	
	prio = 0;					/* priority. */
	printf("---- period  :%d ms ----\n", atoi(argv[1]));
	period = ms_to_timespec(atoi(argv[1]));	/* period. */
	printf("---- runtime :%d ms ----\n", atoi(argv[2]));
	runtime = ms_to_timespec(atoi(argv[2]));			/* execution time. */
	printf("---- deadline:%d ms ----\n", atoi(argv[3]));
	deadline = ms_to_timespec( atoi(argv[3]) );			/* execution time. */
	printf("---- timeout:%d ms ----\n", 30000);
	timeout = ms_to_timespec(3000);			/* timeout. */

	/* bannar. */
	printf("sample program\n");

	rt_init(); 
	rt_set_period(period);
	rt_set_deadline(period);// if period = deadline, don't call "rt_set_deadline"
	rt_set_runtime(runtime);

	rt_set_scheduler(SCHED_EDF); /* you can also set SCHED_EDF. */
	rt_set_priority(prio);
	rt_run(timeout);

	for (i = 0; i < LOOP_COUNT; i++) {
		gettimeofday(&tv1, NULL);
		printf("start : %ld:%06ld\n", (long)tv1.tv_sec, (long)tv1.tv_usec);
//		fflush(stdout);

		cuda_test_madd(1000, ".");
		gettimeofday(&tv2, NULL);
		tvsub(&tv2, &tv1, &tv3);

		printf("finish: %ld:%06ld\n", (long)tv2.tv_sec, (long)tv2.tv_usec);
//		fflush(stdout);
		if (!rt_wait_period()) {
			printf("deadline is missed!\n");
			dmiss_count++;
		}
	}

	printf("Finished Tasks. %d/%d(dmiss/loopcount)\n", dmiss_count, LOOP_COUNT);
	fflush(stdout);

	rt_exit();
	
	return 0;
}
