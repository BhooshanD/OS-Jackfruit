/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Complete implementation covering:
 *   - UNIX domain socket control-plane IPC
 *   - container lifecycle with clone + namespaces
 *   - bounded-buffer producer/consumer logging
 *   - signal handling and graceful shutdown
 *   - kernel module integration via ioctl
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    char               log_path[PATH_MAX];
    int                stop_requested;   /* set before sending SIGTERM/SIGKILL */
    int                pipe_read_fd;     /* supervisor side of container pipe  */
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

/* Per-container producer-thread argument */
typedef struct {
    int              pipe_fd;           /* read end of container pipe */
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_arg_t;

typedef struct {
    int              server_fd;
    int              monitor_fd;
    int              should_stop;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer — used by signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Usage / parsing helpers                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded buffer                                                      */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push — producer side.
 * Blocks while buffer is full (unless shutting down).
 * Returns 0 on success, -1 if shutdown was triggered.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — consumer side.
 * Blocks while buffer is empty.
 * Returns 0 on success, 1 when shutdown and buffer is drained, -1 on error.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return 1;   /* drained + shutdown */
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging consumer thread                                             */
/* ------------------------------------------------------------------ */

/*
 * logging_thread — single consumer.
 * Pops chunks from the bounded buffer and appends to per-container log files.
 * Exits when shutdown is signalled and the buffer is fully drained.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;

    fprintf(stderr, "[logger] consumer thread started\n");

    for (;;) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == 1)   /* drained + shutdown */
            break;
        if (rc != 0)
            continue;

        /* Find log path for this container */
        char log_path[PATH_MAX] = {0};
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strncmp(c->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, c->log_path, PATH_MAX - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') {
            /* Fallback: write to a generic file */
            snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, item.container_id);
        }

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0, total = (ssize_t)item.length;
            const char *ptr = item.data;
            while (written < total) {
                ssize_t n = write(fd, ptr + written, (size_t)(total - written));
                if (n <= 0) break;
                written += n;
            }
            close(fd);
        }
    }

    fprintf(stderr, "[logger] consumer thread exiting\n");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Per-container producer thread                                       */
/* ------------------------------------------------------------------ */

static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    fprintf(stderr, "[producer:%s] started\n", pa->container_id);

    for (;;) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);

        n = read(pa->pipe_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break;   /* container exited / pipe closed */

        item.length = (size_t)n;
        if (bounded_buffer_push(pa->log_buffer, &item) != 0)
            break;
    }

    close(pa->pipe_fd);
    free(pa);
    fprintf(stderr, "[producer] exited\n");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Child clone entry-point                                             */
/* ------------------------------------------------------------------ */

/*
 * child_fn — runs inside the new namespaces.
 * Sets up filesystem isolation, mounts /proc, redirects stdio, execs command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout + stderr to the write end of the pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Set UTS hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal: /proc may already exist in the rootfs */
        perror("mount /proc (warning)");
    }

    /* Execute the requested command via /bin/sh */
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Monitor ioctl wrappers                                              */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container launch helper                                             */
/* ------------------------------------------------------------------ */

/*
 * launch_container — called by the supervisor in response to start/run.
 * Allocates a container_record, forks via clone(), starts a producer thread.
 */
