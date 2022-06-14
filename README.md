# Sluice

Sluice is a program that reads input on stdin and outputs on stdout at a specified data rate.

# Sluice command line options

* -a append to file (-t, -O options only).
* -c specify the constant delay time between each write.
* -d discard data, do not copy it to stdout. This makes sluice act as a data sink.
* -e skip read errors.
* -f specify the frequency of -v verbose statistics updates.
* -h print help.
* -i specify the read/write size.
* -m specify amount of data to process.
* -n no rate controls, just copy data untouched.
* -o detect overflow and re-size read/write buffer size to stop overflows.
* -p enable verbose mode with progress and ETA statistics.
* -O short cut for -dt; output to a file.
* -r specify the data rate.
* -R ignore stdin, read random data from /dev/urandom.
* -s set delay shift, controls delay adjustment.
* -S display statistics at end of stream to stderr.
* -t tee output to the specified file.
* -T stop after a specified amount of time.
* -u detect underflow and re-size read/write buffer.
* -v write verbose statistics to stderr.
* -V print version information.
* -w warn if a long burst of continuous data rate underflow occur.
* -z ignore stdin, generate zeros. 

Note that suffixes of B, K, M and G specify sizes and rates in bytes, Kbytes, Mbytes and Gbytes respectively.

# Sluice signal handling

killing sluice with the following signals will toggle the following modes:

* SIGUSR1 toggle -v verbose mode
* SIGUSR2 toggle -o -u overrun/underrun modes 

# Example

```
cat /dev/zero | sluice -r 64K -v -m 32M > /dev/null

sluice -R -r 2M -T 1m -S -i 4K > myfifo
Data:            119.99 MB
Reads:           30718
Writes:          30718
Avg. Write Size: 4.00 KB
Duration:        60.000 secs
Target rate:     2.00 MB/sec
Average rate:    2.00 MB/sec
Minimum rate:    1.42 MB/sec
Maximum rate:    2.19 MB/sec
Drift from target rate: (%)
   0.00% -  0.99%:  99.43%
   1.00% -  1.99%:   0.31%
   2.00% -  3.99%:   0.16%
   4.00% -  7.99%:   0.07%
   8.00% - 15.99%:   0.02%
  16.00% - 31.99%:   0.02%
  32.00% - 63.99%:   0.00%
 >64.00%         :   0.00%
Overruns:        47.41%
Underruns:       52.46%
User time:       0.180 secs
System time:     19.100 secs
```

# Why so many tweakables?

Sluice provides different ways to adjust the data rate for specific
use-cases. The -i option allows one to specify the read/write size and 
hence the only the delay between writes can be used to control the data
rate. Using the -o and -u options allows further adjustment of the write
size to help reach the desired rate faster. 

The -s option allows sluice to add more control to the data rate adjustments.
The delay time or buffer sizes are modified by the previous values right
shifted by the -s shift value. The larger the shift value the smaller the
modification, and hence the longer it will take to reach the desired data
rate. The smaller the shift value the quicker it will take sluice to reach
the desired data rate, however, it can cause large overruns or underrun
oscillations which are not desirable. 
