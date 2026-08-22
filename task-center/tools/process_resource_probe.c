#include <errno.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  int valid;
  struct rusage_info_v6 usage;
  struct timespec sampled_at;
} Sample;

static double seconds_between(struct timespec newer, struct timespec older) {
  return (double)(newer.tv_sec - older.tv_sec) +
         (double)(newer.tv_nsec - older.tv_nsec) / 1000000000.0;
}

static double mib(uint64_t bytes) {
  return (double)bytes / (1024.0 * 1024.0);
}

static int read_sample(pid_t pid, Sample *sample) {
  memset(sample, 0, sizeof(*sample));
  if (clock_gettime(CLOCK_MONOTONIC, &sample->sampled_at) != 0) {
    return -1;
  }
  if (proc_pid_rusage(pid, RUSAGE_INFO_V6, (rusage_info_t *)&sample->usage) != 0) {
    return -1;
  }
  sample->valid = 1;
  return 0;
}

static void print_timestamp(void) {
  struct timespec now;
  struct tm local;
  char buffer[32];
  clock_gettime(CLOCK_REALTIME, &now);
  localtime_r(&now.tv_sec, &local);
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
  printf("%s.%03ld", buffer, now.tv_nsec / 1000000);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s INTERVAL_SECONDS SAMPLE_COUNT PID [PID ...]\n", argv[0]);
    return 64;
  }

  char *end = NULL;
  double interval = strtod(argv[1], &end);
  if (!end || *end || interval <= 0) {
    fprintf(stderr, "invalid interval: %s\n", argv[1]);
    return 64;
  }
  long sample_count = strtol(argv[2], &end, 10);
  if (!end || *end || sample_count < 2) {
    fprintf(stderr, "sample count must be at least 2: %s\n", argv[2]);
    return 64;
  }

  int pid_count = argc - 3;
  pid_t *pids = calloc((size_t)pid_count, sizeof(*pids));
  Sample *previous = calloc((size_t)pid_count, sizeof(*previous));
  if (!pids || !previous) {
    perror("calloc");
    return 70;
  }
  for (int index = 0; index < pid_count; index++) {
    long value = strtol(argv[index + 3], &end, 10);
    if (!end || *end || value <= 0) {
      fprintf(stderr, "invalid pid: %s\n", argv[index + 3]);
      return 64;
    }
    pids[index] = (pid_t)value;
  }

  printf("timestamp,pid,status,cpu_percent,resident_mib,footprint_mib,idle_wakeups_per_s,interrupt_wakeups_per_s,energy_mw\n");
  fflush(stdout);

  struct timespec sleep_for = {
      .tv_sec = (time_t)interval,
      .tv_nsec = (long)((interval - (double)(time_t)interval) * 1000000000.0),
  };

  for (long iteration = 0; iteration < sample_count; iteration++) {
    for (int index = 0; index < pid_count; index++) {
      Sample current;
      print_timestamp();
      printf(",%d,", pids[index]);
      if (read_sample(pids[index], &current) != 0) {
        printf("unavailable,,,,,,\n");
        previous[index].valid = 0;
        continue;
      }

      if (!previous[index].valid) {
        printf("warmup,,%.3f,%.3f,,,\n",
               mib(current.usage.ri_resident_size),
               mib(current.usage.ri_phys_footprint));
      } else {
        double elapsed = seconds_between(current.sampled_at, previous[index].sampled_at);
        uint64_t cpu_delta =
            (current.usage.ri_user_time - previous[index].usage.ri_user_time) +
            (current.usage.ri_system_time - previous[index].usage.ri_system_time);
        uint64_t idle_delta = current.usage.ri_pkg_idle_wkups -
                              previous[index].usage.ri_pkg_idle_wkups;
        uint64_t interrupt_delta = current.usage.ri_interrupt_wkups -
                                   previous[index].usage.ri_interrupt_wkups;
        uint64_t energy_delta = current.usage.ri_energy_nj -
                                previous[index].usage.ri_energy_nj;
        double cpu_percent = elapsed > 0 ? ((double)cpu_delta / 1000000000.0) / elapsed * 100.0 : 0;
        double energy_mw = elapsed > 0 ? ((double)energy_delta / 1000000.0) / elapsed : 0;
        printf("ok,%.4f,%.3f,%.3f,%.4f,%.4f,%.6f\n",
               cpu_percent,
               mib(current.usage.ri_resident_size),
               mib(current.usage.ri_phys_footprint),
               elapsed > 0 ? (double)idle_delta / elapsed : 0,
               elapsed > 0 ? (double)interrupt_delta / elapsed : 0,
               energy_mw);
      }
      previous[index] = current;
    }
    fflush(stdout);
    if (iteration + 1 < sample_count) {
      while (nanosleep(&sleep_for, &sleep_for) != 0 && errno == EINTR) {
      }
      sleep_for.tv_sec = (time_t)interval;
      sleep_for.tv_nsec = (long)((interval - (double)(time_t)interval) * 1000000000.0);
    }
  }

  free(previous);
  free(pids);
  return 0;
}
