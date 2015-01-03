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
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>

#define KB			(1024ULL)
#define MB			(KB * KB)
#define GB			(KB * KB * KB)

#define UNDERRUN_MAX		(100)
#define UNDERRUN_ADJUST_MAX	(10)
#define OVERRUN_ADJUST_MAX	(10)

#define DELAY_SHIFT_MIN		(1)
#define DELAY_SHIFT_MAX		(16)

#define IO_SIZE_MAX		(MB * 64)
#define IO_SIZE_MIN		(1)

#define DELAY_MIN		(0.01)
#define DELAY_MAX		(10.00)

#define DEFAULT_FREQ		(0.33333333)

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

static int opt_flags;
static const char *app_name = "sluice";
static const char *dev_urandom = "/dev/urandom";
static volatile bool sluice_finish = false;

typedef struct {
	const char ch;		/* Scaling suffix */
	const uint64_t  scale;	/* Amount to scale by */
} scale_t;

/* various statistics */
typedef struct {
	uint64_t	reads;
	uint64_t	writes;
	uint64_t	total_bytes;
	uint64_t	underruns;
	uint64_t	overruns;
	uint64_t	perfect;
	double		time_begin;
	double		time_end;
	double		target_rate;
	double		buf_size_total;
} stats_t;

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
 *  stats_init()
 *	Initialize statistics
 */
static inline void stats_init(stats_t *stats)
{
	stats->reads = 0;
	stats->writes = 0;
	stats->total_bytes = 0;
	stats->underruns = 0;
	stats->overruns = 0;
	stats->perfect = 0;
	stats->time_begin = 0.0;
	stats->time_end = 0.0;
	stats->target_rate = 0.0;
	stats->buf_size_total = 0.0;
}

/*
 *  double_to_str()
 *	convert double size in bytes to string
 */
static char *double_to_str(double val)
{
	int i;
	static char buf[64];
	static char *sizes[] = {
		"",
		"K",
		"M",
		"G",
		"T",
		"P",
	};

	for (i = 0; i < 5; i++) {
		if (val > 512.0)
			val /= 1024.0;
		else
			break;
	}
	snprintf(buf, sizeof(buf), "%.2f%s", val, sizes[i]);

	return buf;
}

/*
 *  stats_info()
 *	display run time statistics
 */
