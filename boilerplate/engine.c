#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sched.h>
#include "monitor_ioctl.h"

/* ─── constants ─────────────────────────────────────────────── */
#define MAX_CONTAINERS  16
#define NAME_LEN        64
#define LOG_DIR         "/tmp/jackfruit_logs"
#define SOCK_PATH       "/tmp/jackfruit.sock"
#define BUF_SIZE        4096
#define RING_CAP        64      /* bounded-buffer slots          */

/* ─── container states ───────────────────────────────────────── */
typedef enum {
    ST_EMPTY = 0,
    ST_STARTING,
    ST_RUNNING,
    ST_STOPPED,
    ST_KILLED
} ContainerState;

/* ─── per-container metadata ─────────────────────────────────── */
typedef struct {
    int            slot;
    char           name[NAME_LEN];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_limit_kb;
    long           hard_limit_kb;
    char           log_path[256];
    int            exit_status;
    int            log_pipe[2];   /* [0]=read  [1]=write */
    pthread_t      log_thread;
} Container;

/* ─── bounded ring buffer (log pipeline) ─────────────────────── */
typedef struct {
    char    data[RING_CAP][BUF_SIZE];
    int     len[RING_CAP];
    int     head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int     done;               /* set to 1 to signal consumer exit */
} RingBuf;

static RingBuf ring;

/* ─── globals ────────────────────────────────────────────────── */
static Container   containers[MAX_CONTAINERS];
static pthread_mutex_t meta_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int supervisor_running = 1;
static int monitor_fd = -1;    /* /dev/container_monitor        */

/* ══════════════════════════════════════════════════════════════
   RING BUFFER
══════════════════════════════════════════════════════════════ */
static void ring_init(void) {
    memset(&ring, 0, sizeof ring);
    pthread_mutex_init(&ring.mu, NULL);
    pthread_cond_init(&ring.not_empty, NULL);
    pthread_cond_init(&ring.not_full, NULL);
}

/* producer: called from per-container log threads */
static void ring_push(const char *buf, int n) {
    pthread_mutex_lock(&ring.mu);
    while (ring.count == RING_CAP && !ring.done)
        pthread_cond_wait(&ring.not_full, &ring.mu);
    if (ring.done) { pthread_mutex_unlock(&ring.mu); return; }
    int slot = ring.tail;
    memcpy(ring.data[slot], buf, n);
    ring.len[slot] = n;
    ring.tail = (ring.tail + 1) % RING_CAP;
    ring.count++;
    pthread_cond_signal(&ring.not_empty);
    pthread_mutex_unlock(&ring.mu);
}

