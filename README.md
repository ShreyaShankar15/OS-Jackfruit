# OS-Jackfruit: Supervised Multi-Container Runtime

## Team Information

| Name | SRN |
|------|-----|
| Shreya S | PES1UG24CS623 |
| Vaishnavi H Shetty | PES1UG24CS641 |

---

## Project Overview

OS-Jackfruit is a lightweight Linux container runtime built in C, inspired by Docker. It consists of two components:
- **User-space engine** (`engine.c`): A supervisor process that manages multiple isolated containers using Linux namespaces, a CLI for control, and a bounded-buffer logging pipeline.
- **Kernel module** (`monitor.c`): A Linux Kernel Module (LKM) that monitors container memory usage and enforces soft/hard limits via `/dev/container_monitor`.

---

## Build Instructions

### Prerequisites
- Ubuntu 22.04 (not WSL)
- `build-essential`, `linux-headers-$(uname -r)`

```bash
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate
make
```

This builds: `engine`, `monitor.ko`, `cpu_hog`, `memory_hog`, `io_pulse`.

### Alpine rootfs setup

```bash
mkdir rootfs
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

---

## Running the Runtime

### Step 1 — Load the kernel module
```bash
sudo insmod monitor.ko
sudo dmesg | tail -5   # verify: "[container_monitor] Module loaded"
```

### Step 2 — Start the supervisor (Terminal 1)
```bash
sudo ./engine supervisor ./rootfs
```

### Step 3 — Use the CLI (Terminal 2)

```bash
# Start containers
sudo ./engine start alpha ./rootfs /bin/sh
sudo ./engine start beta ./rootfs /bin/sh

# List containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
```

### Step 4 — Unload the kernel module
```bash
sudo rmmod monitor
sudo dmesg | tail -5   # verify: "[container_monitor] Module unloaded"
```

---

## Screenshots

1. **Environment preflight check** — `sudo ./environment-check.sh` output showing all [OK]
2. **Supervisor started** — Terminal 1 showing supervisor ready on UNIX socket
3. **Single container running** — `sudo ./engine start alpha` + `sudo ./engine ps`
4. **Multi-container running** — Two containers (alpha + beta) in `ps` output simultaneously
5. **CLI commands** — stop/ps/logs commands working across containers
6. **Kernel module loaded** — `dmesg` showing `[container_monitor] Module loaded`
7. **Container PID registered** — `dmesg` showing `Registering container= pid=XXXX`
8. **Clean teardown** — `dmesg` showing `[container_monitor] Module unloaded`

---

## Engineering Analysis

### 1. Namespace Isolation
Each container is created using `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` flags. This gives each container its own PID namespace (so it sees itself as PID 1), its own hostname via UTS namespace, and its own mount namespace so filesystem changes don't affect the host. We use `chroot()` into an Alpine Linux rootfs to complete the filesystem isolation.

### 2. IPC Between CLI and Supervisor
The CLI client and the long-running supervisor communicate via a **UNIX domain socket** at `/tmp/engine.sock`. The supervisor runs an `accept()` loop, reads one command per connection, executes it, writes the response, and closes the connection. This is simpler and more reliable than named pipes for request-response patterns, and supports concurrent clients.

### 3. Bounded-Buffer Logging (Producer-Consumer)
Each container's stdout/stderr is captured via a `pipe()`. A **producer thread** reads from the pipe and pushes data into a shared ring buffer (capacity: 64 slots, each 4096 bytes). A single **consumer thread** pops entries from the ring buffer and writes them to per-container log files. The ring buffer is protected by a `pthread_mutex_t` with two `pthread_cond_t` variables (`not_empty`, `not_full`) to block producers when full and the consumer when empty. This decouples I/O-heavy log writing from container management.

### 4. Signal Handling
- `SIGCHLD`: Handled with `SA_RESTART | SA_NOCLDSTOP`. The handler calls `waitpid(-1, WNOHANG)` in a loop to reap all zombie children immediately and update their state in the metadata store.
- `SIGTERM` / `SIGINT`: Sets a `volatile int supervisor_running = 0` flag, causing the accept loop to exit and triggering orderly shutdown (stop all containers, drain log pipeline, close FDs).

### 5. Kernel Module Memory Enforcement
The LKM creates `/dev/container_monitor` using `misc_register()`. The supervisor registers container PIDs via `ioctl(MONITOR_REGISTER)`. The module maintains a mutex-protected linked list of monitored PIDs. A kernel timer periodically checks each container's RSS memory usage. If RSS exceeds the **soft limit**, a warning is printed to `dmesg`. If RSS exceeds the **hard limit**, the kernel sends `SIGKILL` to the container process.

---

## Scheduler Experiment Results (Task 5)

### Experiment: CPU-bound workload (`cpu_hog`) under different scheduling priorities

The `cpu_hog` program performs continuous arithmetic operations for 10 seconds. We measured wall-clock time under three different `nice` values on our single-core VirtualBox VM.

| Priority | Nice Value | Real Time | User Time | Sys Time |
|----------|-----------|-----------|-----------|----------|
| Normal   | 0         | 9.477s    | 6.375s    | 3.093s   |
| Low      | +19       | 9.868s    | 9.835s    | 0.023s   |
| High     | -20       | 10.214s   | 0.004s    | 0.024s   |

### Analysis

On a **single-core VM with no competing processes**, the CFS scheduler shows minimal wall-clock difference between nice values because the CPU hog is the dominant process regardless of priority. The more revealing difference is in **user vs sys time split**:

- At nice 0: significant sys time (3.093s) reflects normal kernel overhead.
- At nice +19: almost all time is user-space (9.835s user, 0.023s sys), meaning the low-priority process runs uninterrupted when no higher-priority work exists — CFS still gives it full CPU when idle.
- At nice -20: paradoxically slower wall time because sudo and shell overhead adds latency at process start.

**Conclusion**: CFS priority effects are most visible under **CPU contention** (multiple competing processes). On a lightly loaded single-core VM, CFS efficiently fills idle CPU regardless of nice value. In a multi-process workload, nice -20 processes would receive proportionally more CPU shares (up to 10x more than nice +19), directly reducing their completion time.

---

## Design Decisions

- **UNIX socket over FIFO**: Chosen for IPC because sockets support bidirectional communication in a single connection, making request-response CLI commands cleaner.
- **Ring buffer over unbounded queue**: Bounded capacity (64 slots) prevents memory exhaustion if containers produce logs faster than the consumer can write them.
- **`clone()` over `fork()+unshare()`**: `clone()` allows atomically creating the child in new namespaces, avoiding a race window between fork and namespace setup.
- **`misc_register()` for kernel device**: Simpler than manually allocating a major number; automatically creates `/dev/container_monitor` via udev.

---

## Cleanup

```bash
# Remove compiled binaries
make clean

# Unload kernel module
sudo rmmod monitor

# Remove log files
rm -rf /tmp/jackfruit_logs
rm -f /tmp/engine.sock
```
