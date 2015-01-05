/*
 * Copyright (C) 2014-2015 Canonical
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Author Colin Ian King,  colin.king@canonical.com
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>

#define KB			(1024ULL)
#define MB			(KB * KB)
#define GB			(KB * KB * KB)

#define UNDERRUN_MAX		(100)
#define UNDERRUN_ADJUST_MAX	(2)
#define OVERRUN_ADJUST_MAX	(2)

#define DELAY_SHIFT_MIN		(0)
#define DELAY_SHIFT_MAX		(16)

#define IO_SIZE_MAX		(MB * 64)
#define IO_SIZE_MIN		(1)

#define DELAY_MIN		(0.01)
#define DELAY_MAX		(10.00)

#define DEFAULT_FREQ		(0.250)

#define DEBUG_RATE		(0)

#define OPT_VERBOSE		(0x00000001)
#define OPT_GOT_RATE		(0x00000002)
#define OPT_GOT_IOSIZE		(0x00000004)
#define OPT_GOT_CONST_DELAY	(0x00000008)
#define OPT_WARNING		(0x00000010)
#define OPT_UNDERRUN		(0x00000020)
#define OPT_DISCARD_STDOUT	(0x00000040)
#define OPT_OVERRUN		(0x00000080)
#define OPT_ZERO		(0x00000100)
#define OPT_URANDOM		(0x00000200)
#define OPT_APPEND		(0x00000400)
#define OPT_STATS		(0x00000800)
#define OPT_NO_RATE_CONTROL	(0x00001000)
#define OPT_TIMED_RUN		(0x00002000)
#define OPT_INPUT_FILE		(0x00004000)
#define OPT_VERSION		(0x00008000)
#define OPT_PROGRESS		(0x00010000)
#define OPT_MAX_TRANS_SIZE	(0x00020000)
#define OPT_SKIP_READ_ERRORS	(0x00040000)

#define DRIFT_MAX		(7)

static unsigned int opt_flags;
static const char *app_name = "sluice";
static const char *dev_urandom = "/dev/urandom";
static volatile bool sluice_finish = false;

/* scaling factor */
typedef struct {
	const char ch;			/* Scaling suffix */
	const uint64_t scale;		/* Amount to scale by */
} scale_t;

/* various statistics */
typedef struct {
	uint64_t	reads;		/* Total read calls */
	uint64_t	writes;		/* Total write calls */
	uint64_t	total_bytes;	/* Total bytes copied */
	uint64_t	underruns;	/* Count of underruns */
	uint64_t	overruns;	/* Count of overruns */
	uint64_t	perfect;	/* Count of no under/overruns */
	uint64_t	drift[DRIFT_MAX];/* Drift from desired rate */
	uint64_t	drift_total;	/* Number of drift samples */
	double		time_begin;	/* Time began */
	double		time_end;	/* Time ended */
	double		target_rate;	/* Target transfer rate */
	double		buf_size_total;	/* For average buffer size */
	double		rate_min;	/* Minimum rate */
	double		rate_max;	/* Maximum rate */
	bool		rate_set;	/* Min/max set or not? */
} stats_t;

/*
 *  count_bits()
 *      count bits set, from C Programming Language 2nd Ed
 */
static unsigned int count_bits(const unsigned int val)
{
	register unsigned int c, n = val;

	for (c = 0; n; c++)
		n &= n - 1;
	return c;
}

/*
 *  handle_sigint()
 *	catch SIGINT, jump to tidy termination
 */
static void handle_sigint(int dummy)
{
	(void)dummy;

	sluice_finish = true;
}

/*
 *  handle_siginfo()
 *	catch SIGINFO/SIGUSR1, toggle verbose mode
 */
static void handle_siginfo(int dummy)
{
	(void)dummy;

	opt_flags ^= OPT_VERBOSE;
}

/*
 *  handle_sigusr2()
 *	catch SIGUSR2, toggle underflow/overflow
 */
static void handle_sigusr2(int dummy)
{
	(void)dummy;

	opt_flags ^= (OPT_OVERRUN | OPT_UNDERRUN);
}

/*
 *  stats_init()
 *	Initialize statistics
 */
