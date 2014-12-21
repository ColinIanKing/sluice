/*
 * Copyright (C) 2014 Canonical
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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#define APP_NAME		"sluice"
#define UNDERFLOW_MAX		(100)

#define OPT_VERBOSE		(0x00000001)
#define OPT_GOT_RATE		(0x00000002)
#define OPT_GOT_IOSIZE		(0x00000004)
#define OPT_WARNING		(0x00000008)

static int opt_flags;

typedef struct {
	const char ch;		/* Scaling suffix */
	const uint64_t  scale;	/* Amount to scale by */
} scale_t;

/*
 *  timeval_to_double()
 *	convert timeval to seconds as a double
 */
static inline double timeval_to_double(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) < 0) {
		fprintf(stderr, "gettimeofday error: errno=%d (%s).\n",
			errno, strerror(errno));
		exit(EXIT_FAILURE);
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

	if (val < 10 * 1024ULL) {
		s = (double)val;
		units = "B";
	} else if (val < 10 * 1024ULL * 1024ULL) {
		s = (double)val / 1024ULL;
		units = "KB";
	} else if (val < 10 * 1024ULL * 1024ULL * 1024ULL) {
		s = (double)val / (1024ULL * 1024ULL);
		units = "MB";
	} else {
		s = (double)val / (1024ULL * 1024ULL * 1024ULL);
		units = "GB";
	}
	snprintf(buf, buflen, "%7.1f %s", s, units);
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
	uint64_t val;
	size_t len = strlen(str);
	int i;
	char ch;

	errno = 0;
	val = (uint64_t)strtoull(str, NULL, 10);
	if (errno) {
		fprintf(stderr, "Invalid value %s.\n", str);
		exit(EXIT_FAILURE);
	}
	if (len == 0) {
		fprintf(stderr, "Value %s is an invalid size.\n", str);
		exit(EXIT_FAILURE);
	}
	len--;
	ch = str[len];

	if (isdigit(ch))
		return val;

	ch = tolower(ch);
	for (i = 0; scales[i].ch; i++) {
		if (ch == scales[i].ch)
			return val * scales[i].scale;
	}

	printf("Illegal %s specifier %c\n", msg, str[len]);
	exit(EXIT_FAILURE);
}

/*
 *  get_uint64_byte()
 *	size in bytes, K bytes, M bytes or G bytes
 */
static uint64_t get_uint64_byte(const char *const str)
{
	static const scale_t scales[] = {
		{ 'b', 	1 },
		{ 'k',  1 << 10 },
		{ 'm',  1 << 20 },
		{ 'g',  1 << 30 },
		{ 0,    0 },
	};

	return get_uint64_scale(str, scales, "length");
}

/*
 *  show_usage()
 *	show options
 */
void show_usage(void)
{
	printf("%s, version %s\n\n", APP_NAME, VERSION);
	printf("Usage: %s [options]\n", APP_NAME);
	printf("  -h        print this help.\n");
	printf("  -i size   set io read/write size in bytes.\n");
	printf("  -m size   set maximum amount to process.\n");
	printf("  -r rate   set rate (in bytes per second).\n");
	printf("  -v        set verbose mode (to stderr).\n");
	printf("  -w        warn on data rate underflow.\n");
}