static container_record_t *launch_container(supervisor_ctx_t *ctx,
                                            const control_request_t *req)
{
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        perror("pipe2");
        return NULL;
    }

    /* Prepare child config (on heap; child clones the address space) */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return NULL; }

    strncpy(cfg->id,       req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,   req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command,  req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value    = req->nice_value;
    cfg->log_write_fd  = pipefd[1];

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);
    /* stack memory leaks if we free it here while child may still use it;
     * in production we'd track it. For this project the child exec()s quickly. */
    if (pid < 0) {
        perror("clone");
        free(cfg);
        free(stack);
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }

    /* Close write end in the supervisor */
    close(pipefd[1]);

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Build container record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        free(stack);
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    rec->pipe_read_fd      = pipefd[0];
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* Register with kernel monitor (best-effort) */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, rec->id, pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes);

    /* Start producer thread for this container */
    producer_arg_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->pipe_fd    = pipefd[0];
        pa->log_buffer = &ctx->log_buffer;
        strncpy(pa->container_id, rec->id, CONTAINER_ID_LEN - 1);

        pthread_t tid;
        if (pthread_create(&tid, NULL, producer_thread, pa) == 0)
            pthread_detach(tid);
        else
            free(pa);
    }

    /* Prepend to container list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    fprintf(stderr, "[supervisor] started container %s pid=%d\n", rec->id, pid);
    return rec;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD reaping                                                     */
/* ------------------------------------------------------------------ */

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->exit_signal = 0;
                    c->state = (c->stop_requested) ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code   = 128 + c->exit_signal;
                    if (c->stop_requested)
                        c->state = CONTAINER_STOPPED;
                    else
                        c->state = CONTAINER_KILLED; /* hard-limit or external kill */
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, c->id, pid);
                fprintf(stderr, "[supervisor] container %s exited state=%s\n",
                        c->id, state_to_string(c->state));
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_sigchld_flag = 0;
static volatile sig_atomic_t g_stop_flag    = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    g_sigchld_flag = 1;
}

static void stop_handler(int sig)
{
    (void)sig;
    g_stop_flag = 1;
}

/* ------------------------------------------------------------------ */
/* Control-plane: handle one client connection                         */
/* ------------------------------------------------------------------ */

static void handle_control_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "bad request length");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        /* Check for duplicate ID */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strncmp(c->id, req.container_id, CONTAINER_ID_LEN) == 0 &&
                c->state == CONTAINER_RUNNING) {
                pthread_mutex_unlock(&ctx->metadata_lock);
                resp.status = -1;
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "container %s already running", req.container_id);
                send(client_fd, &resp, sizeof(resp), 0);
                return;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        container_record_t *rec = launch_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "failed to launch container");
        } else {
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "started container %s pid=%d", rec->id, rec->host_pid);
        }
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_RUN: {
        container_record_t *rec = launch_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "failed to launch container");
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }
        /* Ack to client that launch succeeded; client then waits for exit notification */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "running container %s pid=%d",
                 rec->id, rec->host_pid);
        send(client_fd, &resp, sizeof(resp), 0);

        /* Wait for the container to exit */
        int wstatus;
        waitpid(rec->host_pid, &wstatus, 0);

        /* Encode exit status and send second response */
        memset(&resp, 0, sizeof(resp));
        if (WIFEXITED(wstatus)) {
            resp.status = WEXITSTATUS(wstatus);
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container %s exited with code %d", rec->id, resp.status);
        } else if (WIFSIGNALED(wstatus)) {
            resp.status = 128 + WTERMSIG(wstatus);
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container %s killed by signal %d", rec->id, WTERMSIG(wstatus));
        }
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_PS: {
        /* Send one response per container, then a sentinel */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            memset(&resp, 0, sizeof(resp));
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "%-16s %-8d %-10s soft=%-8lu hard=%-8lu exit=%d",
                     c->id, c->host_pid,
                     state_to_string(c->state),
                     c->soft_limit_bytes >> 20,
                     c->hard_limit_bytes >> 20,
                     c->exit_code);
            send(client_fd, &resp, sizeof(resp), 0);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        /* Sentinel: empty message, status = 1 */
        memset(&resp, 0, sizeof(resp));
        resp.status = 1;
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_LOGS: {
        /* Find and stream the log file */
        char log_path[PATH_MAX] = {0};
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strncmp(c->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, c->log_path, PATH_MAX - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container %s not found", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }

        /* First response: OK */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "log:%s", log_path);
        send(client_fd, &resp, sizeof(resp), 0);

        /* Stream log file content */
        int lfd = open(log_path, O_RDONLY);
        if (lfd >= 0) {
            char buf[4096];
            ssize_t nr;
            while ((nr = read(lfd, buf, sizeof(buf))) > 0)
                send(client_fd, buf, (size_t)nr, 0);
            close(lfd);
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strncmp(c->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                if (c->state == CONTAINER_RUNNING) {
                    c->stop_requested = 1;
                    kill(c->host_pid, SIGTERM);
                    /* Give it a moment, then SIGKILL */
                    sleep(1);
                    kill(c->host_pid, SIGKILL);
                    c->state = CONTAINER_STOPPED;
                    resp.status = 0;
                    snprintf(resp.message, CONTROL_MESSAGE_LEN,
                             "stopped container %s", c->id);
                } else {
                    resp.status = -1;
                    snprintf(resp.message, CONTROL_MESSAGE_LEN,
                             "container %s is not running", c->id);
                }
                break;
            }
            c = c->next;
        }
        if (c == NULL) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container %s not found", req.container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx          = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init");
                   pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* --- Open kernel monitor device (best-effort) --- */
    ctx.monitor_fd = open("/dev/" MONITOR_DEV_NAME, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] WARNING: cannot open /dev/%s: %s\n",
                MONITOR_DEV_NAME, strerror(errno));

    /* --- Create UNIX domain socket --- */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[supervisor] listening on %s\n", CONTROL_PATH);
    fprintf(stderr, "[supervisor] base rootfs: %s\n", rootfs);

    /* --- Signal handling --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = stop_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* --- Start logging consumer thread --- */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc; perror("pthread_create (logger)");
        return 1;
    }

    /* --- Make accept() non-blocking so we can handle signals --- */
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    /* --- Event loop --- */
    while (!g_stop_flag) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            reap_children(&ctx);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(10000);  /* 10 ms */
                continue;
            }
            perror("accept");
            break;
        }
        handle_control_request(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

    /* --- Stop all running containers --- */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    sleep(1);
    reap_children(&ctx);

    /* --- Stop logger thread --- */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* --- Free container list --- */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* --- Cleanup --- */
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "[supervisor] clean exit\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client-side helpers                                                 */
/* ------------------------------------------------------------------ */

static int connect_to_supervisor(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return -1;
    }
    return fd;
}

