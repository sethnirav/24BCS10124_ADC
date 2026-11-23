# Thread Pool Job Scheduler in C
### A Fault-Aware, Priority-Based Job Execution Engine

A complete C11 + POSIX Pthreads project. Users submit jobs from a menu-driven terminal program. The jobs wait in bounded, thread-safe circular queues (one queue per priority), and a fixed pool of reusable worker threads runs them. The program supports retries, keeps logs on disk and measures its own performance.

---

## 1. Problem statement
Creating a new thread for every task costs time and memory, and the number of threads can grow without limit. A **thread pool** creates a fixed number of workers once and reuses them. A **scheduler** decides which waiting job runs next. Real systems also have to deal with failing tasks, overload (full queues) and clean shutdown. This project implements all of these in plain C.

## 2. Objectives
- Accept, validate and track jobs through a clear lifecycle.
- Hold jobs in a hand-written bounded circular queue protected by a mutex and condition variables.
- Schedule HIGH > NORMAL > LOW, keep FIFO order within a priority, and prevent starvation through ageing.
- Run jobs concurrently on a configurable, reusable pthread pool.
- Dispatch tasks through a function-pointer registry.
- Retry failed jobs up to a limit, and log everything to files.
- Measure real performance and compare sequential, thread-per-job and thread-pool execution.

## 3. Features
| Area | What is implemented |
|---|---|
| CLI | 14-option menu with a dashboard, aligned tables, colour status labels (ANSI, auto-disabled when not a TTY, or via `NO_COLOR` / `--no-color`) |
| Jobs | ID, name, type, priority, status, submit/start/finish times, last and total execution time, retry count / limit, result or error |
| States | `PENDING → QUEUED → RUNNING → COMPLETED / FAILED / RETRYING → RUNNING …` (invalid transitions are rejected) |
| Queue | Bounded ring buffer: `try_push/push/try_pop/pop/peek/close`, with full/empty/closed results |
| Scheduling | Strict priority with ageing: a job waiting longer than `ageing_ms` moves up one level |
| Pool | N pthreads (1–64), resizable at runtime, graceful retirement, all threads joined |
| Tasks | Checksum (CRC-32), Data validation, File processing, Calculation, Failure simulation |
| Faults | Error capture, retry with a configurable limit, `RETRYING` state, no infinite loops, failure counters |
| Metrics | submitted / rejected / completed / failed / pending / running / retries, average / min / max execution time, average wait, throughput, worker utilization |
| Files | execution log, error log, append-only job journal, text performance report, binary metrics snapshot, config file |

## 4. Architecture
```text
 Job Submission (CLI, main.c)
        │  scheduler_submit()  – validates input, rejects when the queue is full
        ▼
 Job Scheduler (scheduler.c)  – owns all Jobs (dynamic registry), retry logic, metrics
        │  one bounded queue per priority
        ▼
 Priority Queues (job_queue.c)   HIGH [ring]  NORMAL [ring]  LOW [ring]
        │  scheduler_next_job(): selection policy + condition variable wait
        ▼
 Worker Thread Pool (worker_pool.c)  W1 W2 … Wn  (created once, reused)
        │  task_execute(): function-pointer registry
        ▼
 Task Execution (task_handlers.c)
        │  scheduler_complete_job(): COMPLETED / RETRYING (re-queued) / FAILED
        ▼
 Result Management + Logging (logger.c)  →  Performance Monitoring (metrics.c)
```

### Scheduling policy
`effective_level = base_level + floor(head_wait_ms / ageing_ms)`, capped at HIGH (LOW=0, NORMAL=1, HIGH=2).
Only the **head** of each queue is compared, so jobs with the same priority always run in FIFO order. The highest effective level wins. A tie goes to the job that has waited longest, which lets an aged LOW job beat a stream of new HIGH jobs. Set `ageing_ms = 0` for strict priority. The policy is a pure function (`scheduler_select_priority`), kept apart from the queue code and unit-tested.

### Synchronisation design
- `Scheduler.lock` protects the job registry, every Job field that changes after submission, the counters and the shutdown flag.
- `work_cond` wakes workers when work arrives. `idle_cond` wakes `scheduler_wait_idle()` when no unfinished jobs remain. Waits are always in `while` loops, so spurious wake-ups are handled and nothing busy-waits.
- Lock order is always **scheduler → queue → logger/metrics**. No module calls back up this chain, so deadlock is impossible.
- Each queue has its own mutex and `not_empty` / `not_full` condition variables, so the queue is thread-safe on its own (tested with 3 producers and 3 consumers).
- Workers are retired through a C11 `atomic_bool` and an exit-predicate **function pointer** that is checked inside the scheduler's wait loop.
- The CLI only reads **copies** of jobs taken under the lock (`scheduler_snapshot_jobs`), never live pointers.

