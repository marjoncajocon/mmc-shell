/*
** mjobs.c - background jobs, and the builtins about processes:
** jobs wait kill fg bg disown trap times umask ulimit suspend
**
** Job control: on a POSIX terminal an interactive shell gives every job
** a process group; Ctrl-Z stops the one in front, fg and bg let it go on.
** Windows has no such thing for the keyboard (Ctrl-Z goes to the program
** as a key), but kill -STOP / -CONT stop and resume a process there too.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static Job **jobs = NULL;
static int njobs = 0;
static int next_id = 1;
static int cur_job = 0, prev_job = 0;	/* %+ and %- (job ids) */

/* statuses of finished jobs, for "wait PID" after they were collected */
typedef struct Done {
  long pid;
  int status;
} Done;
static Done done_list[256];
static int ndone = 0;


static void remember_done (long pid, int status) {
  done_list[ndone % 256].pid = pid;
  done_list[ndone % 256].status = status;
  ndone++;
}


int job_status_of_pid (long pid, int *status) {
  int i, n = ndone < 256 ? ndone : 256;
  for (i = 0; i < n; i++)
    if (done_list[i].pid == pid) {
      *status = done_list[i].status;
      return 1;
    }
  return 0;
}


static int norm_status (int st) {
  if ((unsigned)st == 0xC000013Au) return 130;
  if (st < 0 || st > 255) return (st & 0xFF) ? (st & 0xFF) : 1;
  return st;
}


Job *job_add (const char *cmd, OsProc *procs, long *pids, int n) {
  Job *j = (Job *)xmalloc(sizeof(Job));
  int i;
  memset(j, 0, sizeof(*j));
  if (njobs == 0) next_id = 1;
  j->id = next_id++;
  j->procs = (OsProc *)xmalloc((size_t)n * sizeof(OsProc));
  j->pids = (long *)xmalloc((size_t)n * sizeof(long));
  for (i = 0; i < n; i++) {
    j->procs[i] = procs[i];
    j->pids[i] = pids[i];
  }
  j->nprocs = n;
  j->running = n;
  j->pid = pids[n - 1];
  j->pgid = pids[0];
  j->cmd = xstrdup(cmd);
  {	/* "sleep 5 &" is shown without the & */
    size_t len = strlen(j->cmd);
    while (len > 0 && (j->cmd[len - 1] == ' ' || j->cmd[len - 1] == '&')) j->cmd[--len] = '\0';
  }
  jobs = (Job **)xrealloc(jobs, (size_t)(njobs + 1) * sizeof(Job *));
  jobs[njobs++] = j;
  prev_job = cur_job;
  cur_job = j->id;
  return j;
}


void job_remove (Job *j) {
  int i, k;
  for (i = 0; i < njobs; i++) {
    if (jobs[i] != j) continue;
    for (k = 0; k < j->nprocs; k++)
      if (j->procs[k] != 0) os_detach(j->procs[k]);
    free(j->procs);
    free(j->pids);
    free(j->cmd);
    free(j);
    memmove(jobs + i, jobs + i + 1, (size_t)(njobs - i - 1) * sizeof(Job *));
    njobs--;
    if (cur_job == j->id) cur_job = prev_job;
    return;
  }
}


int job_count (void) {
  return njobs;
}


/* shopt -s huponexit: the background jobs go when the shell goes */
void job_hup_all (void) {
  int i, k;
  for (i = 0; i < njobs; i++)
    for (k = 0; k < jobs[i]->nprocs; k++)
      if (jobs[i]->pids[k] > 0) os_kill(jobs[i]->pids[k], 15);
}


Job *job_by_pid (long pid) {
  int i, k;
  for (i = 0; i < njobs; i++)
    for (k = 0; k < jobs[i]->nprocs; k++)
      if (jobs[i]->pids[k] == pid) return jobs[i];
  return NULL;
}


static Job *job_by_id (int id) {
  int i;
  for (i = 0; i < njobs; i++)
    if (jobs[i]->id == id) return jobs[i];
  return NULL;
}


