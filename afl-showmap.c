/*
   american fuzzy lop - run program, display map
   ---------------------------------------------

   Written and maintained by Michal Zalewski <lcamtuf@google.com>

   Copyright 2013, 2014 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   A very simple tool that runs the targeted binary and displays
   the contents of the trace bitmap in a human-readable form. Useful in
   scripts to eliminate redundant inputs and perform other checks.

   If AFL_SINK_OUTPUT is set, output from the traced program will be
   redirected to /dev/null. AFL_QUIET inhibits all non-fatal messages,
   too.

 */

#define AFL_MAIN

#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>

#include <sys/fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>

static s32 forksrv_pid,               /* PID of the fork server            */
           child_pid;                 /* PID of the fuzzed program         */

static u8* trace_bits;                /* SHM with instrumentation bitmap   */

static s32 shm_id;                    /* ID of the SHM region              */

static u8  sink_output,               /* Sink program output               */
           be_quiet;                  /* Quiet mode (tuples & errors only) */

/* Classify tuple counts. */

static void classify_counts(u8* mem) {

  u32 i = MAP_SIZE;

  while (i--) {

    switch (*mem) {
      case 3:           *mem = (1 << 2); break;
      case 4 ... 7:     *mem = (1 << 3); break;
      case 8 ... 15:    *mem = (1 << 4); break;
      case 16 ... 31:   *mem = (1 << 5); break;
      case 32 ... 127:  *mem = (1 << 6); break;
      case 128 ... 255: *mem = (1 << 7); break;
    }

    mem++;

  }

}

/* Show all recorded tuples. */

static inline void show_tuples(void) {

  u8* current = (u8*)trace_bits;
  u32 i;

  classify_counts(trace_bits);

  for (i = 0; i < MAP_SIZE; i++) {

    if (*current) SAYF("%05u/%u\n", i, *current);

    current++;

  }

}


/* Count the number of bits set in the bitmap. */

static inline u32 count_bits(void) {

  u32* ptr = (u32*)trace_bits;
  u32  i   = (MAP_SIZE >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    v -= ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    ret += (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;

  }

  return ret;

}



/* Get rid of shared memory (atexit handler). */

static void remove_shm(void) {
  shmctl(shm_id, IPC_RMID, NULL);
}


/* Configure shared memory. */

static void setup_shm(void) {

  u8* shm_str;

  shm_id = shmget(IPC_PRIVATE, MAP_SIZE, IPC_CREAT | IPC_EXCL | 0600);

  if (shm_id < 0) PFATAL("shmget() failed");

  atexit(remove_shm);

  shm_str = alloc_printf("%d", shm_id);

  setenv(SHM_ENV_VAR, shm_str, 1);

  ck_free(shm_str);

  trace_bits = shmat(shm_id, NULL, 0);
  
  if (!trace_bits) PFATAL("shmat() failed");

}


/* Execute target application. */

static void run_target(char** argv) {

  int status = 0;
  int st_pipe[2], ctl_pipe[2];

  if (pipe(st_pipe) || pipe(ctl_pipe)) PFATAL("pipe() failed");

  forksrv_pid = fork();

  if (forksrv_pid < 0) PFATAL("fork() failed");

  if (!forksrv_pid) {

    struct rlimit r;

    if (!getrlimit(RLIMIT_NOFILE, &r) && r.rlim_cur < FORKSRV_FD + 2) {

      r.rlim_cur = FORKSRV_FD + 2;
      setrlimit(RLIMIT_NOFILE, &r); /* Ignore errors */

    }

    if (sink_output) {

      s32 fd = open("/dev/null", O_RDWR);

      if (fd < 0) PFATAL("Cannot open /dev/null");

      if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0) PFATAL("dup2() failed");

      close(fd);

    }

    /* Set up control and status pipes, close the original fds. */

    if (dup2(ctl_pipe[0], FORKSRV_FD) < 0) PFATAL("dup2() failed");
    if (dup2(st_pipe[1], FORKSRV_FD + 1) < 0) PFATAL("dup2() failed");

    close(ctl_pipe[0]);
    close(ctl_pipe[1]);
    close(st_pipe[0]);
    close(st_pipe[1]);

    execvp(argv[0], argv);

    PFATAL("Unable to execute '%s'", argv[0]);

  }

  /* Close the unneeded endpoints, wake up workserver. */

  close(ctl_pipe[0]);
  close(st_pipe[1]);

  if (write(ctl_pipe[1], &status, 4) != 4) 
    FATAL("No instrumentation detected or fork server fault");

  /* The fork server will send us a "hi mom" message first, then the PID,
     then the actual exec status once the child process exits. */

  if (read(st_pipe[0], &status, 4) != 4 ||
      read(st_pipe[0], &child_pid, 4) != 4 || child_pid <= 0 ||
      read(st_pipe[0], &status, 4) != 4) {

    FATAL("No instrumentation detected or fork server fault");

  }

  if (WIFSIGNALED(status))
    SAYF("+++ Killed by signal %u +++\n", WTERMSIG(status));

  /* Fork server will die when we exit because of a failed read() on a
     broken pipe, so no special need to kill it beforehand. */

}



/* Display usage hints. */

static void usage(u8* argv0) {

  SAYF("\n%s /path/to/traced_app [ ... ]\n\n"

       "Shows all instrumentation tuples recorded when executing a binary compiled\n"
       "with afl-gcc or afl-clang. You can set AFL_SINK_OUTPUT=1 to sink all output\n"
       "from the executed program, or AFL_QUIET=1 to suppress non-fatal messages\n"
       "from this tool.\n\n", argv0);

  exit(1);

}


/* Main entry point */

int main(int argc, char** argv) {

  if (!getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-showmap " cBRI VERSION cRST " (" __DATE__ " " __TIME__ 
         ") by <lcamtuf@google.com>\n");

  } else be_quiet = 1;

  if (argc < 2) usage(argv[0]);

  setup_shm();

  if (getenv("AFL_SINK_OUTPUT")) sink_output = 1;

  if (!be_quiet && !sink_output)
    SAYF("\n-- Program output begins --\n");  

  run_target(argv + 1);

  if (!be_quiet && !sink_output)
    SAYF("-- Program output ends --\n");  

  if (!count_bits()) FATAL("No instrumentation data recorded");

  if (!be_quiet) SAYF(cBRI "\nTuples recorded:\n\n" cRST);

  show_tuples();

  exit(0);

}