int main(int argc, char **argv)
{
	char run = ' ';			/* Overrun/underrun flag */
	char *buffer;			/* Temp I/O buffer */
	int64_t delay, last_delay = 0;	/* Delays in 1/1000000 of a second */
	uint64_t io_size = 0,
		data_rate = 0,
		total_bytes = 0,
		max_trans = 0;
	int fdin, fdout;
	int warnings = 0;
	double secs_start, secs_last;

	for (;;) {
		int c = getopt(argc, argv, "r:h?i:vm:w");
		if (c == -1)
			break;
		switch (c) {
		case '?':
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
		case 'r':
			data_rate = get_uint64_byte(optarg);
			opt_flags |= OPT_GOT_RATE;
			break;
		case 'v':
			opt_flags |= OPT_VERBOSE;
			break;
		case 'w':
			opt_flags |= OPT_WARNING;
			break;
		default:
			show_usage();
			exit(EXIT_FAILURE);
		}
	}

	if (!(opt_flags & OPT_GOT_RATE)) {
		fprintf(stderr, "Must specify data rate with -r option.\n");
		exit(EXIT_FAILURE);
	}
	if (data_rate < 1) {
		fprintf(stderr, "Rate value %" PRIu64 " too low.\n", data_rate);
		exit(EXIT_FAILURE);
	}

	/*
	 *  No size specified, then default to rate / 32
	 */
	if (!(opt_flags & OPT_GOT_IOSIZE)) {
		io_size = data_rate / 32;
		/* Make sure we don't have small sized I/O */
		if (io_size < 1024)
			io_size = 1024;
	}

	if ((io_size < 1) || (io_size > (4 * 1024 * 1024))) {
		fprintf(stderr, "I/O buffer size %" PRIu64 " out of range.\n",
			io_size);
		exit(EXIT_FAILURE);
	}
	if ((buffer = malloc(io_size)) == NULL) {
		fprintf(stderr,"Cannot allocate buffer of %" PRIu64 " bytes.\n",
			io_size);
		exit(EXIT_FAILURE);
	}

	fdin = fileno(stdin);
	fdout = fileno(stdout);

	secs_start = timeval_to_double();
	delay = (int)(((double)io_size * 1000000) / (double)data_rate);
	secs_last = secs_start;

	for (;;) {
		unsigned long long int current_rate, inbufsize = 0;
		bool complete = false;
		double secs_now;

		while (!complete && (inbufsize < io_size)) {
			uint64_t sz = io_size - inbufsize;
			/* We hit the user specified max limit to transfer */
			if (max_trans && (total_bytes + sz) > max_trans) {
				sz = max_trans - total_bytes;
				complete = true;
			}

			ssize_t n = read(fdin, buffer, (ssize_t)sz);
			if (n < 0) {
				fprintf(stderr,"Read error: errno=%d (%s).\n",
					errno, strerror(errno));
				exit(EXIT_FAILURE);
			}
			inbufsize += n;
			total_bytes += n;
		}
		if (write(fdout, buffer, (size_t)inbufsize) < 0) {
			fprintf(stderr,"Write error: errno=%d (%s).\n",
				errno, strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (max_trans && total_bytes >= max_trans)
			break;

		if (delay > 0)
			usleep(delay);

		secs_now = timeval_to_double();
		current_rate = (uint64_t)(((double)total_bytes) / (secs_now - secs_start));

		if (current_rate > (double)data_rate) {
			run = '+' ;
			delay += (last_delay >> 3) + 100;
			warnings = 0;
		} else {
			run = '-' ;
			delay -= (last_delay >> 3) - 100;
			warnings++;
		}
		if (delay < 0)
			delay = 0;

		/* Too many continuous underflows? */
		if ((opt_flags & OPT_WARNING) && (warnings > UNDERFLOW_MAX)) {
			fprintf(stderr, "Warning: data underflow, "
				"use larger I/O size (-i option)\n");
			opt_flags &= ~OPT_WARNING;
		}

		last_delay = delay;

		/* Output feedback in verbose mode ~3 times a second */
		if ((opt_flags & OPT_VERBOSE) &&
		    (secs_now > secs_last + 0.333)) {
			char current_rate_str[32];
			char total_bytes_str[32];

			size_to_str(current_rate, current_rate_str,
				sizeof(current_rate_str));
			size_to_str(total_bytes, total_bytes_str,
				sizeof(total_bytes_str));

			fprintf(stderr,"Rate: %s/sec, Adjust: %c, "
				"Total: %s, Duration: %.1f secs  \r",
				current_rate_str, run, total_bytes_str,
				secs_now - secs_start);
			fflush(stderr);
			secs_last = secs_now;
		}
	}
	free(buffer);
	exit(EXIT_SUCCESS);
}
