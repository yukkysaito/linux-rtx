 Linux-rtx : Linux and Real-Time eXtension

--
INSTALL:
 1. Build and install the RESCH core and library:
	./configure --task=(the maximum nubmer of real-time tasks)
	            --cpus=(the number of used CPUs)
	make
	sudo make install
    By default, --task=64 and --cpus=NR_ONLINE_CPUS are set.
    You can also use:
        (1) --disable-load-balance option, if you want to disable
            the load balance function supported by the Linux scheduler
            so that the system becomes more predictable.
        (2) and --use-hrtimer option, if you want to use high-resolution
            timers to release or wake up tasks.
    By default, these options are unset.

 2. Test the scheduling overhead:
	./test/test_overhead
    Note: this procedure may take a couple of minutes.
	
 3. Install plugins if you like. E.g, you can install the state-of-the-art
    hybrid-partitioned-global EDF scheduler as follows:
	cd plugin/hybrid/edf-wm/
	make
	make install 
    You can also install other plugins in the same way. 
    Note: some plugins must be exclusive to each other.

 4. You can run a sample program that executes a real-time task with 
    priority 50 and period 3000ms:
	cd sample
	make
	./rt_task 50 3000
    The range of priority available for user applications is [4, 96].
    Please see /usr/include/resch/api.h for definition.

 5. Several benchmarking programs are available to assess performance.
	cd bench/
	./configure
	make
    For instance, if you want to assess schedulability, run SchedBench.
	cd schedbench
	./schedbench --help
	./schedbench (OPTIONS)

 6. Have fun with RESCH! You can develop your own plugins.
    It would be nice if you notify me of your new plugins!

--
 Thanks,
 Shinpei Kato <shinpei@il.is.s.u-tokyo.ac.jp>
              <shinpei@ece.cmu.edu>
	 
