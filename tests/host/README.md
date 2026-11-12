# Socket IPC regression tests

Run on Linux with a C++20 compiler, Bash, and GNU `timeout`:

```sh
git submodule update --init --recursive
bash tests/host/run.sh
```

The tests compile the production `socket_utils.cpp` and use real Unix sockets,
`SCM_RIGHTS`, and signals. Only the Android logging API is stubbed. Neither the
Android SDK nor Gradle is needed. Build output is temporary and removed on exit.

AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default. Set
`SANITIZE=0` for an unsanitized build, or `CXX` to choose another compiler. In a
restricted environment where LeakSanitizer cannot inspect threads, run:

```sh
ASAN_OPTIONS=detect_leaks=0 bash tests/host/run.sh
```

The repeated-string test independently checks balanced C++ allocations; the FD
tests check `/proc/self/fd` before and after transfers. These checks still run
with LeakSanitizer disabled. Individual test names can be passed as arguments.

Coverage includes string framing, embedded NUL bytes, fragmented/short reads,
overflowing lengths, missing large payloads, repeated reads, valid FD ownership,
missing/extra/truncated descriptors, wrong ancillary types, EOF, syscall failure,
and recovery from `EINTR`. The four-byte FD payload matches the Rust daemon's
`passfd` protocol. Invalid FD frames are rejected, and all descriptors installed
by the rejected message are closed.

These tests validate Linux IPC and resource handling, not Android/SELinux policy,
Zygote injection, or device-specific module compatibility. The existing Android
CI build and real-device testing remain necessary before shipping a module ZIP.
