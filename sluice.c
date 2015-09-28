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

#define UNDERRUN_MAX		(100)		/* Max underruns before warning, see -w */
#define UNDERRUN_ADJUST_MAX	(1)		/* Underruns before adjusting rate */
#define OVERRUN_ADJUST_MAX	(1)		/* Overruns before adjusting rate */

#define DELAY_SHIFT_MIN		(0)		/* Min shift, see -s */
#define DELAY_SHIFT_MAX		(16)		/* Max shift, see -s */

#define IO_SIZE_MIN		(1)		/* Min io buffer size, see -i */
#define IO_SIZE_MAX		(1ULL * GB)	/* Max io buffer size, see -i */

#define DELAY_MIN		(0.01)		/* Min delay time, see -c */
#define DELAY_MAX		(10.00)		/* Max delay time, see -c */

#define DATA_RATE_MIN		(0.1)		/* Min data rate, see -r */

#define FREQ_MIN		(0.01)		/* Min frequency, see -f */

#define DRIFT_MAX		(11)		/* Number of drift stats, see -S */
#define DRIFT_PERCENT_START	(0.0625)	/* Drift stats first point */
#define DEFAULT_FREQ		(0.250)		/* Default verbose feedback freq, see -f */

#define DEBUG_RATE		(0)		/* Set to non-zero to get datarate debug */
#define DEBUG_SETUP		(0)		/* Set to non-zero to dump setup state */

#define OPT_VERBOSE		(0x00000001)	/* -v */
#define OPT_GOT_RATE		(0x00000002)	/* -r */
#define OPT_GOT_IOSIZE		(0x00000004)	/* -i */
#define OPT_GOT_CONST_DELAY	(0x00000008)	/* -c */
#define OPT_WARNING		(0x00000010)	/* -w */
#define OPT_UNDERRUN		(0x00000020)	/* -u */
#define OPT_DISCARD_STDOUT	(0x00000040)	/* -d */
#define OPT_OVERRUN		(0x00000080)	/* -o */
#define OPT_ZERO		(0x00000100)	/* -z */
#define OPT_URANDOM		(0x00000200)	/* -R */
#define OPT_APPEND		(0x00000400)	/* -a */
#define OPT_STATS		(0x00000800)	/* -S */
#define OPT_NO_RATE_CONTROL	(0x00001000)	/* -n */
#define OPT_TIMED_RUN		(0x00002000)	/* -T */
#define OPT_INPUT_FILE		(0x00004000)	/* -I */
#define OPT_VERSION		(0x00008000)	/* -V */
#define OPT_PROGRESS		(0x00010000)	/* -p */
#define OPT_MAX_TRANS_SIZE	(0x00020000)	/* -m */
#define OPT_SKIP_READ_ERRORS	(0x00040000)	/* -e */
#define OPT_GOT_SHIFT		(0x00080000)	/* -s */

#define EXIT_BAD_OPTION		(1)
#define EXIT_FILE_ERROR		(2)
#define EXIT_DELAY_ERROR	(3)
#define EXIT_TIME_ERROR		(4)
#define EXIT_SIGNAL_ERROR	(5)
#define EXIT_READ_ERROR		(6)
#define EXIT_WRITE_ERROR	(7)
#define EXIT_ALLOC_ERROR	(8)

#define BUF_SIZE(sz)		((((size_t)sz) < 1) ? 1 : ((size_t)sz))

/* R = read, W = write, D = delay */
#define DELAY_R_W_D		(0x00000000)	/* full delay */
#define DELAY_D_R_W		(0x00000001)	/* full delay */
#define DELAY_R_D_W		(0x00000002)	/* full delay */
#define DELAY_D_R_D_W		(0x00000003)	/* 2 * 1/2 delay */
#define DELAY_R_D_W_D		(0x00000004)	/* 2 * 1/2 delay */
#define DELAY_D_R_D_W_D		(0x00000005)	/* 3 * 1/3 delay */

#define DELAY_MODE_MIN		0
#define DELAY_MODE_MAX		DELAY_D_R_D_W_D

#define DELAY_D			(0x01)		/* delay */
#define DELAY_S			(0x00)		/* skip */

#define DELAY_SET_ACTION(a1, a2, a3)	((a1 << 0) | (a2 << 1) | (a3 << 2))
#define DELAY_GET_ACTION(n, action)	((1 << n) & action)

typedef struct {
	double	divisor;			/* delay divisor */
	uint8_t	mode;				/* User specified mode */
	uint8_t	action;				/* action bit map */
} delay_info_t;

