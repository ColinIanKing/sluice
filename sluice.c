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
#define KB			(1024ULL)
#define MB			(KB * KB)
#define GB			(KB * KB * KB)
#define UNDERFLOW_MAX		(100)
#define UNDERFLOW_ADJUST_MAX	(10)
#define OVERFLOW_ADJUST_MAX	(10)
#define IO_SIZE_MAX		(4 * MB)
#define IO_SIZE_MIN		(1)

#define OPT_VERBOSE		(0x00000001)
#define OPT_GOT_RATE		(0x00000002)
#define OPT_GOT_IOSIZE		(0x00000004)
#define OPT_WARNING		(0x00000008)
#define OPT_UNDERFLOW		(0x00000010)
#define OPT_DISCARD		(0x00000020)
#define OPT_OVERFLOW		(0x00000040)

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
	printf("  -d        discard input (no output).\n");
	printf("  -h        print this help.\n");
	printf("  -i size   set io read/write size in bytes.\n");
	printf("  -m size   set maximum amount to process.\n");
	printf("  -o        shrink read/write buffer to avoid overflow.\n");
	printf("  -r rate   set rate (in bytes per second).\n");
	printf("  -t file   tee output to file.\n");
	printf("  -u        expand read/write buffer to avoid underflow.\n");
	printf("  -v        set verbose mode (to stderr).\n");
	printf("  -w        warn on data rate underflow.\n");
}

int main(int argc, char **argv)
{
	char run = ' ';			/* Overrun/underrun flag */
	char *buffer = NULL;		/* Temp I/O buffer */
	char *filename = NULL;		/* -t option filename */
	int64_t delay, last_delay = 0;	/* Delays in 1/1000000 of a second */
	uint64_t io_size = 0,
		data_rate = 0,
		total_bytes = 0,
		max_trans = 0;
	int fdin, fdout, fdtee = -1;
	int warnings = 0;
	int underflows = 0, overflows = 0;
	int ret = EXIT_FAILURE;
	double secs_start, secs_last;

	for (;;) {
		int c = getopt(argc, argv, "r:h?i:vm:wudot:");
		if (c == -1)
			break;
		switch (c) {
		case 'd':
			opt_flags |= OPT_DISCARD;
			break;
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
		case 'o':
			opt_flags |= OPT_OVERFLOW;
			break;
		case 'r':
			data_rate = get_uint64_byte(optarg);
			opt_flags |= OPT_GOT_RATE;
			break;
		case 't':
			filename = optarg;
			break;
		case 'u':
			opt_flags |= OPT_UNDERFLOW;
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
		goto tidy;
	}
	if (data_rate < 1) {
		fprintf(stderr, "Rate value %" PRIu64 " too low.\n", data_rate);
		goto tidy;
	}

	if (filename) {
		(void)umask(0077);

		fdtee = open(filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
		if (fdtee < 0) {
			fprintf(stderr, "open on %s failed: errno = %d (%s).\n",
				filename, errno, strerror(errno));
			goto tidy;
		}
	}

	/*
	 *  No size specified, then default to rate / 32
	 */
	if (!(opt_flags & OPT_GOT_IOSIZE)) {
		io_size = data_rate / 32;
		/* Make sure we don't have small sized I/O */
		if (io_size < KB)
			io_size = KB;
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

	fdin = fileno(stdin);
	fdout = fileno(stdout);

	if ((secs_start = timeval_to_double()) < 0)
		goto tidy;
	delay = (int)(((double)io_size * 1000000) / (double)data_rate);
	secs_last = secs_start;

	for (;;) {
		uint64_t current_rate, inbufsize = 0;
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
				goto tidy;
			}
			inbufsize += n;
			total_bytes += n;
		}
		if (!(opt_flags & OPT_DISCARD)) {
			if (write(fdout, buffer, (size_t)inbufsize) < 0) {
				fprintf(stderr,"Write error: errno=%d (%s).\n",
					errno, strerror(errno));
				goto tidy;
			}
		}
		if (fdtee >= 0) {
			if (write(fdtee, buffer, (size_t)inbufsize) < 0) {
				fprintf(stderr,"Write error: errno=%d (%s).\n",
					errno, strerror(errno));
				goto tidy;
			}
		}
		if (max_trans && total_bytes >= max_trans)
			break;

		if (delay > 0) {
			if (usleep(delay) < 0) {
				fprintf(stderr, "usleep error: errno=%d (%s).\n",
					errno, strerror(errno));
				goto tidy;
			}
		}

		if ((secs_now = timeval_to_double()) < 0)
			goto tidy;
		current_rate = (uint64_t)(((double)total_bytes) / (secs_now - secs_start));

		if (current_rate > (double)data_rate) {
			run = '+' ;
			delay += (last_delay >> 3) + 100;
			warnings = 0;
			underflows = 0;
			overflows++;
		} else if (current_rate < (double)data_rate) {
			run = '-' ;
			delay -= (last_delay >> 3) - 100;
			warnings++;
			underflows++;
			overflows = 0;
		} else {
			/* Unlikely.. */
			warnings = 0;
			underflows = 0;
			overflows = 0;
			run = '0';
		}
		if (delay < 0)
			delay = 0;

		if ((opt_flags & OPT_UNDERFLOW) &&
		    (underflows > UNDERFLOW_ADJUST_MAX)) {
			char *tmp;
			uint64_t tmp_io_size = io_size + (io_size >> 2);

			if (tmp_io_size < IO_SIZE_MAX) {
				tmp = realloc(buffer, tmp_io_size);
				if (tmp) {
					buffer = tmp;
					io_size = tmp_io_size;
				}
			}
			underflows = 0;
		}

		if ((opt_flags & OPT_OVERFLOW) &&
		    (overflows > OVERFLOW_ADJUST_MAX)) {
			char *tmp;
			uint64_t tmp_io_size = io_size - (io_size >> 2);

			if (tmp_io_size > IO_SIZE_MIN) {
				tmp = realloc(buffer, tmp_io_size);
				if (tmp) {
					buffer = tmp;
					io_size = tmp_io_size;
				}
			}
			overflows = 0;
		}

		/* Too many continuous underflows? */
		if ((opt_flags & OPT_WARNING) &&
		    (warnings > UNDERFLOW_MAX)) {
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
			fflush(stderr);
			secs_last = secs_now;
		}
	}
	ret = EXIT_SUCCESS;
tidy:
	free(buffer);
	if (fdtee >= 0)
		(void)close(fdtee);
	exit(ret);
}