### Fault handling
A handler returns `-1` with an error message. If `retry_count < max_retries`, the job becomes `RETRYING`, its retry count goes up and it goes back to the end of its priority queue. Otherwise it becomes `FAILED`. Retries are skipped during shutdown, and a retry whose queue is full is marked FAILED instead of looping. Every failure goes to `logs/error.log` and the journal.

## 5. Technology
C11 (`-std=c11`), GCC, POSIX Pthreads, `<stdatomic.h>`, POSIX `dirent`/`stat`/`getrusage`, Make. No other language, framework or library.

## 6. Directory structure
```text
ThreadPoolJobScheduler/
├── include/  config.h job.h job_queue.h scheduler.h worker_pool.h task_handlers.h metrics.h logger.h
├── src/      main.c config.c job.c job_queue.c scheduler.c worker_pool.c task_handlers.c metrics.c logger.c
├── tests/    test_common.h test_queue.c test_scheduler.c test_task_handlers.c test_fault_handling.c
├── benchmarks/benchmark.c
├── test_data/            controlled files for the File Processing task
├── docs/samples/         output captured from real runs (see §12)
├── logs/  reports/       created/filled at runtime
├── Makefile  README.md  .gitignore
```

## 7. Build
Requires Linux, Ubuntu or WSL2 Ubuntu with `gcc` and `make` (`sudo apt install build-essential`).
```bash
make            # builds bin/scheduler with -Wall -Wextra -Wpedantic -Wshadow ... (0 warnings)
make clean
```

## 8. Run
```bash
./bin/scheduler                 # interactive menu
./bin/scheduler --workers 8     # override the worker count
./bin/scheduler --demo          # non-interactive demonstration (15 jobs + 1 invalid)
./bin/scheduler --no-color      # plain text output
```
Run the program from the project root, because the log, report and `test_data/` paths are relative to it.

## 9. CLI guide
| # | Option | Description |
|---|---|---|
| 1 | Dashboard | Uptime, workers, utilization, counts per state, queue depths, metrics, history from earlier sessions |
| 2 | Submit New Job | Name → task type → priority → task input → retry limit |
| 3 | View All Jobs | Table of every job |
| 4 | Search Job by ID | Full details including timestamps and the last error |
| 5–8 | Pending / Running / Completed / Failed | Filtered tables (Pending shows QUEUED and RETRYING) |
| 9 | Worker Pool Information | State, current job, jobs processed, busy time and utilization per thread |
| 10 | Performance Metrics | All metrics listed in §3 |
| 11 | Scheduler Configuration | Worker count (live resize), default retry limit, ageing threshold; saved to `scheduler.conf` |
| 12 | Shutdown System | Graceful (finish queued jobs) or immediate (cancel queued jobs → FAILED) |
| 13 | Load Demo Workload | Submits the demo job set |
| 14 | Export Performance Report | Writes `reports/performance_report.txt` |

End of input (Ctrl-D) also triggers a graceful shutdown.

## 10. Task types (function-pointer registry)
| Type | Input | Behaviour |
|---|---|---|
| Checksum | text | CRC-32 (IEEE) and byte sum |
| Data Validation | `name:age;name:age…` | Name must be 1–32 letters or spaces, age 0–150. Reports valid/invalid counts; fails only if there are no records |
| File Processing | sub-directory of `test_data/` (empty = root) | Reads every regular file: files, bytes, lines, combined CRC-32. Absolute paths, `..` and unusual characters are rejected |
| Calculation | op + n | sum 1..n (n ≤ 1e9), factorial (n ≤ 20), fibonacci (n ≤ 93), prime count with a sieve (n ≤ 2e7) |
| Failure Simulation | fail_attempts N (-1 = always) | Fails on the first N attempts, then succeeds; used to show retries |

```c
typedef int (*TaskHandler)(const Job *, char *result, size_t, char *error, size_t);
static const TaskDescriptor REGISTRY[TASK_TYPE_COUNT] = { {TASK_CHECKSUM, "Checksum", ..., handle_checksum}, ... };
return task_registry_lookup(job->type)(job, result, rlen, error, elen);
```
The program never runs shell commands or user-supplied code.

## 11. Testing
```bash
make test    # 4 suites, ~320 checks
make asan    # same tests with AddressSanitizer + LeakSanitizer + UBSan
make tsan    # same tests with ThreadSanitizer (data races / lock misuse)
```
Coverage: valid and invalid submissions, queue full and empty, FIFO and wrap-around, queue close, multi-threaded producer/consumer, priority order, ageing, status transitions, true concurrency (4 jobs observed RUNNING at the same time using a barrier), pool growing and shrinking while busy, graceful shutdown, cancellation, each task handler (with known values such as CRC32("123456789") = CBF43926 and π(10⁶) = 78498), path-traversal rejection, transient and permanent failures, exact retry limits, failure statistics, log and journal contents, corrupted journal / snapshot / config files. The `asan` and `tsan` runs report no errors, leaks or races.