/*
 *  action bit#
 *	0		sleep on/off
 *			read
 *	1		sleep on/off
 *			write
 *	2		sleep on/off
 */
static delay_info_t delay_info[] = {
	{ 1.0, DELAY_R_W_D,	DELAY_SET_ACTION(DELAY_S, DELAY_S, DELAY_D) },
	{ 1.0, DELAY_D_R_W,	DELAY_SET_ACTION(DELAY_D, DELAY_S, DELAY_S) },
	{ 1.0, DELAY_R_D_W,     DELAY_SET_ACTION(DELAY_S, DELAY_D, DELAY_S) },
	{ 2.0, DELAY_D_R_D_W,   DELAY_SET_ACTION(DELAY_D, DELAY_D, DELAY_S) },
	{ 2.0, DELAY_R_D_W_D,   DELAY_SET_ACTION(DELAY_S, DELAY_D, DELAY_D) },
	{ 3.0, DELAY_D_R_D_W_D, DELAY_SET_ACTION(DELAY_D, DELAY_D, DELAY_D) },
};

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
	uint64_t	delays;		/* Count of delays */
	uint64_t	reallocs;	/* Count of buffer reallocations */
	uint64_t	perfect;	/* Count of no under/overruns */
	uint64_t	io_size_min;	/* Minimum buffer size */
	uint64_t	io_size_max;	/* Maximum buffer size */
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

static unsigned int opt_flags;
static const char *app_name = "sluice";
static const char *dev_urandom = "/dev/urandom";
static volatile bool sluice_finish = false;

static const scale_t byte_scales[] = {
	{ 'b',	1ULL },
	{ 'k',  1ULL << 10 },	/* Kilobytes */
	{ 'm',  1ULL << 20 },	/* Megabytes */
	{ 'g',  1ULL << 30 },	/* Gigabytes */
	{ 't',  1ULL << 40 },	/* Terabytes */
	{ 'p',	1ULL << 60 },	/* Petabytes */
	{ 0,    0 },
};

static const scale_t time_scales[] = {
	{ 's',  1 },
	{ 'm',  60 },
	{ 'h',  3600 },
	{ 'd',  24 * 3600 },
	{ 'y',  365 * 24 * 3600 },
};

static const scale_t second_scales[] = {
	{ 's',	1 },
	{ 'm',	60 },
	{ 'h',  3600 },
	{ 'd',  24 * 3600 },
	{ 'w',  7 * 24 * 3600 },
	{ 'y',  365 * 24 * 3600 },
	{ ' ',  INT64_MAX },
};

/*
 *  count_bits()
 *      count bits set, from C Programming Language 2nd Ed
 */
static inline unsigned int count_bits(const unsigned int val)
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
	stats->delays = 0;
	stats->reallocs = 0;
	stats->perfect = 0;
	stats->io_size_min = 0;
	stats->io_size_max = 0;
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
 *  secs_to_str()
 *	report seconds in different units.
 */
