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
#include <time.h>
#include <pthread.h>

#define gettime(fmt) clock_gettime(CLOCK_PROCESS_CPUTIME_ID, fmt)

#define LOOP_COUNT 20
//#define USE_SCHED_CPU


static struct timespec ns_to_timespec(unsigned long ns)
{
	struct timespec ts;
	ts.tv_sec = ns / 1000000000;
	ts.tv_nsec = (ns - ts.tv_sec * 1000000000LL)  ;
	return ts;
}

static unsigned long timespec_to_ns_sub(struct timespec *ts1, struct timespec *ts2)
{
	unsigned long ret, ret2;
	ret = ts1->tv_sec * 10^9;
	ret2 = ts2->tv_sec * 10^9;
	ret += ts1->tv_nsec;
	ret2 += ts2->tv_nsec;

	ret = ret2 - ret;
	return ret;
}


static struct timespec ms_to_timespec(unsigned long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms - ts.tv_sec * 1000) * 1000000LL;
	return ts;
}

int cuda_test_madd(unsigned int n, char *path);

struct timespec period, runtime, timeout, deadline;

	unsigned long prio;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
void* thread(void *args)
{
    struct timespec ts1,ts2,ts3;
    int i;  


    gettime(&ts1);

#ifdef USE_SCHED_CPU
    rt_init(); 
    rt_set_period(period);
    rt_set_deadline(period);// if period = deadline, don't call "rt_set_deadline"
    rt_set_runtime(runtime);

    rt_set_scheduler(SCHED_FP); /* you can also set SCHED_EDF. */
    rt_set_priority(prio);
    rt_run(timeout);
#endif

    for (i = 0; i < LOOP_COUNT; i++) {
	cuda_test_madd(1000, ".");
    }
    gettime(&ts2);

    pthread_mutex_lock(&mutex);
    printf("%ld,",timespec_to_ns_sub(&ts1,&ts2));
    pthread_mutex_unlock(&mutex);
    return NULL;
}


int main(int argc, char* argv[])
{
	int i,j;
	struct timeval tv1, tv2, tv3;
	int dmiss_count = 0;
	pthread_t th[4];

	if (argc != 4) {
		printf("Error: invalid option\n");
		printf("usage: rt_task period runtime deadline\n");
		exit(EXIT_FAILURE);
	}
	
#ifdef USE_SCHED_CPU
	prio = 0;					/* priority. */
	//printf("---- period  :%d ms ----\n", atoi(argv[1]));
	period = ms_to_timespec(atoi(argv[1]));	/* period. */
	//printf("---- runtime :%d ms ----\n", atoi(argv[2]));
	runtime = ms_to_timespec(atoi(argv[2]));			/* execution time. */
	//printf("---- deadline:%d ms ----\n", atoi(argv[3]));
	deadline = ms_to_timespec( atoi(argv[3]) );			/* execution time. */
	//printf("---- timeout:%d ms ----\n", 30000);
	timeout = ms_to_timespec(3000);			/* timeout. */
#endif

	for(j=0; j<4; j++)
	    pthread_create(&th[j], NULL, thread, (void *)NULL);
	for(j=0; j<4; j++)
	    pthread_join(th[j],NULL);

	printf("\n");

#ifdef USE_SCHED_CPU
	rt_exit();
#endif
	

	return 0;
}
