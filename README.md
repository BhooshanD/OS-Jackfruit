# Complete Demo & Screenshot Guide
## Multi-Container Runtime — All 8 Tasks

---

## Prerequisites: One-Time VM Setup

```bash
# 1. Install dependencies (Ubuntu 22.04 / 24.04)
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

# 2. Confirm kernel headers are present
ls /usr/src/linux-headers-$(uname -r)/

# 3. Run the environment preflight check
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

---

## Step 0: Build Everything

```bash
cd boilerplate          # or wherever your source lives
make                    # builds engine, monitor.ko, memory_hog, cpu_hog, io_pulse

# Verify
ls -lh engine monitor.ko memory_hog cpu_hog io_pulse
```

---

## Step 1: Prepare Root Filesystems

```bash
# Download Alpine mini rootfs (once)
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make per-container writable copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma    # used for memory / scheduling experiments

# Copy workload binaries into the rootfs images so containers can run them
cp memory_hog cpu_hog io_pulse rootfs-alpha/
cp memory_hog cpu_hog io_pulse rootfs-beta/
cp memory_hog cpu_hog io_pulse rootfs-gamma/
```

---

## Step 2: Load the Kernel Module

```bash
# Load
sudo insmod monitor.ko

# Confirm device node exists
ls -l /dev/container_monitor
# Expected: crw------- 1 root root <major>, 0 ...  /dev/container_monitor

# Confirm no errors at load time
dmesg | tail -5
# Expected: [container_monitor] Module loaded. Device: /dev/container_monitor
```

---

## Step 3: Start the Supervisor

Open **Terminal A** — keep it open for the whole demo:

```bash
# supervisor takes base-rootfs as its argument (used for reference only)
sudo ./engine supervisor ./rootfs-base
```

Expected output in Terminal A:
```
[supervisor] listening on /tmp/mini_runtime.sock
[supervisor] base rootfs: ./rootfs-base
[logger] consumer thread started
```

Leave this terminal running.

---

## Task 1 — Screenshot 1: Multi-Container Supervision

Open **Terminal B**:

```bash
# Start two containers running a long-lived command
sudo ./engine start alpha ./rootfs-alpha "while true; do echo hello-alpha; sleep 2; done"
sudo ./engine start beta  ./rootfs-beta  "while true; do echo hello-beta;  sleep 3; done"

# Confirm the supervisor shows both as running
sudo ./engine ps
```

In Terminal A you will see:
```
[supervisor] started container alpha pid=<N>
[supervisor] started container beta  pid=<M>
```

**Take Screenshot 1 here** — show Terminal A (or B) with both containers started and the `ps` output showing two running containers.

---

## Task 2 — Screenshot 2: Metadata Tracking (`ps`)

```bash
# In Terminal B
sudo ./engine ps
```

Expected output:
```
ID               PID      STATE      SOFT(MiB)      HARD(MiB)      EXIT
alpha            <pid>    running    40             64             0
beta             <pid>    running    40             64             0
```

**Take Screenshot 2 here** — full `ps` table with both containers.

---

## Task 3 — Screenshot 3: Bounded-Buffer Logging

```bash
# Wait ~10 seconds so containers produce some output, then:
sudo ./engine logs alpha
sudo ./engine logs beta
```

You should see lines like:
```
hello-alpha
hello-alpha
hello-alpha
...
```

To also show pipeline activity, look at Terminal A where the supervisor prints:
```
[producer:alpha] started
[producer:beta]  started
```

```bash
# Optionally inspect the log files on disk
ls -lh logs/
cat logs/alpha.log
cat logs/beta.log
```

**Take Screenshot 3 here** — show `logs alpha` output AND `ls -lh logs/` showing both log files with nonzero sizes.

---

## Task 2 continued — Screenshot 4: CLI & IPC (stop + ps after stop)

```bash
# In Terminal B — stop one container and check state transitions
sudo ./engine stop alpha