static const char *secs_to_str(const double secs)
{
	static char buf[64];
	int i;

	for (i = 0; i < 5; i++) {
		if (secs <= second_scales[i + 1].scale)
			break;
	}
	snprintf(buf, sizeof(buf), "%.2f %c",
		secs / second_scales[i].scale, second_scales[i].ch);
	return buf;
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
static const char *double_to_str(const double val)
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
	double secs = stats->time_end - stats->time_begin;
	double avg_wr_sz;
	struct tms t;

	if (secs <= 0.0)  {
		fprintf(stderr, "Cannot compute statistics\n");
		return;
	}
	avg_wr_sz = stats->writes ?
		stats->buf_size_total / stats->writes : 0.0;
	fprintf(stderr, "Data:             %s\n",
		double_to_str((double)stats->total_bytes));
	fprintf(stderr, "Reads:            %" PRIu64 "\n",
		stats->reads);
	fprintf(stderr, "Writes:           %" PRIu64 "\n",
		stats->writes);
	fprintf(stderr, "Avg. Write Size:  %s\n",
		double_to_str(avg_wr_sz));
	fprintf(stderr, "Duration:         %s\n",
		secs_to_str(secs));
	fprintf(stderr, "Delays:           %" PRIu64 "\n",
		stats->delays);
	fprintf(stderr, "Buffer reallocs:  %" PRIu64 "\n",
		stats->reallocs);
	fprintf(stderr, "\n");
	if (!(opt_flags & OPT_NO_RATE_CONTROL)) {
		fprintf(stderr, "Target rate:      %s/s\n",
			double_to_str(stats->target_rate));
	}
	fprintf(stderr, "Average rate:     %s/s\n",
		double_to_str((double)stats->total_bytes / secs));
	fprintf(stderr, "Minimum rate:     %s/s\n",
		double_to_str(stats->rate_min));
	fprintf(stderr, "Maximum rate:     %s/s\n",
		double_to_str(stats->rate_max));
	fprintf(stderr, "Minimum buffer:   %s\n",
		double_to_str((double)stats->io_size_min));
	fprintf(stderr, "Maximum buffer:   %s\n",
		double_to_str((double)stats->io_size_max));
	if (times(&t) != (clock_t)-1) {
		/* CPU utilitation stats, if available */
		long int ticks_per_sec;

		if ((ticks_per_sec = sysconf(_SC_CLK_TCK)) > 0) {
			fprintf(stderr, "User time:        %s\n",
				secs_to_str((double)t.tms_utime /
					(double)ticks_per_sec));
			fprintf(stderr, "System time:      %s\n",
				secs_to_str((double)t.tms_stime /
					(double)ticks_per_sec));
			fprintf(stderr, "Total delay time: %s\n",
				secs_to_str(secs - (double)(t.tms_utime +
					t.tms_stime) / (double)ticks_per_sec));
		}
	}

	if (!(opt_flags & OPT_NO_RATE_CONTROL)) {
		/* The following only make sense if we have rate stats */
		int i;
		uint64_t drift_sum = 0;
		double last_percent = 0.0, percent = DRIFT_PERCENT_START;
		double total = stats->underruns +
			       stats->overruns + stats->perfect;

		fprintf(stderr, "Overruns:         %6.2f%%\n", total ?
			100.0 * (double)stats->underruns / total : 0.0);
		fprintf(stderr, "Underruns:        %6.2f%%\n", total ?
			100.0 * (double)stats->overruns / total : 0.0);

		fprintf(stderr, "\nDrift from target rate: (%%)\n");
		for (i = 0; i < DRIFT_MAX; i++, percent *= 2.0) {
			fprintf(stderr, "  %6.3f%% - %6.3f%%: %6.2f%%\n",
				last_percent, percent - 0.0001,
				stats->drift_total ?
					100.0 * (double)stats->drift[i] /
					(double)stats->drift_total : 0.0);
			last_percent = percent;
			drift_sum += stats->drift[i];
		}
		fprintf(stderr, " >%6.3f%%          : %6.2f%%\n",
			(double)last_percent,
			stats->drift_total ?
				100.0 - ((100.0 * (double)drift_sum) /
				(double)stats->drift_total) : 0.0);
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
	errno = 0;			/* clear to be safe */
	if (gettimeofday(&tv, NULL) < 0) {
		if (errno == EINTR)	/* should not occur */
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
		exit(EXIT_BAD_OPTION);
	}
	if (*len == 0) {
		fprintf(stderr, "Value %s is an invalid size.\n", str);
		exit(EXIT_BAD_OPTION);
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
		exit(EXIT_BAD_OPTION);
	}
	if (*len == 0) {
		fprintf(stderr, "Value %s is an invalid size.\n", str);
		exit(EXIT_BAD_OPTION);
	}
	return val;
}


/*
 *  get_double_scale()
 *	get a value and scale it by the given scale factor
 */
static double get_double_scale(
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
		exit(EXIT_BAD_OPTION);
	}

	if (isdigit(ch) || ch == '.')
		return val;

	ch = tolower(ch);
	for (i = 0; scales[i].ch; i++) {
		if (ch == scales[i].ch)
			return val * scales[i].scale;
	}

	printf("Illegal %s specifier %c\n", msg, str[len]);
	exit(EXIT_BAD_OPTION);
}

/*
 *  get_uint64_scale()
 *	get a value and scale it by the given scale factor
 */
static inline uint64_t get_uint64_scale(
	const char *const str,
	const scale_t scales[],
	const char *const msg)
{
	return (uint64_t)get_double_scale(str, scales, msg);
}

/*
 *  get_uint64_byte()
 *	size in bytes, K bytes, M bytes, G bytes, T bytes or P bytes
 */
static inline uint64_t get_uint64_byte(const char *const str)
{
	return get_uint64_scale(str, byte_scales, "length");
}

/*
 *  get_double_byte()
 *	size in bytes, K bytes, M bytes, G bytes, T bytes or P bytes
 */