/* %1 %+ %% %- %name %?text, or a pid */
Job *job_find (const char *spec) {
  int i;
  if (spec == NULL || spec[0] == '\0') return job_by_id(cur_job);
  if (spec[0] != '%') {
    long long pid;
    if (str_to_ll(spec, &pid) == 0) return job_by_pid((long)pid);
    return NULL;
  }
  spec++;
  if (*spec == '\0' || strcmp(spec, "+") == 0 || strcmp(spec, "%") == 0)
    return job_by_id(cur_job);
  if (strcmp(spec, "-") == 0) return job_by_id(prev_job);
  if (isdigit((unsigned char)*spec)) return job_by_id(atoi(spec));
  for (i = njobs - 1; i >= 0; i--) {
    if (*spec == '?' ? strstr(jobs[i]->cmd, spec + 1) != NULL
                     : strncmp(jobs[i]->cmd, spec, strlen(spec)) == 0)
      return jobs[i];
  }
  return NULL;
}


static char job_mark (const Job *j) {
  return j->id == cur_job ? '+' : j->id == prev_job ? '-' : ' ';
}


static void make_current (Job *j) {
  if (cur_job == j->id) return;
  prev_job = cur_job;
  cur_job = j->id;
}


/* a job in the foreground was stopped (Ctrl-Z): it is kept, stopped */
Job *job_stopped (const char *cmd, OsProc *procs, long *pids, int n, long pgid) {
  Job *j = job_add(cmd, procs, pids, n);
  j->stopped = 1;
  j->notified = 1;
  if (pgid > 0) j->pgid = pgid;
  fd_printf(2, "\n[%d]+  Stopped                 %s\n", j->id, j->cmd);
  return j;
}


int job_stopped_count (void) {
  int i, n = 0;
  for (i = 0; i < njobs; i++) n += jobs[i]->stopped;
  return n;
}


void job_hup_stopped (void) {
  int i, k;
  for (i = 0; i < njobs; i++) {
    if (!jobs[i]->stopped) continue;
    for (k = 0; k < jobs[i]->nprocs; k++)
      if (jobs[i]->procs[k] != 0) {
        os_kill(jobs[i]->pids[k], 1);
        os_kill(jobs[i]->pids[k], 18);
      }
  }
}


static void job_continue (Job *j) {
  int k;
  for (k = 0; k < j->nprocs; k++)
    if (j->procs[k] != 0) os_kill(j->pids[k], 18);	/* CONT */
  j->stopped = 0;
  j->notified = 0;
}


/* collects what has finished; 'report': print "Done" lines */
void job_poll (int report) {
  int i;
  for (i = 0; i < njobs; i++) {
    Job *j = jobs[i];
    int k;
    for (k = 0; k < j->nprocs; k++) {
      int st, r;
      if (j->procs[k] == 0) continue;
      r = os_poll_proc(j->procs[k], &st);
      if (r == 2) {	/* stopped in the background (it wanted the terminal) */
        if (!j->stopped) {
          j->stopped = 1;
          make_current(j);
          if (report && sh_interactive)
            fd_printf(2, "[%d]%c  Stopped                 %s\n", j->id, job_mark(j), j->cmd);
        }
        continue;
      }
      if (r) {
        st = norm_status(st);
        j->procs[k] = 0;
        j->running--;
        remember_done(j->pids[k], st);
        if (k == j->nprocs - 1) j->status = st;
      }
    }
  }
  for (i = 0; i < njobs; i++) {
    Job *j = jobs[i];
    if (j->running > 0) continue;
    if (report && sh_interactive) {
      if (j->status == 0) fd_printf(2, "[%d]%c  Done                    %s\n", j->id,
                                    j->id == cur_job ? '+' : j->id == prev_job ? '-' : ' ', j->cmd);
      else fd_printf(2, "[%d]%c  Exit %-18d %s\n", j->id,
                     j->id == cur_job ? '+' : j->id == prev_job ? '-' : ' ', j->status, j->cmd);
    }
    if (report || !sh_interactive) {
      job_remove(j);
      i--;
    }
  }
}