static int send_control_request(const control_request_t *req)
{
    int fd = connect_to_supervisor();
    if (fd < 0) return 1;

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    control_response_t resp;
    ssize_t n;

    /* PS streams multiple responses until sentinel */
    if (req->kind == CMD_PS) {
        printf("%-16s %-8s %-10s %-14s %-14s %s\n",
               "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "EXIT");
        for (;;) {
            n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
            if (n <= 0) break;
            if (resp.status == 1) break;  /* sentinel */
            printf("%s\n", resp.message);
        }
        close(fd);
        return 0;
    }

    /* LOGS: first response is OK / error, then raw data */
    if (req->kind == CMD_LOGS) {
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n <= 0 || resp.status != 0) {
            fprintf(stderr, "logs: %s\n", resp.status != 0 ? resp.message : "error");
            close(fd); return 1;
        }
        char buf[4096];
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, (size_t)n, stdout);
        close(fd);
        return 0;
    }

    /* RUN: two responses — launch ack, then exit status */
    if (req->kind == CMD_RUN) {
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n <= 0) { close(fd); return 1; }
        if (resp.status != 0) {
            fprintf(stderr, "run: %s\n", resp.message);
            close(fd);
            return 1;
        }
        fprintf(stderr, "%s\n", resp.message);
        /* Wait for container to finish */
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n > 0) fprintf(stderr, "%s\n", resp.message);
        close(fd);
        return resp.status;
    }

    /* Default: single response */
    n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n <= 0) { close(fd); return 1; }
    fprintf(stderr, "%s\n", resp.message);
    close(fd);
    return resp.status == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI command handlers                                                */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}