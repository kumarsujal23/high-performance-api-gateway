# High-Performance API Gateway (C++20, Linux)

A reverse-proxy / API gateway built from scratch on raw POSIX sockets +
epoll, written to be **explainable component-by-component** in an
SWE/HFT-style interview — not to compete with nginx/Envoy on features.

Every subsystem in the pipeline below is a separate, independently unit
tested C++ class with no hidden magic:

```
Client
  │
  ▼
TCP Listener (non-blocking, SO_REUSEPORT)
  │
  ▼
epoll Event Loop (one per worker thread)
  │
  ▼
HTTP/1.1 Parser (incremental, resumable state machine)
  │
  ▼
Rate Limiter (token bucket, per client IP)
  │
  ▼
LRU Cache  ──HIT──► HTTP Response
  │ MISS
  ▼
Router (longest-prefix match)
  │
  ▼
Load Balancer (round robin | least connections)
  │
  ▼
Connection Pool (persistent backend TCP connections)
  │
  ▼
Retry (exponential backoff + full jitter) ↔ Circuit Breaker (per backend)
  │
  ▼
Backend Service
```

Cutting across all of this: structured JSON logs, per-request IDs,
Prometheus-style metrics (RPS, active connections, errors, p50/p95/p99
latency), and a background health checker.

## What this is / is not

Implemented: everything in the pipeline above, HTTP/1.1 only (no chunked
transfer-encoding), strict request framing checks, path-segment-aware routing,
health checks, structured logging, request IDs,
metrics, YAML-subset config, round robin + least-connections load
balancing, timeouts, bounded retries with jittered backoff, a circuit
breaker per backend, connection pooling, and a from-scratch benchmark tool.

Deliberately **not** implemented (per project scope): HTTP/2, HTTP/3/QUIC,
TLS, chunked encoding, Kubernetes/service-mesh integration, OAuth, a custom
TCP stack. These are well-understood, well-documented pieces of standard
infrastructure that would add size without adding to what this project
demonstrates.

## Repository layout

```
include/ , src/          headers / implementations, one subsystem per folder
  core/                  EventLoop (epoll wrapper), Connection state, Gateway (orchestration)
  http/                  HttpRequest/HttpResponse, incremental HttpParser
  routing/                Router (longest-prefix match), LoadBalancer (RR / least-conn)
  network/               TcpServer, ConnectionPool, HealthChecker
  resilience/             RateLimiter (token bucket), CircuitBreaker, RetryPolicy
  cache/                  LRUCache (thread-safe, TTL)
  observability/          Logger (structured JSON), Metrics (atomics + latency histogram), RequestId
  config/                 GatewayConfig + minimal YAML-subset parser
tests/                    GoogleTest unit + integration tests (one file per subsystem)
benchmarks/               Dependency-free HTTP load generator
backends/                 Minimal test backend (supports injected latency/failure for chaos testing)
config/gateway.yaml       Example configuration
```

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

This produces three runtime binaries in `build/`:
- `gateway` — the API gateway itself
- `backend_server` — a minimal test backend (`./backend_server <port> [--latency-ms N] [--fail-rate P] [--name NAME]`)
- `gateway_benchmark` — the load generator

With `BUILD_TESTS=ON`, it also produces the `gateway_tests` test binary.

Tests (fetches GoogleTest via CMake `FetchContent`, needs network access on
first configure):

```bash
cmake -S . -B build-tests -DBUILD_TESTS=ON
cmake --build build-tests --parallel
cd build-tests && ctest --output-on-failure
```

Sanitizer build (ASan + UBSan), all 40 tests pass clean with zero warnings:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
cd build-asan && ctest --output-on-failure
```

## Running it

```bash
# Terminal 1: a couple of backend instances
./build/backend_server 9001 --name svc-a
./build/backend_server 9002 --name svc-b

# Terminal 2: the gateway (config points routes at those backends)
./build/gateway config/gateway.yaml