/* consumer: called from the dedicated writer thread */
static int ring_pop(char *buf, int *n) {
    pthread_mutex_lock(&ring.mu);
    while (ring.count == 0 && !ring.done)
        pthread_cond_wait(&ring.not_empty, &ring.mu);
    if (ring.count == 0 && ring.done) {
        pthread_mutex_unlock(&ring.mu);
        return -1;
    }
    int slot = ring.head;
    *n = ring.len[slot];
    memcpy(buf, ring.data[slot], *n);
    ring.head = (ring.head + 1) % RING_CAP;
    ring.count--;
    pthread_cond_signal(&ring.not_full);
    pthread_mutex_unlock(&ring.mu);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   LOG CONSUMER THREAD  (writes ring entries → log files)
══════════════════════════════════════════════════════════════ */
static void *log_consumer(void *arg) {
    (void)arg;
    char buf[BUF_SIZE];
    int  n;
    /* We write to per-container log files via each container's log_path.
       For simplicity the producer threads tag messages; here we just
       write raw bytes to a shared sink.  Per-container sinks are opened
       in the producer thread below. */
    while (ring_pop(buf, &n) == 0) {
        /* Each message starts with "LOGPATH:<path>\n" header (see producer).
           Strip it, open the file, append, close. */
        if (n > 8 && memcmp(buf, "LOGPATH:", 8) == 0) {
            char *nl = memchr(buf + 8, '\n', n - 8);
            if (!nl) continue;
            int hlen = (int)(nl - buf) + 1;
            char path[256];
            int plen = (int)(nl - (buf + 8));
            memcpy(path, buf + 8, plen);
            path[plen] = '\0';
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                write(fd, buf + hlen, n - hlen);
                close(fd);
            }
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   PER-CONTAINER LOG PRODUCER THREAD
══════════════════════════════════════════════════════════════ */
typedef struct { int pipe_fd; char log_path[256]; } LogArgs;

static void *log_producer(void *arg) {
    LogArgs *la = arg;
    char raw[BUF_SIZE - 300];
    char msg[BUF_SIZE];
    ssize_t n;
    while ((n = read(la->pipe_fd, raw, sizeof raw)) > 0) {
        int hlen = snprintf(msg, sizeof msg, "LOGPATH:%s\n", la->log_path);
        if (hlen + n < (int)sizeof msg) {
            memcpy(msg + hlen, raw, n);
            ring_push(msg, hlen + (int)n);
        }
    }
    close(la->pipe_fd);
    free(la);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   CONTAINER HELPERS
══════════════════════════════════════════════════════════════ */
static int find_slot(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == ST_EMPTY) return i;
    return -1;
}

static Container *find_by_name(const char *name) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != ST_EMPTY &&
            strcmp(containers[i].name, name) == 0)
            return &containers[i];
    return NULL;
}

/* child process: set up namespaces + rootfs, exec shell */
static int container_child(void *arg) {
    char **argv = arg;
    /* argv[0]=name  argv[1]=rootfs  argv[2]=cmd */
    const char *rootfs = argv[1];
    const char *cmd    = argv[2];

    /* mount proc inside the container */
    char proc_path[512];
    snprintf(proc_path, sizeof proc_path, "%s/proc", rootfs);
    mount("proc", proc_path, "proc", 0, NULL);

    /* chroot into rootfs */
    if (chroot(rootfs) != 0) { perror("chroot"); return 1; }
    chdir("/");

    /* set hostname to container name */
    sethostname(argv[0], strlen(argv[0]));

    /* exec the requested command */
    char *exec_argv[] = { (char*)cmd, NULL };
    execv(cmd, exec_argv);
    perror("execv");
    return 1;
}

/* ══════════════════════════════════════════════════════════════
   START CONTAINER
══════════════════════════════════════════════════════════════ */
static int start_container(const char *name, const char *rootfs,
                            const char *cmd, int foreground) {
    pthread_mutex_lock(&meta_mu);
    int slot = find_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&meta_mu);
        fprintf(stderr, "No free container slots\n");
        return -1;
    }
    Container *c = &containers[slot];
    memset(c, 0, sizeof *c);
    c->slot = slot;
    strncpy(c->name, name, NAME_LEN - 1);
    c->state          = ST_STARTING;
    c->soft_limit_kb  = 50 * 1024;   /* 50 MB default */
    c->hard_limit_kb  = 100 * 1024;  /* 100 MB default */
    c->start_time     = time(NULL);

    /* per-container log file */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof c->log_path, "%s/%s.log", LOG_DIR, name);

    /* pipe: container stdout/stderr → log producer */
    if (pipe(c->log_pipe) != 0) { perror("pipe"); pthread_mutex_unlock(&meta_mu); return -1; }

    pthread_mutex_unlock(&meta_mu);

    /* clone flags: new PID, UTS, mount namespaces */
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    char *child_argv[] = { (char*)name, (char*)rootfs, (char*)cmd, NULL };

    /* stack for child */
    static char stack[1024 * 1024];

    pid_t pid = clone(container_child, stack + sizeof stack, flags, child_argv);
    if (pid < 0) {
        perror("clone");
        close(c->log_pipe[0]); close(c->log_pipe[1]);
        pthread_mutex_lock(&meta_mu);
        c->state = ST_EMPTY;
        pthread_mutex_unlock(&meta_mu);
        return -1;
    }

    pthread_mutex_lock(&meta_mu);
    c->host_pid = pid;
    c->state    = ST_RUNNING;
    pthread_mutex_unlock(&meta_mu);

    /* close write end in parent, start log producer thread */
    close(c->log_pipe[1]);
    LogArgs *la = malloc(sizeof *la);
    la->pipe_fd = c->log_pipe[0];
    strncpy(la->log_path, c->log_path, sizeof la->log_path - 1);
    pthread_create(&c->log_thread, NULL, log_producer, la);
    pthread_detach(c->log_thread);

    /* register PID with kernel monitor */
    if (monitor_fd >= 0) {
        struct monitor_request mr;
        memset(&mr, 0, sizeof mr);
        mr.pid              = pid;
        mr.soft_limit_bytes = (unsigned long)c->soft_limit_kb * 1024;
        mr.hard_limit_bytes = (unsigned long)c->hard_limit_kb * 1024;
        strncpy(mr.container_id, name, MONITOR_NAME_LEN - 1);
        if (ioctl(monitor_fd, MONITOR_REGISTER, &mr) < 0)
            perror("ioctl MONITOR_REGISTER");
    }

    printf("[engine] Container '%s' started (host PID %d)\n", name, pid);

    if (foreground) {
        int status;
        waitpid(pid, &status, 0);
        pthread_mutex_lock(&meta_mu);
        c->state       = ST_STOPPED;
        c->exit_status = WEXITSTATUS(status);
        pthread_mutex_unlock(&meta_mu);
        printf("[engine] Container '%s' exited (status %d)\n", name, c->exit_status);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   STOP CONTAINER
══════════════════════════════════════════════════════════════ */
static void stop_container(const char *name) {
    pthread_mutex_lock(&meta_mu);
    Container *c = find_by_name(name);
    if (!c) { pthread_mutex_unlock(&meta_mu); fprintf(stderr, "No container: %s\n", name); return; }
    if (c->state != ST_RUNNING) { pthread_mutex_unlock(&meta_mu); fprintf(stderr, "%s not running\n", name); return; }
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&meta_mu);

    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);

    pthread_mutex_lock(&meta_mu);
    c->state       = ST_STOPPED;
    c->exit_status = WEXITSTATUS(status);
    pthread_mutex_unlock(&meta_mu);
    printf("[engine] Container '%s' stopped\n", name);
}