## 12. Sample output (`docs/samples/`)
These files were captured from real runs of this code:
`sample_demo_run.txt`, `sample_interactive_session.txt`, `sample_execution.log`, `sample_error.log`, `sample_job_history.journal`, `sample_performance_report.txt`, `sample_benchmark_results.txt`.

## 13. Benchmarking
```bash
make bench
./bin/benchmark [workers] [heavy_jobs] [heavy_n] [light_jobs] [light_n]   # defaults: nCPU 64 2000000 2000 2000
```
The benchmark runs the same prime-count workload three ways and reports wall time, average handler time, throughput, process CPU time and context switches. Results go to the console and to `reports/benchmark_results.txt`.

Example measured on a 16-CPU sandbox (your numbers will differ):
- **CPU-heavy** (64 × π(2·10⁶)): sequential 529 ms, thread-per-job 65 ms, pool 72 ms. Both parallel methods are about 7–8× faster. Thread-per-job was slightly faster here because 64 threads on 16 cores is cheap.
- **Many tiny jobs** (2000 × π(2000)): sequential 5.7 ms, thread-per-job 326 ms, pool 14 ms. Creating a thread per job is about 23× slower than the pool. For jobs this small, plain sequential execution is fastest of all.

So the pool is **not always** the fastest. Its advantages are bounded resource use and low per-job overhead. The benefit of parallelism depends on how long each job runs and how many cores are available. Context-switch counts depend on the kernel and showed 0 in the sandbox.

## 14. Persistent vs. in-memory data
| Persistent (survives restart) | In-memory (current run only) |
|---|---|
| `logs/execution.log`, `logs/error.log` | Job registry and queues |
| `logs/job_history.journal` (append-only; read at start-up, malformed or truncated lines skipped) | Live metrics and worker statistics |
| `reports/performance_report.txt`, `reports/metrics_snapshot.bin` (fwrite/fread, magic + version checked) | |
| `scheduler.conf` (written atomically through temp file + rename) | |

Jobs are **not** restored after a restart. The journal is a history, not a recovery log.

## 15. Advanced C concepts – where to find them
| Concept | Where |
|---|---|
| Structures and enums | `Job`, `JobStatus`, `JobPriority`, `TaskType`, `SchedulerConfig`, `MetricsSnapshot` |
| Unions | `TaskInput` (text / calc / failure) |
| Pointers | Ring buffer of `Job*`, opaque handles, `Job **` registry, output parameters |
| malloc / calloc / realloc / free | `job_create` (calloc), registry growth (realloc), sieve (calloc), snapshots (malloc) |
| Function pointers | `TaskHandler` registry, `ExitPredicate` worker callback |
| Arrays and strings | Fixed buffers with `snprintf`, `strtok_r` parsing, name tables |
| Modular programming | One header + source per module, opaque structs |
| Circular queue | `job_queue.c` |
| File handling | fopen / fprintf / fgets / fread / fwrite / fclose, `rename` for atomic saves |
| Error handling | Result enums, checked allocations, I/O and pthread results, `strerror_r` |
| Pthreads, mutexes, condition variables | `worker_pool.c`, `scheduler.c`, `job_queue.c` |
| Resource cleanup | `goto` cleanup chains, join before free, verified with ASan/LSan |
| GCC + Makefile | Warning flags, sanitizer targets |

## 16. Limitations
- A running task cannot be stopped mid-execution; shutdown waits for it to finish.
- There is a single scheduler lock. That is fine for this scale, but a very large system would use finer-grained locks.
- Jobs stay in memory until exit, so memory grows with the number of jobs submitted.
- Retries are re-queued immediately, with no backoff delay.
- Worker utilization is busy time divided by thread lifetime, and it includes idle time while the menu waits for input.

## 17. Future enhancements
Exponential retry backoff, job cancellation and timeouts, crash recovery by replaying the journal, dependencies between jobs (DAG), per-priority worker reservation, a socket or IPC submission interface, and histograms of execution time.

## 18. Troubleshooting
- `pthread_barrier_t` unknown: build with the Makefile, which defines `_POSIX_C_SOURCE=200809L` (Linux/WSL).
- `cannot create logs/ or reports/`: run from a writable project root.
- Garbled colours: use `--no-color` or `export NO_COLOR=1`.
- `make tsan` fails to start on some WSL kernels because of ASLR settings: run `setarch -R make tsan`.