# Check metadata updated
sudo ./engine ps
```

Expected `ps` output after stop:
```
alpha   <pid>   stopped    40   64   0
beta    <pid>   running    40   64   0
```

Also start a new container to demonstrate `run` (blocks until exit):
```bash
sudo ./engine run gamma ./rootfs-gamma "echo 'run-complete'; exit 0"
# Should print the container output and return immediately
```

**Take Screenshot 4 here** — show the `stop` command, the subsequent `ps` showing `stopped`, and the `run` output.

---

## Task 4a — Screenshot 5: Soft-Limit Warning

Start a container with a **low soft limit** and run the memory hog:

```bash
# Soft = 10 MiB, Hard = 64 MiB — memory_hog allocates 50 MiB, triggering soft
sudo ./engine start memtest ./rootfs-alpha \
    "/memory_hog 52" \
    --soft-mib 10 --hard-mib 64
```

Wait 2–3 seconds, then check dmesg:

```bash
dmesg | grep -E "SOFT|HARD|container_monitor" | tail -20
```

Expected:
```
[container_monitor] Registering container=memtest pid=<N> soft=10485760 hard=67108864
[container_monitor] SOFT LIMIT container=memtest pid=<N> rss=<rss> limit=10485760
```

**Take Screenshot 5 here** — `dmesg` output showing the SOFT LIMIT line.

---

## Task 4b — Screenshot 6: Hard-Limit Enforcement

Start a container with a soft AND hard limit below what memory_hog allocates:

```bash
# Hard = 20 MiB — memory_hog will be killed
sudo ./engine start hardtest ./rootfs-beta \
    "/memory_hog 100" \
    --soft-mib 10 --hard-mib 20
```

Wait 3–5 seconds (timer fires every 1 second):

```bash
dmesg | grep -E "SOFT|HARD|container_monitor" | tail -20
sudo ./engine ps
```

Expected dmesg:
```
[container_monitor] SOFT LIMIT container=hardtest pid=<N> rss=... limit=10485760
[container_monitor] HARD LIMIT container=hardtest pid=<N> rss=... limit=20971520
```

Expected `ps` after kill:
```
hardtest   <pid>   killed   10   20   137
```
(exit code 137 = 128 + SIGKILL=9)

**Take Screenshot 6 here** — dmesg lines showing HARD LIMIT + `ps` showing state=killed.

---

## Task 5 — Screenshot 7: Scheduling Experiments

### Experiment A: CPU-bound workloads at different nice values

```bash
# Terminal B: start two CPU hogs simultaneously
# One at normal priority (nice=0) and one at lower priority (nice=10)

# Prepare two rootfs copies
cp -a rootfs-base rootfs-sched1
cp -a rootfs-base rootfs-sched2
cp cpu_hog rootfs-sched1/
cp cpu_hog rootfs-sched2/

# Start simultaneously
time sudo ./engine run sched1 ./rootfs-sched1 "/cpu_hog 15" &
time sudo ./engine run sched2 ./rootfs-sched2 "/cpu_hog 15" \
    --nice 10 &
wait
```

Compare the real-time output. The nice=0 container should finish noticeably faster.

### Experiment B: CPU-bound vs I/O-bound

```bash
cp -a rootfs-base rootfs-cpu
cp -a rootfs-base rootfs-io
cp cpu_hog rootfs-cpu/
cp io_pulse rootfs-io/

time sudo ./engine run cpuwork ./rootfs-cpu "/cpu_hog 10" &
time sudo ./engine run iowork  ./rootfs-io  "/io_pulse 10" &
wait
```

The I/O-bound task voluntarily blocks on disk/sleep — the CPU-bound task gets more CPU slices but completes in similar wall-clock time to the I/O task.

```bash
# Show running both at the same time with ps
sudo ./engine ps
```

**Take Screenshot 7 here** — show `ps` with both scheduling containers running, and the terminal output with timing results.

---

## Task 6 — Screenshot 8: Clean Teardown

```bash
# Stop any remaining containers
sudo ./engine stop beta      # if still running
sudo ./engine ps             # confirm all stopped/exited