static inline void stats_init(stats_t *const stats)
{
	stats->reads = 0;
	stats->writes = 0;
	stats->total_bytes = 0;
	stats->underruns = 0;
	stats->overruns = 0;
	stats->perfect = 0;
	memset(&stats->drift, 0, sizeof(stats->drift));
	stats->drift_total = 0;
	stats->time_begin = 0.0;
	stats->time_end = 0.0;
	stats->target_rate = 0.0;
	stats->buf_size_total = 0.0;
	stats->rate_min = 0.0;
	stats->rate_max = 0.0;
	stats->rate_set = false;
}

/*
 *  size_to_str()
 *	report size in different units
 */
static void size_to_str(
	const double val,
	const char *const fmt,
	char *const buf,
	const size_t buflen)
{
	double v = val;
	int i;

	static char *sizes[] = {
		"B ",	/* Bytes */
		"KB",	/* Kilobytes */
		"MB",	/* Megabytes */
		"GB",	/* Gigabytes */
		"TB",	/* Terabytes */
		"PB",	/* Petabytes */
		"EB",	/* Exabytes */
		"ZB",	/* Zettabytes */
		"YB",	/* Yottabytes */
	};

	for (i = 0; i < 8; i++, v /= 1024.0) {
		if (v <= 512.0)
			break;
	}

	snprintf(buf, buflen, fmt, v, sizes[i]);
}

/*
 *  double_to_str()
 *	convert double size in bytes to string
 */
static char *double_to_str(const double val)
{
	static char buf[64];

	size_to_str(val, "%.2f %s", buf, sizeof(buf));
	return buf;
}


/*
 *  stats_info()
 *	display run time statistics
 */
static void stats_info(const stats_t *stats)
{
	double total = stats->underruns + stats->overruns + stats->perfect;
	double secs = stats->time_end - stats->time_begin;
	double avg_wr_sz;
	struct tms t;

	if (secs <= 0.0)  {
		fprintf(stderr, "Cannot compute statistics\n");
		return;
	}
	avg_wr_sz = stats->writes ?
		stats->buf_size_total / stats->writes : 0.0;
	fprintf(stderr, "Data:            %s\n",
		double_to_str((double)stats->total_bytes));
	fprintf(stderr, "Reads:           %" PRIu64 "\n",
		stats->reads);
	fprintf(stderr, "Writes:          %" PRIu64 "\n",
		stats->writes);
	fprintf(stderr, "Avg. Write Size: %s\n",
		double_to_str(avg_wr_sz));
	fprintf(stderr, "Duration:        %.3f secs\n",
		secs);
	if (!(opt_flags & OPT_NO_RATE_CONTROL)) {
		fprintf(stderr, "Target rate:     %s/sec\n",
			double_to_str(stats->target_rate));
	}
	fprintf(stderr, "Average rate:    %s/sec\n",
		double_to_str((double)stats->total_bytes / secs));
	fprintf(stderr, "Minimum rate:    %s/sec\n",
		double_to_str(stats->rate_min));
	fprintf(stderr, "Maximum rate:    %s/sec\n",
		double_to_str(stats->rate_max));
	if (!(opt_flags & OPT_NO_RATE_CONTROL)) {
		int i, last_percent = 0;
		uint64_t drift_sum = 0;

		fprintf(stderr, "Drift from target rate: (%%)\n");
		for (i = 0; i < DRIFT_MAX; i++) {
			int percent = 1 << i;
			fprintf(stderr, "  %5.2f%% - %5.2f%%: %6.2f%%\n",
				(double)last_percent, (double)percent - 0.01,
				stats->drift_total ?
					100.0 * (double)stats->drift[i] / (double)stats->drift_total : 0.0);
			last_percent = percent;
			drift_sum += stats->drift[i];
		}
		fprintf(stderr, " >%5.2f%%         : %6.2f%%\n",
			(double)last_percent,
			stats->drift_total ?
				100.0 - ((100.0 * (double)drift_sum) / (double)stats->drift_total) : 0.0);
		fprintf(stderr, "Overruns:        %5.2f%%\n", total ?
			100.0 * (double)stats->underruns / total : 0.0);
		fprintf(stderr, "Underruns:       %5.2f%%\n", total ?
			100.0 * (double)stats->overruns / total : 0.0);
	}

	if (times(&t) != (clock_t)-1) {
		long int ticks_per_sec;

		if ((ticks_per_sec = sysconf(_SC_CLK_TCK)) > 0) {
			fprintf(stderr, "User time:       %.3f secs\n",
				(double)t.tms_utime / (double)ticks_per_sec);
			fprintf(stderr, "System time:     %.3f secs\n",
				(double)t.tms_stime / (double)ticks_per_sec);
		}
	}
}

