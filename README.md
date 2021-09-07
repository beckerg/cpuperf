# cpuperf
CPU Performance Measurement Tool

**cpuperf** is a simple tool to evaluate the relative performance of various commonly
called functions and/or small chunks of code across a user-specified set of logical CPUs.

It is known to build and run on Linux and FreeBSD, and with a little work should be able
to run on other *BSDs.  Best results will be obtained by running it on an idle machine
and either running it as root or running it under nice to give it maximum scheduling
priority.

## Build
Simply type ``` gmake ``` to build the tool:

```
$ gmake
cc -std=c11 -Wall -Wextra -O2 -march=native -g -I. -I../src -DPROG_VERSION=\"1.0.0-2db85f7d-dirty\" -DNDEBUG  -c -o cpuperf.o cpuperf.c
cc -std=c11 -Wall -Wextra -O2 -march=native -g -I. -I../src -DPROG_VERSION=\"1.0.0-2db85f7d-dirty\" -DNDEBUG  -c -o clp.o clp.c
cc -std=c11 -Wall -Wextra -O2 -march=native -g -I. -I../src -DPROG_VERSION=\"1.0.0-2db85f7d-dirty\" -DNDEBUG  -c -o subr.o subr.c
cc -std=c11 -Wall -Wextra -O2 -march=native -g -I. -I../src -DPROG_VERSION=\"1.0.0-2db85f7d-dirty\" -DNDEBUG  -c -o lfstack.o lfstack.c
cc -std=c11 -Wall -Wextra -O2 -march=native -g -I. -I../src -DPROG_VERSION=\"1.0.0-2db85f7d-dirty\" -DNDEBUG  -c -o xoroshiro.o xoroshiro.c
cc   cpuperf.o clp.o subr.o lfstack.o xoroshiro.o  -lpthread -o cpuperf
```

## Theory of Operation
Given a list of logical CPUs, **cpuperf** will start a thread for each CPU and affine it
to that CPU.  For each test in a given set of tests, it will then run two subtests.  The
first subtest will call the test function repeatedly, attempting to serialize each call
and measure and record the minimum latency.  These results are shown under the **sermin**
columns in the output.  The second subtest will attempt to call the test function
repeatedly without any sort of serialization, such that the average latency can be
computed by dividing the number of calls by the elapsed test time.  These results
are shown under the **avg** columns in the output.


## Examples
### Example 1 - mutex

So, how much more expensive is it to acquire and release an uncontended vs contended mutex?
**cpuperf** contains a test named **lock-mutex-pthread** which executes a minimally sized
critical section guarded by a pthread mutex (i.e., lock + increment an integer + unlock).
To find out, we run **cpuperf** on vCPUs 2 and 4 and tell it to run only the mutex based tests.
For this test I happen to know that vCPUs 2 and 4 are on different cores of my i7-5820K.


```
$ sudo ./cpuperf -t lock-mutex  2 4

      MHz       tot       avg     avg     avg    sermin  sermin
  -   TSC    MCALLS  MCALLS/s  CYCLES   NSECS    CYCLES   NSECS  NAME                DESC
  -  3305  15789.52    716.99     4.6    1.39      24.0    7.26  baseline            baseline
  -  3305    737.08     44.24    74.7   22.60      87.0   26.32  lock-mutex-pthread  lock+inc+unlock
```

We see that **cpuperf** first performs a baseline measurement, which is simply a call to
a function that accepts a single pointer and returns zero.  According to the **sermin**
columns it takes 24 cycles to call this function and wait for completion.  However, it
appears that if we do not serialize the call nor wait for completion we can call this
function an average of 716.99 million times per second per vCPU (roughly five times
more often, likely due to the processor's ability to schedule overlapping calls).
Similary, we see that the cost of calling lock+inc+unlock on an uncontended mutex
is about 87 cycles, and we can do that about 44.24 million times per second.
It's not obvious, but the **sermin** result of the "lock-mutex-pthread" test have
had the baseline measurement subtracted from it.  More on this later...

Next, we run **cpuperf** again with the _**-s**_ option which tells _**cpuperf**_ to use
shared locks amongst all test threads:

```
$ sudo ./cpuperf -t lock-mutex -s  2 4

      MHz       tot       avg     avg     avg    sermin  sermin
  -   TSC    MCALLS  MCALLS/s  CYCLES   NSECS    CYCLES   NSECS  NAME                DESC
  -  3305  15870.76    716.81     4.6    1.40      24.0    7.26  baseline            baseline
  -  3305    159.23      1.82  1813.6  548.62      87.0   26.32  lock-mutex-pthread  lock+inc+unlock

```

Here again we see that **sermin** results for the shared "lock-mutex-pthread" test are the
same as for the non-shared tests, indicating that the overhead of the serialization diminishes
the contention on the lock such that it's not a factor.
That said, the results for the **avg** MCALLS/s show that extreme contention by only two
threads on two different cores reduces the throughput to .041 of the uncontended lock.

Now let's run the same contended test on two threads running on the same core:

```
$ sudo ./cpuperf -tlock-mutex -s 2 3

      MHz      tot       avg     avg     avg    sermin  sermin
  -   TSC   MCALLS  MCALLS/s  CYCLES   NSECS    CYCLES   NSECS  NAME                DESC
  -  3305  6480.16    294.47    11.2    3.40      24.0    7.26  baseline            baseline
  -  3305   101.99      4.29   771.1  233.25     114.0   34.48  lock-mutex-pthread  lock+inc+unlock

```

More to come...