static inline double get_double_byte(const char *const str)
{
	return get_double_scale(str, byte_scales, "length");
}

/*
 *  get_uint64_time()
 *	time in seconds, minutes, hours, days or years
 */
static inline uint64_t get_uint64_time(const char *const str)
{
	return get_uint64_scale(str, time_scales, "time");
}

/*
 *  show_usage()
 *	show options
 */
static void show_usage(void)
{
	printf("%s, version %s\n\n", app_name, VERSION);
	printf("Usage: %s [options]\n", app_name);
	printf("  -a         append to file (-t, -O options only).\n");
	printf("  -c delay   specify constant delay time (seconds).\n");
	printf("  -d         discard output (no output).\n");
	printf("  -D         delay mode.\n");
	printf("  -e         skip read errors.\n");
	printf("  -f freq    frequency of -v statistics.\n");
	printf("  -h         print this help.\n");
	printf("  -i size    set io read/write size in bytes.\n");
	printf("  -m size    set maximum amount to process.\n");
	printf("  -n         no rate controls, just copy data untouched.\n");
	printf("  -o         shrink read/write buffer to avoid overrun.\n");
	printf("  -O file    short cut for -dt file; output to a file.\n");
	printf("  -p         enable verbose mode with progress stats.\n");
	printf("  -P pidfile save process ID intp file pidfile.\n");
	printf("  -r rate    set rate (in bytes per second).\n");
	printf("  -R	     ignore stdin, read from %s.\n", dev_urandom);
	printf("  -s shift   controls delay or buffer size adjustment.\n");
	printf("  -S         display statistics at end of stream to stderr.\n");
	printf("  -t file    tee output to file.\n");
	printf("  -T time    stop after a specified amount of time.\n");
	printf("  -u         expand read/write buffer to avoid underrun.\n");
	printf("  -v         set verbose mode (to stderr).\n");
	printf("  -V         print version information.\n");
	printf("  -w         warn on data rate underrun.\n");
	printf("  -z         ignore stdin, generate zeros.\n");
}

#define DELAY(delay, stats)						\
	if (delay > 0) {						\
		stats.delays++;						\
		if (usleep((useconds_t)delay) < 0) {			\
			if (errno == EINTR) {				\
				if (sluice_finish)			\
					goto finish;			\
				/*					\
				 * usleep got interrupted, let		\
				 * subsequent I/O cater with the	\
				 * delay deltas rather than		\
				 * trying to figure out how much	\
				 * time was lost on early exit		\
				 * from usleep				\
				 */					\
			} else {					\
				fprintf(stderr, "usleep error: "	\
					"errno=%d (%s).\n",		\
					errno, strerror(errno));	\
				ret = EXIT_DELAY_ERROR;			\
				goto tidy;				\
			}						\
		}							\
	}

#define DO_DELAY(delay, di, n, stats)					\
	if (DELAY_GET_ACTION(n, di->action))				\
		DELAY(delay / di->divisor, stats);

static delay_info_t *get_delay_info(uint64_t delay_mode)
{
	int i;

	if (delay_mode > DELAY_MODE_MAX) {
		fprintf(stderr, "Delay mode -D %" PRIu64
			" is too large, range 0..%u.\n",
			delay_mode, DELAY_MODE_MAX);
		return NULL;
	}

	for (i = 0; i <= DELAY_MODE_MAX; i++) {
		if (delay_info[i].mode == delay_mode)
			return &delay_info[i];
	}
	fprintf(stderr, "Cannot find delay mode %" PRIu64 ".\n",
			delay_mode);
	return NULL;
}