/*
 *  timeval_to_double()
 *	convert timeval to seconds as a double
 */
static double timeval_to_double(void)
{
	struct timeval tv;

redo:
	errno = 0;
	if (gettimeofday(&tv, NULL) < 0) {
		if (errno == EINTR)	/* Should not occur */
			goto redo;

		fprintf(stderr, "gettimeofday error: errno=%d (%s).\n",
			errno, strerror(errno));
		return -1.0;
	}
	return (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
}

/*
 *  get_uint64()
 *	get a uint64 value
 */
static uint64_t get_uint64(const char *const str, size_t *const len)
{
	uint64_t val;
	*len = strlen(str);

	errno = 0;
	val = (uint64_t)strtoull(str, NULL, 10);
	if (errno) {
		fprintf(stderr, "Invalid value %s.\n", str);
		exit(EXIT_FAILURE);
	}
	if (*len == 0) {
		fprintf(stderr, "Value %s is an invalid size.\n", str);
		exit(EXIT_FAILURE);
	}
	return val;
}

/*
 *  get_double()
 *	get a double value
 */
static double get_double(const char *const str, size_t *const len)
{
	double val;
	*len = strlen(str);

	errno = 0;
	val = strtod(str, NULL);
	if (errno) {
		fprintf(stderr, "Invalid value %s.\n", str);
		exit(EXIT_FAILURE);
	}
	if (*len == 0) {
		fprintf(stderr, "Value %s is an invalid size.\n", str);
		exit(EXIT_FAILURE);
	}
	return val;
}


/*
 *  get_uint64_scale()
 *	get a value and scale it by the given scale factor
 */
static uint64_t get_uint64_scale(
	const char *const str,
	const scale_t scales[],
	const char *const msg)
{
	double val;
	size_t len = strlen(str);
	int i;
	char ch;

	val = get_double(str, &len);
	len--;
	ch = str[len];

	if (val < 0.0) {
		printf("Value %s cannot be negative\n", str);
		exit(EXIT_FAILURE);
	}

	if (isdigit(ch) || ch == '.')
		return (uint64_t)val;

	ch = tolower(ch);
	for (i = 0; scales[i].ch; i++) {
		if (ch == scales[i].ch)
			return (uint64_t)(val * scales[i].scale);
	}

	printf("Illegal %s specifier %c\n", msg, str[len]);
	exit(EXIT_FAILURE);
}

/*
 *  get_uint64_byte()
 *	size in bytes, K bytes, M bytes, G bytes, T bytes or P bytes
 */
static uint64_t get_uint64_byte(const char *const str)
{
	static const scale_t scales[] = {
		{ 'b', 	1ULL },
		{ 'k',  1ULL << 10 },	/* Kilobytes */
		{ 'm',  1ULL << 20 },	/* Megabytes */
		{ 'g',  1ULL << 30 },	/* Gigabytes */
		{ 't',  1ULL << 40 },	/* Terabytes */
		{ 'p',	1ULL << 60 },	/* Petabytes */
		{ 0,    0 },
	};

	return get_uint64_scale(str, scales, "length");
}

/*
 *  get_uint64_time()
 *	time in seconds, minutes, hours, days or years
 */
uint64_t get_uint64_time(const char *const str)
{
	static const scale_t scales[] = {
		{ 's',  1 },
		{ 'm',  60 },
		{ 'h',  3600 },
		{ 'd',  24 * 3600 },
		{ 'y',  365 * 24 * 3600 },
	};

	return get_uint64_scale(str, scales, "time");
}

/*
 *  show_usage()
 *	show options
 */
static void show_usage(void)
{
	printf("%s, version %s\n\n", app_name, VERSION);
	printf("Usage: %s [options]\n", app_name);
	printf("  -a        append to file (-t, -O options only).\n");
	printf("  -c delay  specify constant delay time (seconds).\n");
	printf("  -d        discard input (no output).\n");
	printf("  -e        skip read errors.\n");
	printf("  -f freq   frequency of -v statistics.\n");
	printf("  -h        print this help.\n");
	printf("  -i size   set io read/write size in bytes.\n");
	printf("  -m size   set maximum amount to process.\n");
	printf("  -n        no rate controls, just copy data untouched.\n");
	printf("  -o        shrink read/write buffer to avoid overrun.\n");
	printf("  -O file   short cut for -dt file; output to a file.\n");
	printf("  -p        enable verbose mode with progress stats.\n");
	printf("  -r rate   set rate (in bytes per second).\n");
	printf("  -R	    ignore stdin, read from %s.\n", dev_urandom);
	printf("  -s shift  controls delay or buffer size adjustment.\n");
	printf("  -S        display statistics at end of stream to stderr.\n");
	printf("  -t file   tee output to file.\n");
	printf("  -T time   stop after a specified amount of time.\n");
	printf("  -u        expand read/write buffer to avoid underrun.\n");
	printf("  -v        set verbose mode (to stderr).\n");
	printf("  -V        print version information.\n");
	printf("  -w        warn on data rate underrun.\n");
	printf("  -z        ignore stdin, generate zeros.\n");
}

int main(int argc, char **argv)
{
	char run = ' ';			/* Overrun/underrun flag */
	char *buffer = NULL;		/* Temp I/O buffer */
	char *out_filename = NULL;	/* -t or -O option filename */
	char *in_filename = NULL;	/* -I option filename */
	int64_t delay, last_delay = 0;	/* Delays in 1/1000000 of a second */
	uint64_t io_size = 0;
	uint64_t data_rate = 0;
	uint64_t total_bytes = 0;
	uint64_t max_trans = 0;
	off_t progress_size = 0;
	uint64_t adjust_shift = 3;
	uint64_t timed_run = 0;
	int underrun_adjust = UNDERRUN_ADJUST_MAX;
	int overrun_adjust = OVERRUN_ADJUST_MAX;
	int fdin = -1, fdout, fdtee = -1;
	int underruns = 0, overruns = 0, warnings = 0;
	int ret = EXIT_FAILURE;
	double secs_start, secs_last, freq = DEFAULT_FREQ;
	double const_delay = -1.0;
	bool eof = false;
	stats_t stats;
	struct sigaction new_action;

	stats_init(&stats);

	for (;;) {
		const int c = getopt(argc, argv, "ar:h?i:vm:wudot:f:zRs:c:O:SnT:I:Vpe");
		size_t len;

		if (c == -1)
			break;
		switch (c) {
		case 'a':
			opt_flags |= OPT_APPEND;
			break;
		case 'c':
			opt_flags |= (OPT_GOT_CONST_DELAY |
				      OPT_UNDERRUN |
				      OPT_OVERRUN);
			const_delay = atof(optarg);
			underrun_adjust = 1;
			overrun_adjust = 1;
			break;
		case 'd':
			opt_flags |= OPT_DISCARD_STDOUT;
			break;
		case 'e':
			opt_flags |= OPT_SKIP_READ_ERRORS;
			break;
		case 'f':
			freq = atof(optarg);
			break;
		case 'h':
			show_usage();
			exit(EXIT_SUCCESS);
		case 'i':
			opt_flags |= OPT_GOT_IOSIZE;
			io_size = get_uint64_byte(optarg);
			break;
		case 'I':
			opt_flags |= OPT_INPUT_FILE;
			in_filename = optarg;
			break;
		case 'm':
			opt_flags |= OPT_MAX_TRANS_SIZE;
			max_trans = get_uint64_byte(optarg);
			break;
		case 'n':
			opt_flags |= OPT_NO_RATE_CONTROL;
			break;
		case 'o':
			opt_flags |= OPT_OVERRUN;
			break;
		case 'O':
			opt_flags |= OPT_DISCARD_STDOUT;
			out_filename = optarg;
			break;
		case 'p':
			opt_flags |= (OPT_PROGRESS | OPT_VERBOSE);
			break;
		case 'r':
			data_rate = get_uint64_byte(optarg);
			opt_flags |= OPT_GOT_RATE;
			break;
		case 'R':
			opt_flags |= OPT_URANDOM;
			break;
		case 's':
			adjust_shift = get_uint64(optarg, &len);
			break;
		case 'S':
			opt_flags |= OPT_STATS;
			break;
		case 't':
			out_filename = optarg;
			break;
		case 'T':
			opt_flags |= OPT_TIMED_RUN;
			timed_run = get_uint64_time(optarg);
			break;
		case 'u':
			opt_flags |= OPT_UNDERRUN;
			break;
		case 'v':
			opt_flags |= OPT_VERBOSE;
			break;
		case 'V':
			opt_flags |= OPT_VERSION;
			printf("%s: %s\n", app_name, VERSION);
			exit(EXIT_SUCCESS);
			break;
		case 'w':
			opt_flags |= OPT_WARNING;
			break;
		case 'z':
			opt_flags |= OPT_ZERO;
			break;
		case '?':
			printf("Try '%s -h' for more information.\n", app_name);
			exit(EXIT_FAILURE);
		default:
			show_usage();
			exit(EXIT_FAILURE);
		}
	}

	if ((opt_flags & OPT_NO_RATE_CONTROL) &&
            (opt_flags & (OPT_GOT_CONST_DELAY | OPT_GOT_RATE | OPT_UNDERRUN | OPT_OVERRUN))) {
		fprintf(stderr, "Cannot use -n option with -c, -r, -u or -o options.\n");
		goto tidy;
	}

	if (!out_filename && (opt_flags & OPT_APPEND)) {
		fprintf(stderr, "Must use -t filename when using the -a option.\n");
		goto tidy;
	}
	if (!(opt_flags & (OPT_GOT_RATE | OPT_NO_RATE_CONTROL))) {
		fprintf(stderr, "Must specify data rate with -r option.\n");
		goto tidy;
	}
	if ((opt_flags & (OPT_GOT_IOSIZE | OPT_GOT_CONST_DELAY)) ==
	    (OPT_GOT_IOSIZE | OPT_GOT_CONST_DELAY)) {
		fprintf(stderr, "Cannot use both -i and -c options together.\n");
		goto tidy;
	}
	if ((opt_flags & OPT_GOT_RATE) && (data_rate < 1)) {
		fprintf(stderr, "Rate value %" PRIu64 " too low.\n", data_rate);
		goto tidy;
	}
	if (freq < 0.01) {
		fprintf(stderr, "Frequency too low.\n");
		goto tidy;
	}

#if DELAY_SHIFT_MIN > 0
	if (adjust_shift < DELAY_SHIFT_MIN || adjust_shift > DELAY_SHIFT_MAX) {
#else
	if (adjust_shift > DELAY_SHIFT_MAX) {
#endif
		fprintf(stderr, "Delay shift must be %d .. %d.\n",
			DELAY_SHIFT_MIN, DELAY_SHIFT_MAX);
		goto tidy;
	}
	if ((opt_flags & OPT_GOT_CONST_DELAY) &&
	    (const_delay < DELAY_MIN || const_delay > DELAY_MAX)) {
		fprintf(stderr, "Delay time must be %.2f .. %.2f seconds.\n",
			DELAY_MIN, DELAY_MAX);
		goto tidy;
	}

	/*
	 *  No size specified, then default rate
	 */
	if (!(opt_flags & OPT_GOT_IOSIZE)) {
		if (opt_flags & OPT_GOT_CONST_DELAY) {
			io_size = (KB * data_rate * const_delay) / KB;
			if (io_size < 1) {
				fprintf(stderr, "Delay too small, internal buffer too small.\n");
				goto tidy;
			}
			if (io_size > IO_SIZE_MAX) {
				fprintf(stderr, "Delay too large, internal buffer too big.\n");
				goto tidy;
			}
		}
		else {
			if (opt_flags & OPT_NO_RATE_CONTROL) {
				io_size = 4 * KB;
			} else {
				io_size = data_rate / 32;
				/* Make sure we don't have small sized I/O */
				if (io_size < KB)
					io_size = KB;
				if (io_size > IO_SIZE_MAX) {
					fprintf(stderr, "Rate too high, maximum allowed: %" PRIu64 ".\n",
						(uint64_t)IO_SIZE_MAX * 32);
					goto tidy;
				}
			}
		}
	}

	if ((io_size < 1) || (io_size > IO_SIZE_MAX)) {
		fprintf(stderr, "I/O buffer size %" PRIu64 " out of range.\n",
			io_size);
		goto tidy;
	}
	if ((buffer = malloc(io_size)) == NULL) {
		fprintf(stderr,"Cannot allocate buffer of %" PRIu64 " bytes.\n",
			io_size);
		goto tidy;
	}
	if (opt_flags & OPT_ZERO)
		memset(buffer, 0, io_size);

	if (count_bits(opt_flags & (OPT_ZERO | OPT_URANDOM | OPT_INPUT_FILE)) > 1) {
		fprintf(stderr, "Cannot use -z, -R or -I options together.\n");
		goto tidy;
	}

	if (opt_flags & OPT_INPUT_FILE) {
		struct stat buf;

		fdin = open(in_filename, O_RDONLY);
		if (fdin < 0) {
			fprintf(stderr, "open on %s failed: errno = %d (%s).\n",
				in_filename, errno, strerror(errno));
			goto tidy;
		}
		if (fstat(fdin, &buf) < 0) {
			fprintf(stderr, "fstat on file %s failed: errnp = %d (%s).\n",
				in_filename, errno, strerror(errno));
		}
		progress_size = buf.st_size;
	}
	if (opt_flags & OPT_MAX_TRANS_SIZE)
		progress_size = (off_t)max_trans;

	if (opt_flags & OPT_URANDOM) {
		fdin = open(dev_urandom, O_RDONLY);
		if (fdin < 0) {
			fprintf(stderr, "Cannot open %s: errno=%d (%s).\n",
				dev_urandom, errno, strerror(errno));
			goto tidy;
		}
	}
	if (out_filename) {
		int open_flags = (opt_flags & OPT_APPEND) ? O_APPEND : O_TRUNC;

		(void)umask(0077);
		fdtee = open(out_filename, O_CREAT | open_flags | O_WRONLY, S_IRUSR | S_IWUSR);
		if (fdtee < 0) {
			fprintf(stderr, "open on %s failed: errno = %d (%s).\n",
				out_filename, errno, strerror(errno));
			goto tidy;
		}
	}

	if (fdin == -1)
		fdin = fileno(stdin);
	fdout = fileno(stdout);

	if ((secs_start = timeval_to_double()) < 0.0)
		goto tidy;

	if (opt_flags & OPT_NO_RATE_CONTROL) {
		delay = 0;
	} else if (opt_flags & OPT_GOT_CONST_DELAY) {
		delay = 1000000 * const_delay;
	} else {
		delay = (int)(((double)io_size * 1000000) / (double)data_rate);
	}
	secs_last = secs_start;
	stats.time_begin = secs_start;
	stats.target_rate = data_rate;

	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_handler = handle_sigint;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	if (sigaction(SIGINT, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto tidy;
	}

	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_handler = handle_siginfo;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	if (sigaction(SIGUSR1, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto tidy;
	}
#ifdef SIGINFO
	if (sigaction(SIGINFO, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto tidy;
	}
#endif
	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_handler = handle_sigusr2;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	if (sigaction(SIGUSR2, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto tidy;
	}

	while (!(eof | sluice_finish)) {
		uint64_t current_rate, inbufsize = 0;
		bool complete = false;
		double secs_now;

		if (opt_flags & OPT_ZERO) {
			inbufsize = io_size;
			total_bytes += io_size;
			stats.reads++;
		} else {
			char *ptr = buffer;
			while (!complete && (inbufsize < io_size)) {
				uint64_t sz = io_size - inbufsize;
				ssize_t n;

				/* We hit the user specified max limit to transfer */
				if (max_trans && (total_bytes + sz) > max_trans) {
					sz = max_trans - total_bytes;
					complete = true;
				}

				n = read(fdin, ptr, (ssize_t)sz);
				if (n < 0) {
					if (errno == EINTR) {
						if (sluice_finish)
							goto finish;
						/* read needs re-doing */
						continue;
					}
					if (opt_flags & OPT_SKIP_READ_ERRORS) {
						memset(ptr, 0, sz);
						n = sz;
					} else {
						fprintf(stderr,"read error: errno=%d (%s).\n",
							errno, strerror(errno));
						goto tidy;
					}
				}
				if (n == 0) {
					eof = true;
					break;
				}
				inbufsize += n;
				total_bytes += n;
				ptr += n;
				stats.reads++;
			}
		}
		stats.writes++;
		stats.total_bytes += inbufsize;
		stats.buf_size_total += inbufsize;
		if (!(opt_flags & OPT_DISCARD_STDOUT)) {
			if (write(fdout, buffer, (size_t)inbufsize) < 0) {
				fprintf(stderr,"Write error: errno=%d (%s).\n",
					errno, strerror(errno));
				goto tidy;
			}
		}
		if (fdtee >= 0) {
redo_write:
			if (write(fdtee, buffer, (size_t)inbufsize) < 0) {
				if (errno == EINTR) {
					if (sluice_finish)
						goto finish;
					/* write needs re-doing */
					goto redo_write;
				} else {
					fprintf(stderr, "write error: errno=%d (%s).\n",
						errno, strerror(errno));
					goto tidy;
				}
			}
		}
		if (eof || (max_trans && total_bytes >= max_trans))
			break;

		if (delay > 0) {
			if (usleep(delay) < 0) {
				if (errno == EINTR) {
					if (sluice_finish)
						goto finish;
					/*
					 * usleep got interrupted, let
					 * subsequent I/O cater with the
					 * delay deltas rather than
					 * trying to figure out how much
					 * time was lost on early exit
					 * from usleep
					 */
				} else {
					fprintf(stderr, "usleep error: errno=%d (%s).\n",
						errno, strerror(errno));
					goto tidy;
				}
			}
		}

		if ((secs_now = timeval_to_double()) < 0.0)
			goto tidy;
		current_rate = (uint64_t)(((double)total_bytes) / (secs_now - secs_start));

		if (stats.rate_set) {
			if (current_rate > stats.rate_max)
				stats.rate_max = current_rate;
			if (current_rate < stats.rate_min)
				stats.rate_min = current_rate;
		} else {
			stats.rate_min = current_rate;
			stats.rate_max = current_rate;
			stats.rate_set = true;
		}

		if (!(opt_flags & OPT_NO_RATE_CONTROL)) {
			double drift_rate = 100.0 * fabs((double)current_rate - (double)data_rate) / data_rate;
			int i;

			stats.drift_total++;
			for (i = 0; i < DRIFT_MAX; i++) {
				int percent = 1 << i;
				if (drift_rate < (double)percent) {
					stats.drift[i]++;
					break;
				}
			}
		}
#if DEBUG_RATE
		fprintf(stderr, "%" PRIu64 "\n", current_rate);
#endif

		if (opt_flags & OPT_NO_RATE_CONTROL) {
			run = '-';
		} else {
			if (current_rate > (double)data_rate) {
				run = '+' ;
				if (!(opt_flags & OPT_GOT_CONST_DELAY)) {
					if (adjust_shift)
						delay += ((last_delay >> adjust_shift) + 100);
					else {
						double d1 = (double)(total_bytes) / (double)current_rate;
						double d2 = (double)(total_bytes + inbufsize) / (double)data_rate;

						delay += 1000000.0 * fabs(d1 - d2);
					}
				}
				warnings = 0;
				underruns = 0;
				overruns++;
				stats.overruns++;
			} else if (current_rate < (double)data_rate) {
				run = '-' ;
				if (!(opt_flags & OPT_GOT_CONST_DELAY)) {
					if (adjust_shift)
						delay -= ((last_delay >> adjust_shift) + 100);
					else {
						double d1 = (double)(total_bytes) / (double)current_rate;
						double d2 = (double)(total_bytes + inbufsize) / (double)data_rate;

						delay -= 1000000.0 * fabs(d1 - d2);
					}
				}
				warnings++;
				underruns++;
				stats.underruns++;
				overruns = 0;
			} else {
				/* Unlikely.. */
				warnings = 0;
				underruns = 0;
				overruns = 0;
				stats.perfect++;
				run = '0';
			}
			if (delay < 0)
				delay = 0;

			if ((opt_flags & OPT_UNDERRUN) &&
			    (underruns > underrun_adjust)) {
				char *tmp;
				uint64_t tmp_io_size;

				if (adjust_shift) {
					tmp_io_size = io_size + (io_size >> adjust_shift);
				} else {
					uint64_t io_size_current = (KB * current_rate * const_delay) / KB;
					uint64_t io_size_desired = (KB * data_rate * const_delay) / KB;
					int64_t delta = llabs(io_size_desired - io_size_current);

					tmp_io_size = io_size + delta;
				}

				/* If size is too small, we get stuck at 1 */
				if (tmp_io_size < 4)
					tmp_io_size = 4;

				if (tmp_io_size < IO_SIZE_MAX) {
					tmp = realloc(buffer, tmp_io_size);
					if (tmp) {
						if (opt_flags & OPT_ZERO)
							memset(tmp, 0, tmp_io_size);
						buffer = tmp;
						io_size = tmp_io_size;
					}
				}
				underruns = 0;
			}

			if ((opt_flags & OPT_OVERRUN) &&
			    (overruns > overrun_adjust)) {
				char *tmp;
				uint64_t tmp_io_size;

				if (adjust_shift) {
					tmp_io_size = io_size - (io_size >> adjust_shift);
				} else {
					uint64_t io_size_current = (KB * current_rate * const_delay) / KB;
					uint64_t io_size_desired = (KB * data_rate * const_delay) / KB;
					int64_t delta = llabs(io_size_desired - io_size_current);

					tmp_io_size = io_size - delta;
				}

				if (tmp_io_size > IO_SIZE_MIN) {
					tmp = realloc(buffer, tmp_io_size);
					if (tmp) {
						if (opt_flags & OPT_ZERO)
							memset(tmp, 0, tmp_io_size);
						buffer = tmp;
						io_size = tmp_io_size;
					}
				}
				overruns = 0;
			}

			/* Too many continuous underruns? */
			if ((opt_flags & OPT_WARNING) &&
			    (warnings > UNDERRUN_MAX)) {
				fprintf(stderr, "Warning: data underrun, "
					"use larger I/O size (-i option)\n");
				opt_flags &= ~OPT_WARNING;
			}
		}
		last_delay = delay;

		/* Output feedback in verbose mode */
		if ((opt_flags & OPT_VERBOSE) &&
		    (secs_now > secs_last + freq)) {
			char current_rate_str[32];
			char total_bytes_str[32];

			size_to_str(current_rate, "%7.1f %s",
				current_rate_str,
				sizeof(current_rate_str));
			size_to_str(total_bytes, "%7.1f %s",
				total_bytes_str,
				sizeof(total_bytes_str));

			if (opt_flags & OPT_PROGRESS) {
				/* Progress % and ETA estimates */
				double secs = secs_now - secs_start;
				if (progress_size) {
					double percent = 100.0 * (double)stats.total_bytes / (double)progress_size;
					double alpha = secs * (double)progress_size / (double)stats.total_bytes;
					double secs_left = alpha - secs;

					fprintf(stderr,"Rate: %s/S, "
						"Total: %s, Dur: %.1f S, %5.1f%% ETA: %.1f S  \r",
						current_rate_str, total_bytes_str, secs,
						percent, secs_left);
				} else {
					/* No size, avoid division by zero */
					fprintf(stderr,"Rate: %s/S, "
						"Total: %s, Dur: %.1f S, ??.?%% ETA: ?.? S  \r",
						current_rate_str, total_bytes_str, secs);
				}
			} else {
				/* Default progress info */
				char io_size_str[32];

				size_to_str(io_size, "%7.1f %s",
					io_size_str,
					sizeof(io_size_str));
				fprintf(stderr,"Rate: %s/S, Adj: %c, "
					"Total: %s, Dur: %.1f S, Buf: %s  \r",
					current_rate_str, run, total_bytes_str,
					secs_now - secs_start, io_size_str);
			}
			(void)fflush(stderr);
			secs_last = secs_now;
		}

		if ((opt_flags & OPT_TIMED_RUN) &&
		    ((secs_now - secs_start) > timed_run))
			break;
	}
	ret = EXIT_SUCCESS;

finish:
	if (opt_flags & OPT_VERBOSE)
		fprintf(stderr, "%78s\r", "");

	if (opt_flags & OPT_STATS) {
		if ((stats.time_end = timeval_to_double()) < 0.0)
			goto tidy;
		stats_info(&stats);
	}
tidy:

	if ((fdin != -1) && (opt_flags & OPT_URANDOM)) {
		(void)close(fdin);
	}
	free(buffer);
	if (fdtee >= 0)
		(void)close(fdtee);
	exit(ret);
}
