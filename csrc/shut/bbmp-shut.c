/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-shut: start a replayer shut in by Landlock, where bubblewrap can't start
 * (inside a Flatpak, which has no user namespaces to give it).
 *
 *   bbmp-shut --abi
 *       prints the kernel's Landlock ABI version (0: no Landlock) and exits
 *   bbmp-shut [--rx PATH]... [--ro PATH]... [--rw PATH]... [--tmp DIR] [--data BYTES]
 *             -- PROGRAM ARGS...
 *       --rx  may be read and its programs run    (the system, the replayers)
 *       --ro  may be read                         (the song, its folder)
 *       --rw  may be read, written, made, removed (its own temporary folder, a
 *             folder to unpack into)
 *       --tmp its own temporary folder: --rw, the folder it starts in, and TMPDIR
 *       --data  its RLIMIT_DATA in bytes
 *
 * Then, as bubblewrap's --new-session and --die-with-parent: a session of its own
 * (no terminal to push keys into), killed when the thread that started it ends (the
 * player's spawner thread lives as long as the player). Then no_new_privs; the Landlock ruleset (all the file rights this kernel knows are
 * handled, so anything not named above is refused; on ABI 4 and later TCP bind and
 * connect are refused; on ABI 6 and later abstract Unix sockets and signals stay
 * inside; on ABI 9 and later so do Unix sockets by path); a seccomp filter that
 * refuses socket() whatever the ABI (Landlock never covers UDP, raw or netlink
 * sockets), io_uring (it can open sockets past the filter) and 32-bit system calls;
 * and execve. A path that doesn't exist is left out. If Landlock is there and can't
 * be applied, the program is not started (exit 126).
 *
 * A C program and not ctypes in the player's preexec_fn: the player is threaded, and
 * Python code between fork and exec in a threaded program can deadlock; this is a few
 * system calls, and it costs nothing when a song is seeked (each seek starts the
 * helper again).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#ifndef LANDLOCK_ACCESS_FS_RESOLVE_UNIX
#define LANDLOCK_ACCESS_FS_RESOLVE_UNIX (1ULL << 16)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#define LANDLOCK_RULE_NET_PORT 2
#endif
#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif

/* the ruleset's attributes as of ABI 6; older kernels get the first fields only */
struct ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
    uint64_t scoped;
};

#define FS_V1 ((1ULL << 13) - 1)                /* execute .. make_sym */
#define FILE_RIGHTS (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | \
                     LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | \
                     LANDLOCK_ACCESS_FS_IOCTL_DEV)
#define RO (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR)
#define RX (RO | LANDLOCK_ACCESS_FS_EXECUTE)
#define RW (RO | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | \
            LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
            LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | \
            LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_MAKE_FIFO | \
            LANDLOCK_ACCESS_FS_REFER)

static int abi_version(void) {
    long v = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return v < 0 ? 0 : (int)v;
}

static void die(const char* what) {
    fprintf(stderr, "bbmp-shut: %s: %s\n", what, strerror(errno));
    exit(126);
}

static uint64_t handled_fs(int abi) {
    uint64_t fs = FS_V1;
    if (abi >= 2) fs |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi >= 5) fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    if (abi >= 9) fs |= LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
    return fs;
}

static void allow(int ruleset, const char* path, uint64_t access, uint64_t handled) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == EACCES) return;
        die(path);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) die(path);
    if (!S_ISDIR(st.st_mode)) access &= FILE_RIGHTS;      /* the rest are a folder's */
    struct landlock_path_beneath_attr rule = {.allowed_access = access & handled,
                                              .parent_fd = fd};
    if (rule.allowed_access &&
        syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) != 0)
        die(path);
    close(fd);
}

#if defined(__x86_64__)
#define ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ARCH AUDIT_ARCH_AARCH64
#endif

/* socket() refused (EACCES), io_uring refused (ENOSYS), and on x86-64 the x32 and
 * 32-bit calls (socketcall would pass the filter) refused. */
static void no_sockets(void) {
#ifdef ARCH
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef __x86_64__
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),    /* x32 */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
#endif
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {.len = sizeof filter / sizeof filter[0], .filter = filter};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) die("seccomp");
#endif
}

static void usage(void) {
    fprintf(stderr, "usage: bbmp-shut --abi | [--rx|--ro|--rw PATH]... [--tmp DIR] "
                    "[--data BYTES] -- PROGRAM ARGS...\n");
    exit(126);
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--abi") == 0) {
        printf("%d\n", abi_version());
        return 0;
    }
    int i = 1;
    for (; i < argc && strcmp(argv[i], "--") != 0; i += 2)
        if (i + 1 >= argc) usage();
    if (i + 1 >= argc) usage();                 /* no program */
    char** program = argv + i + 1;

    const char* tmp = NULL;
    for (int j = 1; j < i; j += 2) {
        if (strcmp(argv[j], "--tmp") == 0) {
            tmp = argv[j + 1];
        } else if (strcmp(argv[j], "--data") == 0) {
            rlim_t n = strtoull(argv[j + 1], NULL, 10);
            struct rlimit lim = {n, n};
            if (setrlimit(RLIMIT_DATA, &lim) != 0) die("RLIMIT_DATA");
        } else if (strcmp(argv[j], "--rx") && strcmp(argv[j], "--ro") && strcmp(argv[j], "--rw")) {
            usage();
        }
    }
    if (tmp && (chdir(tmp) != 0 || setenv("TMPDIR", tmp, 1) != 0)) die(tmp);
    pid_t parent = getppid();
    setsid();                                   /* fails only for a group leader: fine */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) die("PR_SET_PDEATHSIG");
    if (getppid() != parent) return 126;        /* the player ended before that */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) die("no_new_privs");

    int abi = abi_version();
    if (abi >= 1) {
        struct ruleset_attr attr = {.handled_access_fs = handled_fs(abi)};
        size_t size = offsetof(struct ruleset_attr, handled_access_net);
        if (abi >= 4) {
            attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
            size = offsetof(struct ruleset_attr, scoped);
        }
        if (abi >= 6) {
            attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
            size = sizeof attr;
        }
        int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, size, 0);
        if (ruleset < 0) die("landlock_create_ruleset");
        for (int j = 1; j < i; j += 2) {
            uint64_t access = strcmp(argv[j], "--rx") == 0 ? RX
                            : strcmp(argv[j], "--ro") == 0 ? RO
                            : strcmp(argv[j], "--rw") == 0 ? RW
                            : strcmp(argv[j], "--tmp") == 0 ? RW : 0;
            if (access) allow(ruleset, argv[j + 1], access, attr.handled_access_fs);
        }
        if (syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) die("landlock_restrict_self");
        close(ruleset);
    }
    no_sockets();
    execvp(program[0], program);
    die(program[0]);
    return 126;
}