# Send SIGTERM to the supervisor (Ctrl+C in Terminal A, or:)
# In Terminal B:
sudo kill -TERM $(pgrep -f "engine supervisor")
```

In Terminal A you should see:
```
[supervisor] shutting down...
[logger] consumer thread exiting
[supervisor] clean exit
```

Then verify no zombies:

```bash
# No zombie container processes
ps aux | grep -E "Z|zombie"

# Supervisor has exited
pgrep -f "engine supervisor" || echo "supervisor not running"

# Log files still intact
ls -lh logs/
```

Unload the kernel module:

```bash
sudo rmmod monitor

# Confirm clean unload
dmesg | tail -5
# Expected: [container_monitor] Module unloaded.

# Device node gone
ls /dev/container_monitor 2>&1
# Expected: No such file or directory
```

**Take Screenshot 8 here** — show Terminal A with clean exit messages, `ps aux` showing no zombies, and `dmesg` showing module unloaded.

---

## Quick Reference: Full Sequence (Copy-Paste)

```bash
# === Build ===
cd boilerplate && make

# === Setup ===
mkdir -p rootfs-base
wget -q https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
for name in alpha beta gamma; do cp -a rootfs-base rootfs-$name; done
cp memory_hog cpu_hog io_pulse rootfs-alpha/ rootfs-beta/ rootfs-gamma/

# === Load module ===
sudo insmod monitor.ko
ls -l /dev/container_monitor

# === Terminal A: Start supervisor ===
sudo ./engine supervisor ./rootfs-base

# === Terminal B: Demo ===
sudo ./engine start alpha ./rootfs-alpha "while true; do echo hello-alpha; sleep 2; done"
sudo ./engine start beta  ./rootfs-beta  "while true; do echo hello-beta; sleep 3; done"
sudo ./engine ps
sleep 6
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine ps

# === Memory limit tests ===
cp -a rootfs-base rootfs-memtest && cp memory_hog rootfs-memtest/
sudo ./engine start memtest ./rootfs-memtest "/memory_hog 52" --soft-mib 10 --hard-mib 64
sleep 3 && dmesg | grep container_monitor | tail -5
sudo ./engine start hardtest ./rootfs-beta "/memory_hog 100" --soft-mib 10 --hard-mib 20
sleep 5 && dmesg | grep container_monitor | tail -10
sudo ./engine ps

# === Scheduler experiments ===
cp -a rootfs-base rootfs-sched1 && cp -a rootfs-base rootfs-sched2
cp cpu_hog rootfs-sched1/ && cp cpu_hog rootfs-sched2/
time sudo ./engine run sched1 ./rootfs-sched1 "/cpu_hog 15" &
time sudo ./engine run sched2 ./rootfs-sched2 "/cpu_hog 15" --nice 10 &
wait

# === Teardown ===
sudo ./engine stop beta
sudo ./engine ps
# Ctrl+C in Terminal A (or kill the supervisor)
ps aux | grep -E "Z|defunct" | grep -v grep
sudo rmmod monitor
dmesg | tail -5
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `insmod: ERROR: could not insert module` | Check `dmesg` for the reason. Make sure `make` completed for the kernel module. Ensure Secure Boot is OFF. |
| `/dev/container_monitor` doesn't appear | Check `dmesg` for device creation errors. Try `sudo modprobe -r monitor` first. |
| `connect (is the supervisor running?)` | Start `sudo ./engine supervisor ./rootfs-base` in another terminal first. |
| `chroot: No such file or directory` | The rootfs path is wrong or the rootfs wasn't extracted correctly. |
| Memory hog not found inside container | `cp memory_hog ./rootfs-<name>/` before starting that container. |
| Zombies seen in `ps aux` | SIGCHLD handler is registered but may miss fast-exiting children — add `waitpid(-1, NULL, WNOHANG)` loop at the top of the event loop as a fallback. |
| `mount /proc` warning in log | Normal if `/proc` already exists in the Alpine rootfs — it is non-fatal. |