int main(int argc, char **argv)
{
	char run = ' ';			/* Overrun/underrun flag */
	char *buffer = NULL;		/* Temp I/O buffer */
	char *out_filename = NULL;	/* -t or -O option filename */
	char *in_filename = NULL;	/* -I option filename */
	char *pid_filename = NULL;	/* -P option filename */

	double delay;
	double io_size = 0.9;		/* -i IO buffer size */
	double data_rate = 0.0;		/* -r data rate */
	double secs_start, secs_last, freq = DEFAULT_FREQ;
	double const_delay = -1.0;	/* -c delay time between I/O */

	uint64_t last_delay = 0;	/* Delays in 1/1000000 of a second */
	uint64_t total_bytes = 0;	/* cumulative number of bytes read */
	uint64_t max_trans = 0;		/* -m maximum data transferred */
	uint64_t adjust_shift = 0;	/* -s adjustment scaling shift */
	uint64_t timed_run = 0;		/* -T timed run duration */
	uint64_t delay_mode = DELAY_R_W_D; /* read, write then delay */

	off_t progress_size = 0;

	int underrun_adjust = UNDERRUN_ADJUST_MAX;
	int overrun_adjust = OVERRUN_ADJUST_MAX;
	int fdin = -1, fdout, fdtee = -1;
	int underruns = 0, overruns = 0, warnings = 0;
	int ret = EXIT_SUCCESS;

	bool eof = false;		/* EOF on input */

	stats_t stats;			/* Data rate statistics */
	struct sigaction new_action;
	delay_info_t *di = NULL;

	stats_init(&stats);

	for (;;) {
		const int c = getopt(argc, argv,
			"ar:h?i:vm:wudot:f:zRs:c:O:SnT:I:VpeD:P:");
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
		case 'D':
			delay_mode = get_uint64(optarg, &len);
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
			io_size = (double)get_uint64_byte(optarg);
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
		case 'P':
			pid_filename = optarg;
			break;
		case 'r':
			data_rate = get_double_byte(optarg);
			opt_flags |= OPT_GOT_RATE;
			break;
		case 'R':
			opt_flags |= OPT_URANDOM;
			break;
		case 's':
			opt_flags |= OPT_GOT_SHIFT;
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
			exit(EXIT_BAD_OPTION);
		default:
			show_usage();
			exit(EXIT_BAD_OPTION);
		}
	}

	if (pid_filename) {
		FILE *pid_file = fopen(pid_filename, "w");
		if (pid_file) {
			fprintf(pid_file, "%d\n", getpid());
			(void)fclose(pid_file);
		} else {
			fprintf(stderr, "Cannot create pid file '%s', errno=%d (%s).\n",
				pid_filename, errno, strerror(errno));
			ret = EXIT_FILE_ERROR;
			goto tidy;
		}
	}
	if ((di = get_delay_info(delay_mode)) == NULL) {
		ret = EXIT_FILE_ERROR;
		goto tidy;
	}
	if ((opt_flags & OPT_NO_RATE_CONTROL) &&
            (opt_flags & (OPT_GOT_CONST_DELAY | OPT_GOT_RATE | OPT_UNDERRUN | OPT_OVERRUN))) {
		fprintf(stderr, "Cannot use -n option with -c, -r, -u or -o options.\n");
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if (!out_filename && (opt_flags & OPT_APPEND)) {
		fprintf(stderr, "Must use -t filename when using the -a option.\n");
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if (!(opt_flags & (OPT_GOT_RATE | OPT_NO_RATE_CONTROL))) {
		fprintf(stderr, "Must specify data rate with -r option (or use -n for no rate control).\n");
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if ((opt_flags & (OPT_GOT_IOSIZE | OPT_GOT_CONST_DELAY)) ==
	    (OPT_GOT_IOSIZE | OPT_GOT_CONST_DELAY)) {
		fprintf(stderr, "Cannot use both -i and -c options together.\n");
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if ((opt_flags & OPT_GOT_RATE) && (data_rate < DATA_RATE_MIN)) {
		fprintf(stderr, "Rate value %.2f too low. Minimum allowed is %.2f bytes/sec.\n",
			data_rate, DATA_RATE_MIN);
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if (freq < FREQ_MIN) {
		fprintf(stderr, "Frequency %.3f too low. Minimum allowed is %.3f Hz.\n",
			freq, FREQ_MIN);
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
#if DELAY_SHIFT_MIN > 0
	if (adjust_shift < DELAY_SHIFT_MIN || adjust_shift > DELAY_SHIFT_MAX) {
#else
	if (adjust_shift > DELAY_SHIFT_MAX) {
#endif
		fprintf(stderr, "Delay shift must be %d .. %d.\n",
			DELAY_SHIFT_MIN, DELAY_SHIFT_MAX);
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if ((opt_flags & OPT_GOT_CONST_DELAY) &&
	    (const_delay < DELAY_MIN || const_delay > DELAY_MAX)) {
		fprintf(stderr, "Delay time must be %.2f .. %.2f seconds.\n",
			DELAY_MIN, DELAY_MAX);
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}

	/*
	 *  No size specified, then default rate
	 */
	if (!(opt_flags & OPT_GOT_IOSIZE)) {
		if (opt_flags & OPT_GOT_CONST_DELAY) {
			io_size = data_rate * const_delay;
			if (io_size < IO_SIZE_MIN) {
				fprintf(stderr, "Delay too small, internal buffer too small.\n");
				ret = EXIT_BAD_OPTION;
				goto tidy;
			}
			if (io_size > IO_SIZE_MAX) {
				fprintf(stderr, "Delay too large, internal buffer too big.\n");
				ret = EXIT_BAD_OPTION;
				goto tidy;
			}
		}
		else {
			if (opt_flags & OPT_NO_RATE_CONTROL) {
				io_size = 4 * KB;
			} else {
				/*
				 * User has not specified -i or -c, so define
				 * the io_size based on 1/32 of the data rate,
				 * e.g. ~32 writes per second
				 */
				io_size = data_rate / 32.0;
				/* Make sure we don't have small sized I/O */
				if (io_size < IO_SIZE_MIN)
					io_size = IO_SIZE_MIN;
				if (io_size > IO_SIZE_MAX) {
					fprintf(stderr, "Rate too high for the buffer size, maximum allowed: %s/sec.\n",
						double_to_str((double)IO_SIZE_MAX * 32.0));
					fprintf(stderr, "Use -i to explicitly set a larger buffer size.\n");
					ret = EXIT_BAD_OPTION;
					goto tidy;
				}
			}
		}
	}
	if (opt_flags & OPT_MAX_TRANS_SIZE)
		if (io_size > max_trans)
			io_size = (double)max_trans;

	if ((io_size < IO_SIZE_MIN) || (io_size > IO_SIZE_MAX)) {
		fprintf(stderr, "I/O buffer size too large, maximum allowed: %s.\n",
			double_to_str((double)IO_SIZE_MAX));
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}
	if ((buffer = malloc(BUF_SIZE(io_size))) == NULL) {
		fprintf(stderr,"Cannot allocate buffer of %.0f bytes.\n",
			io_size);
		ret = EXIT_ALLOC_ERROR;
		goto tidy;
	}
	if (opt_flags & OPT_ZERO)
		memset(buffer, 0, (size_t)io_size);

	if (count_bits(opt_flags & (OPT_ZERO | OPT_URANDOM | OPT_INPUT_FILE)) > 1) {
		fprintf(stderr, "Cannot use -z, -R or -I options together.\n");
		ret = EXIT_BAD_OPTION;
		goto tidy;
	}

	if (opt_flags & OPT_INPUT_FILE) {
		struct stat buf;

		fdin = open(in_filename, O_RDONLY);
		if (fdin < 0) {
			fprintf(stderr, "open on %s failed: errno = %d (%s).\n",
				in_filename, errno, strerror(errno));
			ret = EXIT_FILE_ERROR;
			goto tidy;
		}
		if (fstat(fdin, &buf) < 0) {
			fprintf(stderr, "fstat on file %s failed: errno = %d (%s).\n",
				in_filename, errno, strerror(errno));
			progress_size = 0;
		} else {
			progress_size = buf.st_size;
		}
	}
	if (opt_flags & OPT_MAX_TRANS_SIZE)
		progress_size = (off_t)max_trans;

	if (opt_flags & OPT_URANDOM) {
		fdin = open(dev_urandom, O_RDONLY);
		if (fdin < 0) {
			fprintf(stderr, "Cannot open %s: errno=%d (%s).\n",
				dev_urandom, errno, strerror(errno));
			ret = EXIT_FILE_ERROR;
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
			ret = EXIT_FILE_ERROR;
			goto tidy;
		}
	}

	/* Default to stdin if not specified */
	if (fdin == -1)
		fdin = fileno(stdin);
	fdout = fileno(stdout);

	if ((secs_start = timeval_to_double()) < 0.0) {
		ret = EXIT_TIME_ERROR;
		goto tidy;
	}

	if (opt_flags & OPT_NO_RATE_CONTROL) {
		delay = 0.0;
	} else if (opt_flags & OPT_GOT_CONST_DELAY) {
		delay = 1000000.0 * const_delay;
	} else {
		delay = io_size * 1000000.0 / (double)data_rate;
	}

#if DEBUG_SETUP
	fprintf(stderr, "io_size:         %.0f\n", io_size);
	fprintf(stderr, "data_rate:       %f\n", data_rate);
	fprintf(stderr, "const_delay:     %f\n", const_delay);
	fprintf(stderr, "delay:           %f\n", delay);
	fprintf(stderr, "progress_size:   %" PRIu64 "\n",
		(uint64_t)progress_size);
	fprintf(stderr, "max_trans:       %" PRIu64 "\n", max_trans);
	fprintf(stderr, "shift:           %" PRIu64 "\n", adjust_shift);
#endif
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
		ret = EXIT_SIGNAL_ERROR;
		goto tidy;
	}

	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_handler = handle_siginfo;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	if (sigaction(SIGUSR1, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		ret = EXIT_SIGNAL_ERROR;
		goto tidy;
	}
#ifdef SIGINFO
	if (sigaction(SIGINFO, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		ret = EXIT_SIGNAL_ERROR;
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
		ret = EXIT_SIGNAL_ERROR;
		goto tidy;
	}

	/*
	 *  Main loop:
	 *	read data until buffer is full
	 *	write data
	 *	get new data rate
	 *	adjust delay or buffer size
	 *	check for timeout
	 */
	while (!(eof | sluice_finish)) {
		uint64_t inbufsize = 0;
		bool complete = false;
		double current_rate, secs_now;

		DO_DELAY(delay, di, 0, stats);

		if (opt_flags & OPT_ZERO) {
			inbufsize = (uint64_t)io_size;
			total_bytes += (uint64_t)io_size;
			stats.reads++;
		} else {
			char *ptr = buffer;

			while (!complete && (inbufsize < (uint64_t)io_size)) {
				uint64_t sz = (uint64_t)io_size - inbufsize;
				ssize_t n;

				/*
				 * We hit the user specified max
				 * limit to transfer
				 */
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
					/* Ignore errors? */
					if (opt_flags & OPT_SKIP_READ_ERRORS) {
						/* Ensure block is empty */
						memset(ptr, 0, sz);
						n = sz;
					} else {
						fprintf(stderr,"read error: errno=%d (%s).\n",
							errno, strerror(errno));
						ret = EXIT_READ_ERROR;
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
		if (eof)
			break;

		DO_DELAY(delay, di, 1, stats);

		stats.writes++;
		stats.total_bytes += inbufsize;
		stats.buf_size_total += inbufsize;
		if (!(opt_flags & OPT_DISCARD_STDOUT)) {
			if (write(fdout, buffer, (size_t)inbufsize) < 0) {
				fprintf(stderr,"Write error: errno=%d (%s).\n",
					errno, strerror(errno));
				ret = EXIT_WRITE_ERROR;
				goto tidy;
			}
		}

		/* -t Tee mode output */
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
					ret = EXIT_WRITE_ERROR;
					goto tidy;
				}
			}
		}
		if (eof)
			break;

		DO_DELAY(delay, di, 2, stats);

		if ((secs_now = timeval_to_double()) < 0.0) {
			ret = EXIT_TIME_ERROR;
			goto tidy;
		}
		current_rate = ((double)total_bytes) / (secs_now - secs_start);

		/* Update min/max rate stats */
		if (stats.rate_set) {
			if (current_rate > stats.rate_max)
				stats.rate_max = current_rate;
			if (current_rate < stats.rate_min)
				stats.rate_min = current_rate;
			if (io_size > stats.io_size_max)
				stats.io_size_max = io_size;
			if (io_size < stats.io_size_min)
				stats.io_size_min = io_size;
		} else {
			stats.rate_min = current_rate;
			stats.rate_max = current_rate;
			stats.io_size_min = io_size;
			stats.io_size_max = io_size;
			stats.rate_set = true;
		}

		/* Update drift stats only if we have rate controls enabled */
		if (!(opt_flags & OPT_NO_RATE_CONTROL)) {
			double drift_rate = 100.0 *
				fabs(current_rate - data_rate) / data_rate;
			int i;
			double percent = DRIFT_PERCENT_START;

			stats.drift_total++;
			for (i = 0; i < DRIFT_MAX; i++, percent *= 2.0) {
				if (drift_rate < percent) {
					stats.drift[i]++;
					break;
				}
			}
		}
#if DEBUG_RATE
		fprintf(stderr, "rate-pre : %.2f delay: %.2f io_size: %.3f\n",
			current_rate, delay, io_size);
#endif

		if (opt_flags & OPT_NO_RATE_CONTROL) {
			/* No rate to compare to */
			run = '-';
		} else {
			if (current_rate > data_rate) {
				/* Overrun */
				run = '+' ;
				if (!(opt_flags & OPT_GOT_CONST_DELAY)) {
					if (adjust_shift)
						delay += ((last_delay >> adjust_shift) + 100);
					else {
						double secs_desired = secs_start + ((total_bytes + inbufsize) / data_rate);
						delay = 1000000.0 * (secs_desired - secs_now);
						if (delay < 0)
							delay = 0;
					}
				}
				warnings = 0;
				underruns = 0;
				overruns++;
				stats.overruns++;
			} else if (current_rate < data_rate) {
				/* Underrun */
				run = '-' ;
				if (!(opt_flags & OPT_GOT_CONST_DELAY)) {
					if (adjust_shift)
						delay -= ((last_delay >> adjust_shift) + 100);
					else {
						double secs_desired = secs_start + ((total_bytes + inbufsize) / data_rate);
						delay = 1000000.0 * (secs_desired - secs_now);
						if (delay < 0)
							delay = 0;
					}
				}
				warnings++;
				underruns++;
				stats.underruns++;
				overruns = 0;
			} else {
				/* Perfect, rather unlikely.. */
				warnings = 0;
				underruns = 0;
				overruns = 0;
				stats.perfect++;
				run = '0';
			}

			/* Avoid the impossible */
			if (delay < 0)
				delay = 0;

			if ((opt_flags & OPT_UNDERRUN) &&
			    (underruns >= underrun_adjust)) {
				/* Adjust rate due to underruns */
				char *tmp;
				double tmp_io_size;

				if (adjust_shift) {
					/* Adjust by scaling io_size */
					tmp_io_size = io_size + (io_size / (1 << adjust_shift));
					/*
					 * If size is too small, we get
					 * stuck at 1
					 */
					if (tmp_io_size < 1)
						tmp_io_size = 1;
				} else {
					/*
					 * Adjust by comparing differences
					 * in rates
					*/
					tmp_io_size = io_size +
						(data_rate - current_rate) *
						const_delay;
				}

				/* Need to grow buffer? */
				if ((tmp_io_size > io_size) &&
				    (tmp_io_size < IO_SIZE_MAX)) {
					stats.reallocs++;
					tmp = realloc(buffer, BUF_SIZE(tmp_io_size));
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
			    (overruns >= overrun_adjust)) {
				/* Adjust rate due to overruns */
				char *tmp;
				double tmp_io_size;

				if (adjust_shift) {
					/* Adjust by scaling io_size */
					tmp_io_size = io_size -
						(io_size / (1 << adjust_shift));
					/*
					 * If size is too small, we get
					 * stuck at 1
					 */
					if (tmp_io_size < 1)
						tmp_io_size = 1;
				} else {
					/*
					 * Adjust by comparing differences
					 * in rates
					 */
					tmp_io_size = io_size +
						(data_rate - current_rate) *
						const_delay;
				}

				/* Need to grow buffer? */
				if ((tmp_io_size > io_size) &&
				    (tmp_io_size < IO_SIZE_MAX)) {
					stats.reallocs++;
					tmp = realloc(buffer, BUF_SIZE(tmp_io_size));
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
		last_delay = (uint64_t)delay;

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
					double percent = 100.0 *
						(double)progress_size /
						(double)stats.total_bytes;
					double alpha = secs *
						(double)progress_size /
						(double)stats.total_bytes;
					double secs_left = alpha - secs;

					fprintf(stderr,"Rate: %s/S, "
						"Total: %s, Dur: %.1f S, %5.1f%% ETA: %s  \r",
						current_rate_str, total_bytes_str, secs,
						percent, secs_to_str(secs_left));
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
#if DEBUG_RATE
		fprintf(stderr, "rate-post: %.2f delay: %.2f io_size: %.3f\n",
			current_rate, delay, io_size);
#endif
		/* Timed run, if we timed out then stop */
		if ((opt_flags & OPT_TIMED_RUN) &&
		    ((secs_now - secs_start) > timed_run))
			break;
		if (max_trans && total_bytes >= max_trans)
			break;
	}
	ret = EXIT_SUCCESS;

finish:
	if (opt_flags & OPT_VERBOSE)
		fprintf(stderr, "%78s\r", "");

	if (opt_flags & OPT_STATS) {
		if ((stats.time_end = timeval_to_double()) < 0.0) {
			ret = EXIT_TIME_ERROR;
			goto tidy;
		}
		stats_info(&stats);
	}
tidy:
	if (pid_filename) {
		(void)unlink(pid_filename);
	}

	if ((fdin != -1) && (opt_flags & OPT_URANDOM)) {
		(void)close(fdin);
	}
	free(buffer);
	if (fdtee >= 0)
		(void)close(fdtee);
	exit(ret);
}
