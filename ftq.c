/**
 * ftq.c : Fixed Time Quantum microbenchmark
 *
 * Written by Matthew Sottile (matt@cs.uoregon.edu)
 *
 * This is a complete rewrite of the original tscbase code by
 * Ron and Matt, in order to use a better set of portable timers,
 * and more flexible argument handling and parameterization.
 *
 * 4/15/2014 : Major rewrite; remove pthreads ifdef. Separate out OS bits. 
 *             use --include to pull in whatever OS defines you are using.
 *             New file format with time (in ns) and count in one place;
 *             comments at front are self describing, including sample
 *             frequency. Spits out octave commands to run as well.
 *             Puts cpu info in there as well as uname info. 
 *             This makes the files more useful as time goes by.
 * 12/30/2007 : Updated to add pthreads support for FTQ execution on
 *              shared memory multiprocessors (multicore and SMPs).
 *              Also, removed unused TAU support.
 *
 * Licensed under the terms of the GNU Public Licence.  See LICENCE_GPL
 * for details.
 */
#include "ftq.h"
#include <sys/mman.h>

int ignore_wire_failures = 0;
int set_realtime = 0;

void usage(char *av0)
{
	fprintf(stderr,
			"usage: %s [-t threads] [-n samples] [-f frequency] [-h] [-o outname] [-s] [-r] [-a test-argument]"
			"[-T ticks-per-ns-float] "
			"[-w (ignore wire failures -- only do this if there is no option"
			"\n",
			av0);
	exit(EXIT_FAILURE);
}

void header(FILE * f, float nspercycle, int core)
{
	fprintf(f, "# Frequency %f\n", 1e9 / interval);
	fprintf(f, "# Ticks per ns: %g\n", ticksperns);
	fprintf(f, "# octave: pkg load signal\n");
	fprintf(f, "# x = load(<file name>)\n");
	fprintf(f, "# pwelch(x(:,2),[],[],[],%f)\n", 1e9 / interval);
	fprintf(f, "# core %d\n", core);
	if (ignore_wire_failures)
		fprintf(f, "# Warning: not wired to this core; results may be flaky\n");
	osinfo(f, core);
}