int job_wait (Job *j) {
  int k, status = 0;
  for (k = 0; k < j->nprocs; k++) {
    if (j->procs[k] == 0) continue;
    status = norm_status(os_wait(j->procs[k]));
    j->procs[k] = 0;
    j->running--;
    remember_done(j->pids[k], status);
    if (k == j->nprocs - 1) j->status = status;
  }
  status = j->status;
  job_remove(j);
  return status;
}


int job_wait_any (void) {
  for (;;) {
    int i;
    if (njobs == 0) return 127;
    job_poll(0);
    for (i = 0; i < njobs; i++)
      if (jobs[i]->running == 0) {
        int st = jobs[i]->status;
        job_remove(jobs[i]);
        return st;
      }
    if (os_interrupted) return 130;
    {	/* nothing yet: wait a little */
      long long until = os_now_us() + 20000;
      while (os_now_us() < until) os_wait_readable(-1, 20);
    }
  }
}


void job_list (int fd, int mode) {
  int i;
  job_poll(0);
  for (i = 0; i < njobs; i++) {
    Job *j = jobs[i];
    char mark = j->id == cur_job ? '+' : j->id == prev_job ? '-' : ' ';
    const char *state = j->running > 0 ? (j->stopped ? "Stopped" : "Running") :
                        (j->status == 0 ? "Done" : "Exit");
    if (mode == 2) fd_printf(fd, "%ld\n", j->pid);
    else if (mode == 1) fd_printf(fd, "[%d]%c %ld %-22s %s\n", j->id, mark, j->pid, state, j->cmd);
    else fd_printf(fd, "[%d]%c  %-22s %s\n", j->id, mark, state, j->cmd);
  }
}

/* }================================================================== */


/*
** {==================================================================
** Builtins
** ===================================================================
*/

int b_jobs (int argc, char **argv, int in, int out, int err) {
  int i, mode = 0;
  (void)in; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-'; i++) {
    if (strchr(argv[i], 'l')) mode = 1;
    if (strchr(argv[i], 'p')) mode = 2;
  }
  if (i < argc) {
    for (; i < argc; i++) {
      Job *j = job_find(argv[i]);
      if (j == NULL) {
        sh_error("jobs: %s: no such job", argv[i]);
        return 1;
      }
      if (mode == 2) fd_printf(out, "%ld\n", j->pid);
      else fd_printf(out, "[%d]%c  %s  %s\n", j->id, job_mark(j),
                     j->running ? (j->stopped ? "Stopped" : "Running") : "Done", j->cmd);
    }
    return 0;
  }
  job_list(out, mode);
  return 0;
}


int b_wait (int argc, char **argv, int in, int out, int err) {
  int i, status = 0, any = 0;
  (void)in; (void)out; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    if (strcmp(argv[i], "-n") == 0) any = 1;
    else if (strcmp(argv[i], "-f") == 0) continue;
    else if (strcmp(argv[i], "-p") == 0) i++;
    else if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
  }
  if (any) return job_wait_any();
  if (i >= argc) {	/* wait for everything */
    while (job_count() > 0) {
      Job *j = job_find("%+");
      if (j == NULL) break;
      job_wait(j);
    }
    return 0;
  }
  for (; i < argc; i++) {
    Job *j = job_find(argv[i]);
    if (j == NULL) {
      long long pid;
      int st;
      if (argv[i][0] != '%' && str_to_ll(argv[i], &pid) == 0 &&
          job_status_of_pid((long)pid, &st)) {
        status = st;
        continue;
      }
      if (argv[i][0] == '%') sh_error("wait: %s: no such job", argv[i]);
      else sh_error("wait: pid %s is not a child of this shell", argv[i]);
      status = 127;
      continue;
    }
    status = job_wait(j);
  }
  return status;
}