/* ══════════════════════════════════════════════════════════════
   PS  (list containers)
══════════════════════════════════════════════════════════════ */
static void list_containers(int out_fd) {
    char line[512];
    int n = snprintf(line, sizeof line,
        "%-16s %-8s %-10s %-10s %-10s\n",
        "NAME", "PID", "STATE", "SOFT_MB", "HARD_MB");
    write(out_fd, line, n);

    const char *state_str[] = {"empty","starting","running","stopped","killed"};

    pthread_mutex_lock(&meta_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (c->state == ST_EMPTY) continue;
        n = snprintf(line, sizeof line,
            "%-16s %-8d %-10s %-10ld %-10ld\n",
            c->name, c->host_pid,
            state_str[c->state],
            c->soft_limit_kb / 1024,
            c->hard_limit_kb / 1024);
        write(out_fd, line, n);
    }
    pthread_mutex_unlock(&meta_mu);
}

/* ══════════════════════════════════════════════════════════════
   LOGS  (cat a container log)
══════════════════════════════════════════════════════════════ */
static void show_logs(const char *name, int out_fd) {
    pthread_mutex_lock(&meta_mu);
    Container *c = find_by_name(name);
    char path[256] = "";
    if (c) strncpy(path, c->log_path, sizeof path - 1);
    pthread_mutex_unlock(&meta_mu);

    if (!path[0]) { dprintf(out_fd, "No container: %s\n", name); return; }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { dprintf(out_fd, "No log file yet for %s\n", name); return; }
    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        write(out_fd, buf, n);
    close(fd);
}

/* ══════════════════════════════════════════════════════════════
   SIGCHLD HANDLER  (reap zombies)
══════════════════════════════════════════════════════════════ */
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&meta_mu);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].host_pid == pid) {
                containers[i].state       = ST_STOPPED;
                containers[i].exit_status = WEXITSTATUS(status);
                break;
            }
        }
        pthread_mutex_unlock(&meta_mu);
    }
}

/* ══════════════════════════════════════════════════════════════
   SIGTERM/SIGINT HANDLER  (orderly shutdown)
══════════════════════════════════════════════════════════════ */
static void sigterm_handler(int sig) {
    (void)sig;
    supervisor_running = 0;
}