int main(int argc, char **argv)
{
	/* local variables */
	static char fname[8192], outname[255];
	int i, j;
	int numthreads = 1, use_threads = 0;
	FILE *fp;
	int use_stdout = 0;
	int rc;
	pthread_t *threads;
	ticks start, end, ns;
	ticks cyclestart, cycleend, cycles, base;
	float nspercycle;
	size_t samples_size;

	/* default output name prefix */
	sprintf(outname, "ftq");

	/*
	 * getopt_long to parse command line options.
	 * basically the code from the getopt man page.
	 */
	while (1) {
		int c;
		int option_index = 0;
		static struct option long_options[] = {
			{"help", 0, 0, 'h'},
			{"numsamples", 0, 0, 'n'},
			{"frequency", 0, 0, 'f'},
			{"outname", 0, 0, 'o'},
			{"stdout", 0, 0, 's'},
			{"threads", 0, 0, 't'},
			{"ticksperns", 0, 0, 'T'},
			{"ignore_wire_failures", 0, 0, 'w'},
			{"realtime", 0, 0, 'r'},
			{"argument", 0, 0, 'a'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "n:hsf:o:t:T:wra:", long_options,
						&option_index);
		if (c == -1)
			break;

		switch (c) {
			case 't':
				numthreads = atoi(optarg);
				use_threads = 1;
				break;
			case 's':
				use_stdout = 1;
				break;
			case 'o':
				sprintf(outname, "%s", optarg);
				break;
			case 'f':
				/* the interval units are ns. */
				interval = (unsigned long long)
					(1e9 / atoi(optarg));
				break;
			case 'n':
				numsamples = atoi(optarg);
				break;
			case 'w':
				ignore_wire_failures++;
				break;
			case 'T':
				sscanf(optarg, "%lg", &ticksperns);
				break;
			case 'r':
				set_realtime = 1;
				break;
			case 'a':
				test_argument = optarg;
				break;
			case 'h':
			default:
				usage(argv[0]);
				break;
		}
	}

	/* sanity check */
	if (numsamples > MAX_SAMPLES) {
		fprintf(stderr, "WARNING: sample count exceeds maximum.\n");
		fprintf(stderr, "         setting count to maximum.\n");
		numsamples = MAX_SAMPLES;
	}
	/* allocate sample storage */
	samples_size = sizeof(unsigned long long) * numsamples * 2 * numthreads;
	samples = mmap(0, samples_size, PROT_READ | PROT_WRITE,
	               MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE | MAP_LOCKED,
	               -1, 0);
	if (samples == MAP_FAILED) {
		perror("Failed to mmap, will just malloc");
		samples = malloc(samples_size);
		assert(samples);
	}
	/* in case mmap failed or MAP_POPULATE didn't populate */
	memset(samples, 0, samples_size);

	if (use_stdout == 1 && numthreads > 1) {
		fprintf(stderr,
				"ERROR: cannot output to stdout for more than one thread.\n");
		exit(EXIT_FAILURE);
	}

	if (ticksperns == 0.0)
		ticksperns = compute_ticksperns();

	/*
	 * set up sampling.  first, take a few bogus samples to warm up the
	 * cache and pipeline
	 */
	if (use_threads == 1) {
		if (threadinit(numthreads) < 0) {
			fprintf(stderr, "threadinit failed\n");
			assert(0);
		}
		threads = malloc(sizeof(pthread_t) * numthreads);
		/* fault in the array, o/w we'd take the faults after 'start' */
		memset(threads, 0, sizeof(pthread_t) * numthreads);
		assert(threads != NULL);
		start = nsec();
		cyclestart = getticks();
		/* TODO: abstract this nonsense into a call in linux.c/akaros.c/etc */
		for (i = 0; i < numthreads; i++) {
			rc = pthread_create(&threads[i], NULL, ftq_core,
								(void *)(intptr_t) i);
			if (rc) {
				fprintf(stderr, "ERROR: pthread_create() failed.\n");
				exit(EXIT_FAILURE);
			}
		}

		hounds = 1;
		/* TODO: abstract this nonsense into a call in linux.c/akaros.c/etc */
		for (i = 0; i < numthreads; i++) {
			rc = pthread_join(threads[i], NULL);
			if (rc) {
				fprintf(stderr, "ERROR: pthread_join() failed.\n");
				exit(EXIT_FAILURE);
			}
		}
		cycleend = getticks();
		end = nsec();
	} else {
		hounds = 1;
		start = nsec();
		cyclestart = getticks();
		ftq_core(0);
		cycleend = getticks();
		end = nsec();
	}

	/* We now have the total ns used, and the total ticks used. */
	ns = end - start;
	cycles = cycleend - cyclestart;
	/* TODO: move IO to linux.c, assuming we ever need Plan 9 again.
	 * but IFDEF is NOT ALLOWED
	 */
	fprintf(stderr, "Start %lld end %lld elapsed %lld\n", start, end, ns);
	fprintf(stderr, "cyclestart %lld cycleend %lld elapsed %lld\n",
			cyclestart, cycleend, cycles);
	nspercycle = (1.0 * ns) / cycles;
	fprintf(stderr, "Avg Cycles(ticks) per ns. is %f; nspercycle is %f\n",
			(1.0 * cycles) / ns, nspercycle);
	fprintf(stderr, "Pre-computed ticks per ns: %f\n", ticksperns);
	fprintf(stderr, "Sample frequency is %f\n", 1e9 / interval);
	if (use_stdout == 1) {
		header(stdout, nspercycle, 0);
		for (i = 0, base = samples[0]; i < numsamples; i++) {
			fprintf(stdout, "%lld %lld\n",
					(ticks) (nspercycle * (samples[i * 2] - base)),
					samples[i * 2 + 1]);
		}
	} else {

		for (j = 0; j < numthreads; j++) {
			sprintf(fname, "%s_%d.dat", outname, j);
			fp = fopen(fname, "w");
			if (!fp) {
				perror("can not create file");
				exit(EXIT_FAILURE);
			}
			header(fp, nspercycle, j);
			for (i = 0, base = samples[numsamples * j * 2]; i < numsamples; i++) {
				fprintf(fp, "%lld %lld\n",
						(ticks) (nspercycle *
								 (samples[j * numsamples * 2 + i * 2] - base)),
						samples[j * numsamples * 2 + i * 2 + 1]);
			}
			fclose(fp);
		}
	}

	if (use_threads)
		pthread_exit(NULL);

	exit(EXIT_SUCCESS);
}