static const struct { const char *name; int num; } sigs[] = {
  {"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"ILL", 4}, {"TRAP", 5}, {"ABRT", 6},
  {"BUS", 7}, {"FPE", 8}, {"KILL", 9}, {"USR1", 10}, {"SEGV", 11}, {"USR2", 12},
  {"PIPE", 13}, {"ALRM", 14}, {"TERM", 15}, {"CHLD", 17}, {"CONT", 18},
  {"STOP", 19}, {"TSTP", 20}, {"TTIN", 21}, {"TTOU", 22}, {"WINCH", 28},
  {NULL, 0}
};


static int sig_number (const char *s) {
  int i;
  if (isdigit((unsigned char)*s)) return atoi(s);
  if (m_strnicmp(s, "SIG", 3) == 0) s += 3;
  for (i = 0; sigs[i].name; i++)
    if (m_stricmp(sigs[i].name, s) == 0) return sigs[i].num;
  return -1;
}


int b_kill (int argc, char **argv, int in, int out, int err) {
  int i = 1, sig = 15, status = 0;
  (void)in; (void)err;
  if (argc < 2) {
    sh_error("kill: usage: kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]");
    return 2;
  }
  if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "-L") == 0) {
    if (argc > 2) {
      int n = atoi(argv[2]);
      if (n > 128) n -= 128;
      for (i = 0; sigs[i].name; i++)
        if (sigs[i].num == n || m_stricmp(sigs[i].name, argv[2]) == 0) {
          if (isdigit((unsigned char)argv[2][0])) fd_printf(out, "%s\n", sigs[i].name);
          else fd_printf(out, "%d\n", sigs[i].num);
          return 0;
        }
      return 1;
    }
    for (i = 0; sigs[i].name; i++) fd_printf(out, "%2d) SIG%s\n", sigs[i].num, sigs[i].name);
    return 0;
  }
  if (strcmp(argv[1], "-s") == 0 || strcmp(argv[1], "-n") == 0) {
    if (argc < 3) return 2;
    sig = sig_number(argv[2]);
    i = 3;
  }
  else if (argv[1][0] == '-' && argv[1][1] != '\0' && !isdigit((unsigned char)argv[1][1])) {
    sig = sig_number(argv[1] + 1);
    i = 2;
  }
  else if (argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
    sig = atoi(argv[1] + 1);
    i = 2;
  }
  if (sig < 0) {
    sh_error("kill: %s: invalid signal specification", argv[i - 1]);
    return 1;
  }
  for (; i < argc; i++) {
    long pid;
    if (argv[i][0] == '%') {
      Job *j = job_find(argv[i]);
      int k;
      if (j == NULL) {
        sh_error("kill: %s: no such job", argv[i]);
        status = 1;
        continue;
      }
      for (k = 0; k < j->nprocs; k++)
        if (j->procs[k] != 0 && os_kill(j->pids[k], sig) != 0) status = 1;
      if (sig >= 19 && sig <= 22) {	/* STOP TSTP TTIN TTOU */
        j->stopped = 1;
        make_current(j);
      }
      else if (sig == 18) j->stopped = 0;	/* CONT */
      continue;
    }
    pid = atol(argv[i]);
    if (pid == os_getpid() || pid == sh_pid) {	/* ourselves: through our traps */
      const char *t = trap_get(sig);
      if (sig == 0) continue;
      if (t != NULL) {
        if (t[0] != '\0') os_pending[sig] = 1;	/* runs before the next command */
        continue;
      }
      if (sig == 17 || sig == 18 || sig == 23 || sig == 28) continue;	/* ignored by default */
      sh_status = 128 + sig;
      sh_exit = 1;
      return sh_status;
    }
    if (pid == 0 && argv[i][0] != '0') {
      sh_error("kill: %s: arguments must be process or job IDs", argv[i]);
      status = 1;
      continue;
    }
    if (os_kill(pid, sig) != 0) {
      sh_error("kill: (%ld) - No such process", pid);
      status = 1;
    }
  }
  return status;
}