# Terminal 3
curl http://localhost:8080/api/users/42
curl http://localhost:8080/metrics       # Prometheus-format counters + latency percentiles
curl http://localhost:8080/gateway/health
```

`config/gateway.yaml` defines listen address/port, worker thread count,
rate-limit/cache/circuit-breaker/retry/timeout knobs, one or more named
backend groups (each with a load-balancing strategy), and path-prefix
routes mapping to those groups. See the file for the full schema — it's
intentionally small.

## Design tradeoffs (things I'd explain in an interview)

**Thread-per-core with SO_REUSEPORT, not a single shared event loop.**
Each worker thread owns its own epoll instance *and* its own listening
socket, bound to the same port via `SO_REUSEPORT`. The kernel load-balances
incoming connections across those sockets, so there's no shared accept()
lock and no cross-thread hand-off of file descriptors — a connection is
handled start-to-finish by the worker that accepted it. This is the same
pattern nginx and Envoy use for multi-core scaling.

**Backend I/O is synchronous-with-timeout, not fully async.** When a
worker thread has a complete client request ready to proxy, it calls the
backend using blocking sockets managed with `poll()` and timeouts, right there
on the same thread that runs that worker's epoll loop. This means a single
proxied request briefly blocks *that worker's* other client connections for
the duration of the backend round trip (bounded by `backend_read_timeout_ms`).
This was a deliberate choice: it keeps the entire request-handling path
linear and easy to read/step-through/explain, instead of a second nested
state machine for backend sockets registered in the same epoll instance.
The benchmark below shows the real cost of this choice — going from 1 to 4
worker threads on the same box roughly **4x's throughput and cuts p50
latency by ~4x**, which is exactly what you'd expect if each worker is
occasionally blocked waiting on a backend. A fully async version would
register backend sockets with the event loop too and drive them through
their own read/write state machine, at the cost of a noticeably more
complex `Connection`-like class per backend leg. I'd implement that as the
natural next step if this were headed to production.

**Level-triggered epoll, not edge-triggered.** Simpler to reason about
(no risk of missing a wakeup because you didn't drain a socket to EAGAIN),
at the cost of a few more `epoll_wait` wakeups under heavy pipelining.
Given the synchronous backend-I/O tradeoff above, that overhead isn't the
bottleneck.

**Token bucket over sliding-window log for rate limiting.** O(1) memory
per client key and allows short bursts up to `capacity` while enforcing a
steady-state `refill_per_second` — the standard tradeoff vs. a sliding
window's better burst-smoothing at higher memory cost.

**Full jitter for retry backoff**, per the AWS Architecture Blog formula
(`sleep = random(0, min(cap, base * 2^attempt))`), specifically to avoid
retry storms where many clients back off in lockstep and then retry at the
same instant — worse than either fixed delay or no jitter at all under
concurrent load.

**Per-backend circuit breakers**, not one breaker per backend *group*. A
single failing instance behind a load balancer shouldn't cause the gateway
to fail-fast requests that would've been routed to its healthy siblings.

**Hand-rolled config parser, not a YAML library dependency.** The config
schema is small and fixed-shape; a full YAML parser would be a heavyweight
dependency for a handful of nested lists and key/value pairs. The parser
(`src/config/Config.cpp`) explicitly documents that it's a *subset*, not
general YAML.

## Testing

- **Unit tests**: `HttpParser` (incremental delivery, pipelining, malformed
  input, invalid framing, unsupported chunked encoding), `Router`/`LoadBalancer` (longest-prefix match, round robin,
  least-connections, unhealthy-backend skipping), `RateLimiter` (bucket
  exhaustion, per-key isolation, refill), `LRUCache` (eviction order, TTL
  expiry, recency refresh), `CircuitBreaker` (all three state transitions,
  half-open trial limiting), `RetryPolicy` (backoff bounds and growth).
- **Integration tests**: spin up a *real* `Gateway` and a *real* raw-socket
  test backend over loopback TCP (no internal test hooks — only the public
  `Gateway` API), and verify the full accept → parse → route → proxy →
  respond pipeline, including the 502/503 backend-failure path and 404
  routing.
- 44/44 tests pass, including under `-fsanitize=address,undefined`.

## Benchmarking

`gateway_benchmark` is a small, dependency-free load generator (not a
wrk/hey replacement — deliberately simple so the measurement methodology
itself is inspectable) that opens N connections and fires GET requests for
a fixed duration, recording per-request latency:

```bash
./build/gateway_benchmark <host> <port> <path> <connections> <duration_s> [--no-keepalive]
```

**Real numbers from this run** (single container, 4 vCPU equivalent,
gateway + backend + benchmark client all colocated on loopback — so these
are directional, not representative of production hardware; re-run
`gateway_benchmark` on your own machine for numbers you can cite):

| Configuration                                   | Throughput   | p50      | p95      | p99      |
|--------------------------------------------------|-------------:|---------:|---------:|---------:|
| 1 connection, keep-alive, 4 workers               |  424 req/s   |  2.4 ms  |  4.4 ms  |  4.4 ms  |
| 4 connections, keep-alive, 4 workers              | 1409 req/s   |  2.5 ms  |  6.8 ms  |  9.1 ms  |
| 32 connections, keep-alive, 4 workers             | 1813 req/s   | 17.1 ms  | 29.7 ms  | 42.5 ms  |
| 32 connections, **no** keep-alive, 4 workers      | 1787 req/s   | 15.8 ms  | 41.7 ms  | 49.7 ms  |
| 32 connections, keep-alive, **1 worker**          |  439 req/s   | 72.2 ms  | 86.4 ms  | 135.4 ms |

Takeaways, and why they follow from the design:
- **Worker count matters a lot** (1813 vs 439 req/s, 4x): confirms the
  synchronous-backend-I/O tradeoff above — with 1 worker, every proxied
  request briefly stalls *all* other connections on that single thread;
  with 4 workers the stalls are spread out and mostly overlap.
- **Keep-alive vs. no keep-alive was roughly a wash at 32 connections** in
  this environment (1813 vs 1787 req/s) — at this concurrency and on
  loopback, connection setup cost isn't the bottleneck (backend round-trip
  time dominates); the gap would be expected to widen over a real network
  with non-trivial RTT/handshake cost, or at lower concurrency where
  handshake overhead is a bigger fraction of total time.
- Throughput does not scale linearly from 4→32 connections (1409 → 1813, a
  1.3x for 8x the connections) — consistent with 4 worker threads being
  the saturating resource once concurrency exceeds the worker count.

No numbers in this table are invented — they're outputs of the exact
command line shown above run against this exact build in this environment.