/* ══════════════════════════════════════════════════════════════
   SUPERVISOR  (long-running daemon, listens on UNIX socket)
══════════════════════════════════════════════════════════════ */
static void run_supervisor(const char *rootfs) {
    /* signal setup */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* ring buffer + consumer thread */
    ring_init();
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, log_consumer, NULL);

    /* open kernel monitor device */
    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[engine] Warning: cannot open /dev/container_monitor (%s)\n", strerror(errno));

    /* UNIX domain socket */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    unlink(SOCK_PATH);
    bind(srv, (struct sockaddr*)&addr, sizeof addr);
    listen(srv, 8);

    printf("[engine] Supervisor started. Listening on %s\n", SOCK_PATH);
    printf("[engine] rootfs: %s\n", rootfs);

    /* main accept loop */
    while (supervisor_running) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* read one command line */
        char buf[512] = {0};
        ssize_t n = read(cli, buf, sizeof buf - 1);
        if (n <= 0) { close(cli); continue; }
        buf[n] = '\0';
        /* trim newline */
        buf[strcspn(buf, "\n")] = '\0';

        /* parse: "CMD [ARG1] [ARG2]" */
        char *tok  = strtok(buf, " ");
        char *arg1 = strtok(NULL, " ");
        char *arg2 = strtok(NULL, " ");

        if (!tok) { close(cli); continue; }

        if (strcmp(tok, "start") == 0 && arg1) {
            const char *cmd = arg2 ? arg2 : "/bin/sh";
            start_container(arg1, rootfs, cmd, 0);
            dprintf(cli, "OK: started %s\n", arg1);
        } else if (strcmp(tok, "run") == 0 && arg1) {
            const char *cmd = arg2 ? arg2 : "/bin/sh";
            start_container(arg1, rootfs, cmd, 1);
            dprintf(cli, "OK: run %s done\n", arg1);
        } else if (strcmp(tok, "stop") == 0 && arg1) {
            stop_container(arg1);
            dprintf(cli, "OK: stopped %s\n", arg1);
        } else if (strcmp(tok, "ps") == 0) {
            list_containers(cli);
        } else if (strcmp(tok, "logs") == 0 && arg1) {
            show_logs(arg1, cli);
        } else {
            dprintf(cli, "ERR: unknown command '%s'\n", tok);
        }

        close(cli);
    }

    /* ── orderly shutdown ── */
    printf("[engine] Supervisor shutting down...\n");

    /* stop all running containers */
    pthread_mutex_lock(&meta_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == ST_RUNNING)
            kill(containers[i].host_pid, SIGTERM);
    }
    pthread_mutex_unlock(&meta_mu);

    /* drain remaining children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* stop log pipeline */
    pthread_mutex_lock(&ring.mu);
    ring.done = 1;
    pthread_cond_broadcast(&ring.not_empty);
    pthread_cond_broadcast(&ring.not_full);
    pthread_mutex_unlock(&ring.mu);
    pthread_join(consumer_tid, NULL);

    if (monitor_fd >= 0) close(monitor_fd);
    close(srv);
    unlink(SOCK_PATH);
    printf("[engine] Supervisor exited cleanly.\n");
}

/* ══════════════════════════════════════════════════════════════
   CLI CLIENT  (sends one command to the supervisor)
══════════════════════════════════════════════════════════════ */
static void send_command(int argc, char **argv) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        fprintf(stderr, "Cannot connect to supervisor (is it running?)\n");
        close(fd);
        return;
    }

    /* build command string */
    char msg[512] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strcat(msg, " ");
        strcat(msg, argv[i]);
    }
    strcat(msg, "\n");
    write(fd, msg, strlen(msg));

    /* print reply */
    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        fwrite(buf, 1, n, stdout);
    close(fd);
}

/* ══════════════════════════════════════════════════════════════
   MAIN
══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo %s supervisor <rootfs>          # start supervisor\n"
            "  sudo %s start <name> [cmd]           # launch container\n"
            "  sudo %s run   <name> [cmd]           # launch + wait\n"
            "  sudo %s stop  <name>                 # stop container\n"
            "  sudo %s ps                           # list containers\n"
            "  sudo %s logs  <name>                 # show logs\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <rootfs>\n", argv[0]); return 1; }
        run_supervisor(argv[2]);
    } else {
        /* all other subcommands go to the running supervisor */
        send_command(argc, argv);
    }
    return 0;
}