/* fg: the job gets the terminal and goes on; we wait, unless it stops again */
int b_fg (int argc, char **argv, int in, int out, int err) {
  Job *j = job_find(argc > 1 ? argv[1] : NULL);
  int k, status = 0, stopped = 0;
  (void)in; (void)err;
  if (j == NULL) {
    sh_error("fg: %s: no such job", argc > 1 ? argv[1] : "current");
    return 1;
  }
  fd_printf(out, "%s\n", j->cmd);
  os_tty_give(j->pgid);
  if (j->stopped) job_continue(j);
  for (k = 0; k < j->nprocs; k++) {
    int st, s = 0;
    if (j->procs[k] == 0) continue;
    st = norm_status(os_wait_fg(j->procs[k], &s));
    if (s) {	/* Ctrl-Z again */
      stopped = 1;
      status = st;
      break;
    }
    j->procs[k] = 0;
    j->running--;
    remember_done(j->pids[k], st);
    if (k == j->nprocs - 1) j->status = st;
  }
  os_tty_give(0);
  os_tty_fix();	/* the program may have left the terminal in any mode */
  if (stopped) {
    j->stopped = 1;
    make_current(j);
    fd_printf(2, "\n[%d]+  Stopped                 %s\n", j->id, j->cmd);
    return status;
  }
  status = j->status;
  job_remove(j);
  return status;
}


int b_bg (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in; (void)err;
  for (i = 1; i < argc || i == 1; i++) {
    Job *j = job_find(i < argc ? argv[i] : NULL);
    if (j == NULL) {
      sh_error("bg: %s: no such job", i < argc ? argv[i] : "current");
      status = 1;
      continue;
    }
    if (!j->stopped) {
      sh_error("bg: job %d already in background", j->id);
      continue;
    }
    job_continue(j);
    fd_printf(out, "[%d]%c %s &\n", j->id, job_mark(j), j->cmd);
    if (i >= argc) break;
  }
  return status;
}


int b_disown (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in; (void)out; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-'; i++)
    if (strchr(argv[i], 'a')) {
      while (job_count() > 0) job_remove(job_find("%+") ? job_find("%+") : job_find("%1"));
      return 0;
    }
  if (i >= argc) {
    Job *j = job_find(NULL);
    if (j) job_remove(j);
    return 0;
  }
  for (; i < argc; i++) {
    Job *j = job_find(argv[i]);
    if (j == NULL) {
      sh_error("disown: %s: no such job", argv[i]);
      status = 1;
    }
    else job_remove(j);
  }
  return status;
}


/* suspend [-f]: stop this shell until its parent lets it go on */
int b_suspend (int argc, char **argv, int in, int out, int err) {
  int force = argc > 1 && strcmp(argv[1], "-f") == 0;
  (void)in; (void)out; (void)err;
  if (opt_get("login_shell") && !force) {
    sh_error("suspend: cannot suspend a login shell");
    return 1;
  }
  if (os_suspend_self() != 0) {
    sh_error("suspend: not possible here (Windows cannot stop a console shell)");
    return 1;
  }
  return 0;
}


int b_trap (int argc, char **argv, int in, int out, int err) {
  int i = 1, status = 0, k;
  (void)in; (void)err;
  if (argc > 1 && strcmp(argv[1], "-l") == 0) {
    for (k = 1; k < 32; k++) fd_printf(out, "%2d) SIG%s%s", k, trap_signame(k), k % 5 ? "\t" : "\n");
    fd_puts(out, "\n");
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "-p") == 0) i = 2;
  if (argc == 1 || i == 2) {	/* list */
    for (k = 0; k < TRAP_MAX; k++) {
      const char *a = trap_get(k);
      int j, want = (i >= argc);
      for (j = i; j < argc && !want; j++) want = trap_signum(argv[j]) == k;
      if (a != NULL && want) {
        char *q = shell_quote(a);
        fd_printf(out, "trap -- %s %s\n", a[0] ? q : "''", trap_signame(k));
        free(q);
      }
    }
    return 0;
  }
  if (strcmp(argv[1], "--") == 0) i = 2;
  if (i < argc && argc - i == 1) {	/* "trap SIG": back to the default */
    return trap_set(argv[i], NULL) ? 1 : 0;
  }
  {
    const char *action = argv[i++];
    int reset = strcmp(action, "-") == 0;
    if (trap_signum(action) >= 0 && is_name(action) == 0 && isdigit((unsigned char)action[0]) &&
        i == argc) {
      /* "trap 2" alone was handled above */
    }
    for (; i < argc; i++)
      if (trap_set(argv[i], reset ? NULL : action) != 0) status = 1;
  }
  return status;
}