static void stats_info(stats_t *stats)
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
	fprintf(stderr, "Avg. Write Size: %sB\n",
		double_to_str(avg_wr_sz));
	fprintf(stderr, "Duration:        %.3f secs\n",
		secs);
	fprintf(stderr, "Target rate:     %s/sec\n",
		double_to_str(stats->target_rate));
	fprintf(stderr, "Actual rate:     %s/sec\n",
		double_to_str((double)stats->total_bytes / secs));
	fprintf(stderr, "Overruns:        %3.2f%%\n", total ?
		100.0 * (double)stats->underruns / total : 0.0);
	fprintf(stderr, "Underruns:       %3.2f%%\n", total ?
		100.0 * (double)stats->overruns / total : 0.0);

	if (times(&t) != (clock_t)-1) {
		long int ticks_per_sec;

		if ((ticks_per_sec = sysconf(_SC_CLK_TCK)) >= 0) {
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
static inline double timeval_to_double(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) < 0) {
		if (errno != EINTR) {
			fprintf(stderr, "gettimeofday error: errno=%d (%s).\n",
				errno, strerror(errno));
		}
		return -1.0;
	}
	return (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
}

/*
 *  size_to_str()
 *	report size in different units
 */
static void size_to_str(
	const uint64_t val,
	char *buf,
	const size_t buflen)
{
	double s;
	char *units;

	memset(buf, 0, buflen);

	if (val < 10 * KB) {
		s = (double)val;
		units = "B";
	} else if (val < 10 * MB) {
		s = (double)val / KB;
		units = "KB";
	} else if (val < 10 * GB) {
		s = (double)val / MB;
		units = "MB";
	} else {
		s = (double)val / GB;
		units = "GB";
	}
	snprintf(buf, buflen, "%7.1f %s", s, units);
}

/*
 *  get_uint64()
 *	get a uint64 value
 */
static uint64_t get_uint64(const char *const str, size_t *len)
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
static double get_double(const char *const str, size_t *len)
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
 *	size in bytes, K bytes, M bytes, G bytes or T bytes
 */
static uint64_t get_uint64_byte(const char *const str)
{
	static const scale_t scales[] = {
		{ 'b', 	1ULL },
		{ 'k',  1ULL << 10 },
		{ 'm',  1ULL << 20 },
		{ 'g',  1ULL << 30 },
		{ 't',  1ULL << 40 },
		{ 0,    0 },
	};

	return get_uint64_scale(str, scales, "length");
}

/*
 *  show_usage()
 *	show options
 */
static void show_usage(void)
{
	printf("%s, version %s\n\n", app_name, VERSION);
	printf("Usage: %s [options]\n", app_name);
	printf("  -a        append to file (-t option only).\n");
	printf("  -c delay  specify constant delay time (seconds).\n");
	printf("  -d        discard input (no output).\n");
	printf("  -f freq   frequency of -v statistics.\n");
	printf("  -h        print this help.\n");
	printf("  -i size   set io read/write size in bytes.\n");
	printf("  -m size   set maximum amount to process.\n");
	printf("  -o        shrink read/write buffer to avoid overrun.\n");
	printf("  -O file   short cut for -dt file; output to a file.\n");
	printf("  -r rate   set rate (in bytes per second).\n");
	printf("  -R	    ignore stdin, read from %s.\n", dev_urandom);
	printf("  -s shift  delay shift, controls delay adjustment.\n");
	printf("  -S        display statistics at end of stream to sterr.\n");
	printf("  -t file   tee output to file.\n");
	printf("  -u        expand read/write buffer to avoid underrun.\n");
	printf("  -v        set verbose mode (to stderr).\n");
	printf("  -w        warn on data rate underrun.\n");
	printf("  -z        ignore stdin, generate zeros.\n");
}

int main(int argc, char **argv)
{
	char run = ' ';			/* Overrun/underrun flag */
	char *buffer = NULL;		/* Temp I/O buffer */
	char *filename = NULL;		/* -t option filename */
	int64_t delay, last_delay = 0;	/* Delays in 1/1000000 of a second */
	uint64_t io_size = 0;
	uint64_t data_rate = 0;
	uint64_t total_bytes = 0;
	uint64_t max_trans = 0;
	uint64_t delay_shift = 3;
	int underrun_adjust = UNDERRUN_ADJUST_MAX;
	int overrun_adjust = OVERRUN_ADJUST_MAX;
	int fdin = -1, fdout, fdtee = -1;
	int warnings = 0;
	int underruns = 0, overruns = 0;
	int ret = EXIT_FAILURE;
	double secs_start, secs_last, freq = DEFAULT_FREQ;
	double const_delay = -1.0;
	bool eof = false;
	stats_t stats;
	struct sigaction new_action;

	stats_init(&stats);

	for (;;) {
		size_t len;
		int c = getopt(argc, argv, "ar:h?i:vm:wudot:f:zRs:c:O:S");
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
		case 'f':
			freq = atof(optarg);
			break;
		case 'h':
			show_usage();
			exit(EXIT_SUCCESS);
		case 'i':
			io_size = get_uint64_byte(optarg);
			opt_flags |= OPT_GOT_IOSIZE;
			break;
		case 'm':
			max_trans = get_uint64_byte(optarg);
			break;
		case 'o':
			opt_flags |= OPT_OVERRUN;
			break;
		case 'O':
			opt_flags |= OPT_DISCARD_STDOUT;
			filename = optarg;
			break;
		case 'r':
			data_rate = get_uint64_byte(optarg);
			opt_flags |= OPT_GOT_RATE;
			break;
		case 'R':
			opt_flags |= OPT_URANDOM;
			break;
		case 's':
			delay_shift = get_uint64(optarg, &len);
			break;
		case 'S':
			opt_flags |= OPT_STATS;
			break;
		case 't':
			filename = optarg;
			break;
		case 'u':
			opt_flags |= OPT_UNDERRUN;
			break;
		case 'v':
			opt_flags |= OPT_VERBOSE;
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

	if (!filename && (opt_flags & OPT_APPEND)) {
		fprintf(stderr, "Must use -t filename when using the -a option.\n");
		goto tidy;
	}
	if (!(opt_flags & OPT_GOT_RATE)) {
		fprintf(stderr, "Must specify data rate with -r option.\n");
		goto tidy;
	}
	if ((opt_flags & (OPT_GOT_IOSIZE | OPT_GOT_CONST_DELAY)) ==
	    (OPT_GOT_IOSIZE | OPT_GOT_CONST_DELAY)) {
		fprintf(stderr, "Cannot use both -i and -c options together.\n");
		goto tidy;
	}
	if (data_rate < 1) {
		fprintf(stderr, "Rate value %" PRIu64 " too low.\n", data_rate);
		goto tidy;
	}
	if (freq < 0.01) {
		fprintf(stderr, "Frequency too low.\n");
		goto tidy;
	}
	if (delay_shift < DELAY_SHIFT_MIN || delay_shift > DELAY_SHIFT_MAX) {
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

	if ((opt_flags & (OPT_ZERO | OPT_URANDOM)) ==
		(OPT_ZERO | OPT_URANDOM)) {
		fprintf(stderr, "Cannot use both -z and -R options together.\n");
		goto tidy;
	}

	if (filename) {
		(void)umask(0077);
		int open_flags = (opt_flags & OPT_APPEND) ? O_APPEND : O_TRUNC;

		fdtee = open(filename, O_CREAT | open_flags | O_WRONLY, S_IRUSR | S_IWUSR);
		if (fdtee < 0) {
			fprintf(stderr, "open on %s failed: errno = %d (%s).\n",
				filename, errno, strerror(errno));
			goto tidy;
		}
	}
	if (opt_flags & OPT_URANDOM) {
		fdin = open(dev_urandom, O_RDONLY);
		if (fdin < 0) {
			fprintf(stderr, "Cannot open %s: errno=%d (%s).\n",
				dev_urandom, errno, strerror(errno));
			goto tidy;
		}
	} else {
		fdin = fileno(stdin);
	}
	fdout = fileno(stdout);

	if ((secs_start = timeval_to_double()) < 0.0)
		goto tidy;

	if (opt_flags & OPT_GOT_CONST_DELAY)
		delay = 1000000 * const_delay;
	else
		delay = (int)(((double)io_size * 1000000) / (double)data_rate);
	secs_last = secs_start;
	stats.time_begin = secs_start;
	stats.target_rate = data_rate;

	new_action.sa_handler = handle_sigint;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	if (sigaction(SIGINT, &new_action, NULL) < 0) {
		fprintf(stderr, "Sigaction failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto tidy;
	}

	while (!eof) {
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
					if ((errno == EINTR) && sluice_finish)
						goto finish;
					fprintf(stderr,"Read error: errno=%d (%s).\n",
						errno, strerror(errno));
					goto tidy;
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
			if (write(fdtee, buffer, (size_t)inbufsize) < 0) {
				if ((errno == EINTR) && sluice_finish)
					goto finish;
				fprintf(stderr, "Write error: errno=%d (%s).\n",
					errno, strerror(errno));
				goto tidy;
			}
		}
		if (eof || (max_trans && total_bytes >= max_trans))
			break;

		if (delay > 0) {
			if (usleep(delay) < 0) {
				if ((errno == EINTR) && sluice_finish)
					goto finish;
				fprintf(stderr, "usleep error: errno=%d (%s).\n",
					errno, strerror(errno));
				goto tidy;
			}
		}

		if ((secs_now = timeval_to_double()) < 0.0) {
			if ((errno == EINTR) && sluice_finish)
				goto finish;
			goto tidy;
		}
		current_rate = (uint64_t)(((double)total_bytes) / (secs_now - secs_start));

		if (current_rate > (double)data_rate) {
			run = '+' ;
			if (!(opt_flags & OPT_GOT_CONST_DELAY))
				delay += ((last_delay >> delay_shift) + 100);
			warnings = 0;
			underruns = 0;
			overruns++;
			stats.overruns++;
		} else if (current_rate < (double)data_rate) {
			run = '-' ;
			if (!(opt_flags & OPT_GOT_CONST_DELAY))
				delay -= ((last_delay >> delay_shift) + 100);
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
			uint64_t tmp_io_size = io_size + (io_size >> 2);

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
			uint64_t tmp_io_size = io_size - (io_size >> 2);

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

		last_delay = delay;

		/* Output feedback in verbose mode ~3 times a second */
		if ((opt_flags & OPT_VERBOSE) &&
		    (secs_now > secs_last + freq)) {
			char current_rate_str[32];
			char total_bytes_str[32];
			char io_size_str[32];

			size_to_str(current_rate, current_rate_str,
				sizeof(current_rate_str));
			size_to_str(total_bytes, total_bytes_str,
				sizeof(total_bytes_str));
			size_to_str(io_size, io_size_str,
				sizeof(io_size_str));

			fprintf(stderr,"Rate: %s/S, Adj: %c, "
				"Total: %s, Dur: %.1f S, Buf: %s  \r",
				current_rate_str, run, total_bytes_str,
				secs_now - secs_start, io_size_str);
			(void)fflush(stderr);
			secs_last = secs_now;
		}
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