static void put_time (int out, double s) {
  int m = (int)(s / 60);
  fd_printf(out, "%dm%.3fs", m, s - 60 * m);
}


int b_times (int argc, char **argv, int in, int out, int err) {
  double t[4];
  (void)argc; (void)argv; (void)in; (void)err;
  os_times(t);
  put_time(out, t[0]);
  fd_puts(out, " ");
  put_time(out, t[1]);
  fd_puts(out, "\n");
  put_time(out, t[2]);
  fd_puts(out, " ");
  put_time(out, t[3]);
  fd_puts(out, "\n");
  return 0;
}


int b_umask (int argc, char **argv, int in, int out, int err) {
  int i = 1, symbolic = 0, print = 0, mask;
  (void)in; (void)err;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (strchr(argv[i], 'S')) symbolic = 1;
    if (strchr(argv[i], 'p')) print = 1;
  }
  if (i >= argc) {
    mask = os_umask(-1);
    if (symbolic) {
      const char *who = "ugo";
      int k;
      for (k = 0; k < 3; k++) {
        int bits = (~mask >> (6 - 3 * k)) & 7;
        fd_printf(out, "%c=%s%s%s%s", who[k], bits & 4 ? "r" : "", bits & 2 ? "w" : "",
                  bits & 1 ? "x" : "", k < 2 ? "," : "\n");
      }
    }
    else fd_printf(out, "%s%04o\n", print ? "umask " : "", mask);
    return 0;
  }
  if (isdigit((unsigned char)argv[i][0])) {
    mask = (int)strtol(argv[i], NULL, 8);
    os_umask(mask & 0777);
    return 0;
  }
  {	/* u=rwx,g=rx,o= */
    int cur = ~os_umask(-1) & 0777;
    const char *p = argv[i];
    while (*p) {
      int who = 0, perm = 0;
      char op;
      while (*p && strchr("ugoa", *p)) {
        if (*p == 'u') who |= 0700;
        else if (*p == 'g') who |= 0070;
        else if (*p == 'o') who |= 0007;
        else who |= 0777;
        p++;
      }
      if (who == 0) who = 0777;
      op = *p ? *p++ : '=';
      while (*p && strchr("rwx", *p)) {
        if (*p == 'r') perm |= 0444;
        else if (*p == 'w') perm |= 0222;
        else perm |= 0111;
        p++;
      }
      perm &= who;
      if (op == '+') cur |= perm;
      else if (op == '-') cur &= ~perm;
      else cur = (cur & ~who) | perm;
      if (*p == ',') p++;
      else if (*p) {
        sh_error("umask: %s: invalid symbolic mode", argv[i]);
        return 1;
      }
    }
    os_umask(~cur & 0777);
  }
  return 0;
}


/*
** mmc keeps no limits of its own: what it reports is what the system
** gives it, and a limit it cannot set it says so about instead of
** quietly accepting it.
*/
int b_ulimit (int argc, char **argv, int in, int out, int err) {
  int i;
  (void)in; (void)err;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-a") == 0) {
      fd_printf(out, "open files                      (-n) %d\n", MMC_FDS);
      fd_puts(out, "everything else                      unlimited (mmc sets no limits)\n");
      return 0;
    }
    if (argv[i][0] == '-' && i + 1 < argc && argv[i + 1][0] != '-') {
      sh_error("ulimit: %s: mmc cannot change limits", argv[i]);
      return 1;
    }
    if (argv[i][0] != '-') {
      sh_error("ulimit: mmc cannot change limits");
      return 1;
    }
    if (strchr(argv[i], 'n') != NULL) {
      fd_printf(out, "%d\n", MMC_FDS);
      return 0;
    }
  }
  fd_puts(out, "unlimited\n");
  return 0;
}

/* }================================================================== */
