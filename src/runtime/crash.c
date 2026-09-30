/*
 * The crash handler. Read include/inkwell/runtime/crash.h first - the rules this file follows
 * are stated there, and most of what looks roundabout below is one of them.
 *
 * _GNU_SOURCE rather than the _POSIX_C_SOURCE the rest of inkwell asks for: the register names
 * on x86-64 (REG_RIP) are behind it, and musl and glibc agree about that much.
 */
#define _GNU_SOURCE

#include "crash_internal.h"

#include "inkwell/base/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Darwin keeps ucontext_t in <sys/ucontext.h> and refuses <ucontext.h> without _XOPEN_SOURCE,
   which would hide half of what the rest of this file includes. */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#if defined(__linux__)
#include <link.h>
#endif

ptrdiff_t inkwell_crash_internal_backend_write(int fd, const char *data, size_t len) {
    return (ptrdiff_t)write(fd, data, len);
}

bool inkwell_crash_internal_path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

int inkwell_crash_internal_unlink(const char *path) {
    return unlink(path);
}

/* ---- what the handler is allowed to have ----------------------------------------------------
 *
 * Everything in this block is written from ordinary context and only ever read from the signal
 * handler. `sig_atomic_t` where the handler and the rest of the process both touch it; plain
 * arrays where the handler only reads and a torn read costs a garbled line rather than a fault.
 */
static bool g_installed;

/* Where this image is mapped, already formatted as "0x...". Read out of /proc/self/maps at
   install time, because the whole point of the number is to be subtracted from the addresses
   below and a PIE build's are meaningless without it. Empty when it could not be read, in which
   case the report says so rather than printing a zero that looks like an answer. */
static char g_load_base[32];

/*
 * Which binary this is, as the linker named it, in hex: the ELF GNU build-id note on Linux, the
 * Mach-O LC_UUID on a Mac. Read at install for the reason the load base is.
 *
 * A version says which *release* a report came from; this says which *file*, and they are not
 * the same question. Two builds of one tag differ by compiler and flags, a local build carries
 * the version of the tree it came from, and the address arithmetic the report exists for is
 * only right against the exact binary that faulted. A symbol server matches on this and nothing
 * else. Empty when the binary carries none - a link without `--build-id`, a system this has no
 * reader for - and the line is then left out rather than printed with nothing after it.
 *
 * 20 bytes is SHA-1, what `--build-id` writes by default; a longer id is cut at the buffer and a
 * reader who needs the rest has the binary.
 */
static char g_build_id[2U * 32U + 1U];

/* How much address space the image spans from its load base, as "0x...": the extent of its
   loaded segments. What turns "an address past the base" into "an address inside this binary"
   for a reader holding several images - a symbol server asks for it beside the id. */
static char g_image_size[32];

/* The probe pipe. Writing an address to it reports EFAULT instead of faulting, which is what
   makes walking a broken stack survivable. Both ends non-blocking so a probe can never wait. */
static int g_probe_fd[2] = {-1, -1};

/*
 * The stack the handler runs on.
 *
 * Without this a SIGSEGV raised by *exhausting* the stack cannot be reported at all: the kernel
 * builds the signal frame on the stack that has just run out, fails, and kills the process
 * without ever entering the handler - so the one class of crash where a backtrace is most
 * valuable is the class that silently produced no file. `sigaltstack()` hands the kernel
 * somewhere else to put that frame, and SA_ONSTACK is what asks for it.
 *
 * Sized as a constant rather than from SIGSTKSZ, which stopped being a compile-time constant in
 * glibc 2.34 (it reads sysconf(_SC_SIGSTKSZ) now) and so cannot size a static array. 64 KiB is
 * far more than this handler needs - its deepest frame holds a 64-byte probe sink and a 32-byte
 * digit buffer - and being generous here costs bss on a device with a gigabyte of it.
 */
#define INKWELL_CRASH_SIGSTACK_SIZE (64U * 1024U)
static char g_sig_stack[INKWELL_CRASH_SIGSTACK_SIZE];

/* The signals worth catching: the four faults and the abort that an assert or a libc check
   raises. SIGQUIT and the rest are ways of being asked to stop, which is not a crash. */
static const int k_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

/* ---- probing memory -------------------------------------------------------------------------
 *
 * The trick this module turns on. write(2) validates its buffer in the kernel and answers
 * EFAULT for a page that is not there, so asking the pipe whether an address is readable costs
 * a syscall and cannot fault - where the obvious `*(uint64_t *)fp` costs a second SIGSEGV
 * inside the handler for the first one.
 *
 * Whatever is written has to come straight back out or the pipe fills and the next probe blocks
 * - hence the drain, and hence the non-blocking ends as the belt to that brace.
 */
static bool crash_readable(const void *address, size_t len) {
    if (g_probe_fd[0] < 0 || g_probe_fd[1] < 0 || address == NULL) {
        return false;
    }
    const ssize_t written = write(g_probe_fd[1], address, len);
    if (written < 0) {
        return false;
    }
    char sink[64];
    size_t drained = 0U;
    while (drained < (size_t)written) {
        const ssize_t got = read(g_probe_fd[0], sink, sizeof sink);
        if (got <= 0) {
            break;
        }
        drained += (size_t)got;
    }
    return (size_t)written == len;
}

/* ---- the fault itself ------------------------------------------------------------------------ */

static const char *crash_signal_name(int signal_number) {
    switch (signal_number) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

/*
 * The program counter and frame pointer out of the interrupted context.
 *
 * Two architectures because those are the two this project builds for - aarch64 is the Brick and
 * x86-64 is every test run and every container build - and anything else gets a report with no
 * stack rather than a build failure. That is the right trade: the notes and the log tail are
 * most of the value, and they are portable.
 */
static bool crash_registers(void *ucontext, uint64_t *pc, uint64_t *fp) {
    if (ucontext == NULL) {
        return false;
    }
    const ucontext_t *uc = (const ucontext_t *)ucontext;
#if defined(__APPLE__) && defined(__aarch64__)
    *pc = (uint64_t)uc->uc_mcontext->__ss.__pc;
    *fp = (uint64_t)uc->uc_mcontext->__ss.__fp;
    return true;
#elif defined(__APPLE__) && defined(__x86_64__)
    *pc = (uint64_t)uc->uc_mcontext->__ss.__rip;
    *fp = (uint64_t)uc->uc_mcontext->__ss.__rbp;
    return true;
#elif defined(__aarch64__)
    *pc = (uint64_t)uc->uc_mcontext.pc;
    /* x29 is the frame pointer under AAPCS64. */
    *fp = (uint64_t)uc->uc_mcontext.regs[29];
    return true;
#elif defined(__x86_64__)
    *pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    *fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
    return true;
#else
    (void)uc;
    (void)pc;
    (void)fp;
    return false;
#endif
}

/*
 * Walk the frame pointers.
 *
 * Both supported architectures lay a frame out the same way once a frame pointer is kept: the
 * saved pointer to the caller's frame first, the return address next. The chain is trusted only
 * as far as it stays plausible - readable, aligned, and climbing - because a corrupted stack is
 * the ordinary case here rather than the exception, and a walk that believed it would print
 * whatever happened to be in memory as though it were a call chain.
 *
 * What makes the whole thing safe is the probe rather than the checks: the checks stop nonsense
 * being printed, the probe stops the handler dying.
 */
static void crash_write_frames(int fd, uint64_t fp) {
    uint64_t previous = 0U;
    for (unsigned depth = 0U; depth < 32U; ++depth) {
        /*
         * Eight, not sixteen. AAPCS64 really does keep the stack sixteen-aligned throughout, so
         * an aarch64 x29 is always 0 mod 16 and the tighter test looks like the safer one - but
         * x86-64 only promises that alignment at a call boundary, and a frame there is as often
         * 8 mod 16 as not. Asked for sixteen, the walk ended after a single frame on every host
         * build: one plausible address, printed without complaint, where a chain was expected.
         * A frame pointer cannot be less than pointer-aligned, and that is the whole of what is
         * knowable here.
         */
        if (fp == 0U || (fp & (uint64_t)(sizeof(uint64_t) - 1U)) != 0U || fp <= previous) {
            return;
        }
        if (!crash_readable((const void *)(uintptr_t)fp, 2U * sizeof(uint64_t))) {
            return;
        }
        const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;
        const uint64_t next = frame[0];
        const uint64_t return_address = frame[1];
        if (return_address == 0U) {
            return;
        }
        inkwell_crash_internal_puts(fd, " #");
        char index[8];
        const size_t len =
            inkwell_crash_internal_format_unsigned(index, sizeof index, depth, 10U, 2U);
        inkwell_crash_internal_write(fd, index, len);
        inkwell_crash_internal_puts(fd, " ");
        inkwell_crash_internal_write_address(fd, return_address);
        inkwell_crash_internal_puts(fd, "\n");
        previous = fp;
        fp = next;
    }
}

/* ---- the handler ----------------------------------------------------------------------------- */

static void crash_handler(int signal_number, siginfo_t *info, void *ucontext) {
    /*
     * Stand every handler down before doing anything else.
     *
     * Two things come out of it. A fault *inside* this function then kills the process the
     * ordinary way instead of re-entering here forever, which is the failure mode a crash
     * handler is most likely to have. And the re-raise at the bottom reaches the default
     * disposition, so the process dies of the signal it was actually given - which is what keeps
     * the exit status honest for whatever started us.
     */
    for (size_t i = 0U; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
        signal(k_signals[i], SIG_DFL);
    }

    if (inkwell_crash_internal_path()[0] != '\0') {
        const int fd =
            open(inkwell_crash_internal_path(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            CrashFault fault = {
                .signal_number = signal_number,
                .signal_name = crash_signal_name(signal_number),
                .has_info = info != NULL,
                .code = info != NULL ? (int64_t)info->si_code : 0,
                .fault_address = info != NULL ? (uint64_t)(uintptr_t)info->si_addr : 0U,
                .load_base = g_load_base,
                .build_id = g_build_id,
                .image_size = g_image_size,
            };
            fault.have_registers = crash_registers(ucontext, &fault.pc, &fault.fp);
            inkwell_crash_internal_write_report(fd, &fault, crash_write_frames);
            /* The Brick's card is mounted `sync`, so this is close to free there - and on
               anything else it is what makes the report survive a device that loses power
               between the fault and the next clean unmount. */
            (void)fsync(fd);
            (void)close(fd);
        }
    }

    raise(signal_number);
}

/* ---- install --------------------------------------------------------------------------------- */

/*
 * The first mapping of this process, as text.
 *
 * /proc/self/maps opens with the executable's own first segment, so its start address is the
 * load base every address in the report has to be measured from. Read here, in ordinary context,
 * because parsing is exactly what the handler may not do - by the time it runs this is a string
 * to be written out and nothing more.
 */
static void crash_capture_load_base(void) {
    g_load_base[0] = '\0';
#if defined(__APPLE__)
    /* No /proc on Darwin. Image 0 is the executable, and its Mach header is where it was
       mapped. */
    (void)snprintf(g_load_base, sizeof g_load_base, "%p", (const void *)_dyld_get_image_header(0));
#else
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == NULL) {
        return;
    }
    char line[256];
    if (fgets(line, sizeof line, maps) != NULL) {
        /*
         * The field is the hex digits up to the first dash, and it is checked rather than
         * trusted. An address is sixteen digits at the very most, so anything longer - or
         * carrying anything that is not a digit - is a /proc that does not look like the one
         * this expects, and an unset base prints an honest "no base" where a copied prefix
         * would print a number the reader would go on to subtract.
         */
        size_t len = 0U;
        while (len < sizeof g_load_base - 3U && line[len] != '\0' && line[len] != '-') {
            const char c = line[len];
            const bool hex =
                (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) {
                len = 0U;
                break;
            }
            ++len;
        }
        if (len > 0U && line[len] == '-') {
            g_load_base[0] = '0';
            g_load_base[1] = 'x';
            memcpy(g_load_base + 2, line, len);
            g_load_base[len + 2U] = '\0';
        }
    }
    (void)fclose(maps);
#endif
}

static void crash_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    static const char k_hex[] = "0123456789abcdef";
    size_t at = 0U;
    for (size_t i = 0U; i < len && at + 2U < out_len; ++i) {
        out[at++] = k_hex[bytes[i] >> 4U];
        out[at++] = k_hex[bytes[i] & 0x0fU];
    }
    out[at] = '\0';
}

#if defined(__linux__)
/*
 * The executable is the first object dl_iterate_phdr() visits; its notes are in PT_NOTE.
 *
 * Its extent is measured from where its first PT_LOAD is mapped - that segment's address rounded
 * down to its alignment, which is the start of the mapping the load base was read from - to the
 * end of its last. From the lowest segment rather than from zero: a position-independent binary
 * is linked at 0 and the two agree, but a fixed-address one is linked at 0x400000 or so, and
 * measured from zero its "size" would be its end address.
 */
static int crash_find_build_id(struct dl_phdr_info *info, size_t size, void *userdata) {
    (void)size;
    (void)userdata;
    uint64_t start = UINT64_MAX;
    uint64_t extent = 0U;
    for (size_t i = 0U; i < (size_t)info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *const phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        const uint64_t align = phdr->p_align > 1U ? (uint64_t)phdr->p_align : 1U;
        const uint64_t mapped = (uint64_t)phdr->p_vaddr & ~(align - 1U);
        if (mapped < start) {
            start = mapped;
        }
        if ((uint64_t)(phdr->p_vaddr + phdr->p_memsz) > extent) {
            extent = (uint64_t)(phdr->p_vaddr + phdr->p_memsz);
        }
    }
    if (extent > start) {
        (void)snprintf(g_image_size, sizeof g_image_size, "0x%llx",
                       (unsigned long long)(extent - start));
    }
    for (size_t i = 0U; i < (size_t)info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *const phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_NOTE) {
            continue;
        }
        const uint8_t *at = (const uint8_t *)(uintptr_t)(info->dlpi_addr + phdr->p_vaddr);
        const uint8_t *const end = at + phdr->p_memsz;
        while (at + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *const note = (const ElfW(Nhdr) *)(const void *)at;
            const size_t name_len = ((size_t)note->n_namesz + 3U) & ~(size_t)3U;
            const size_t desc_len = ((size_t)note->n_descsz + 3U) & ~(size_t)3U;
            const uint8_t *const name = at + sizeof *note;
            const uint8_t *const desc = name + name_len;
            if (desc + desc_len > end) {
                break;
            }
            /* NT_GNU_BUILD_ID, spelled as its value: musl's <elf.h> has not always named it. */
            if (note->n_type == 3U && note->n_namesz == 4U && memcmp(name, "GNU", 4U) == 0) {
                crash_hex(desc, note->n_descsz, g_build_id, sizeof g_build_id);
                return 1;
            }
            at = desc + desc_len;
        }
    }
    return 1; /* only the first object is this binary; the rest are its libraries */
}
#endif

static void crash_capture_build_id(void) {
    g_build_id[0] = '\0';
    g_image_size[0] = '\0';
#if defined(__APPLE__)
    const struct mach_header_64 *const header =
        (const struct mach_header_64 *)(const void *)_dyld_get_image_header(0);
    if (header == NULL || header->magic != MH_MAGIC_64) {
        return;
    }
    /* The extent is measured from __TEXT, which is where the header - the load base - sits;
       __PAGEZERO is below it and is not part of the image anybody resolves against. */
    uint64_t text = 0U;
    uint64_t extent = 0U;
    bool have_text = false;
    const uint8_t *at = (const uint8_t *)(header + 1);
    for (uint32_t i = 0U; i < header->ncmds; ++i) {
        const struct load_command *const command = (const struct load_command *)(const void *)at;
        if (command->cmd == LC_UUID) {
            const struct uuid_command *const uuid = (const struct uuid_command *)(const void *)at;
            crash_hex(uuid->uuid, sizeof uuid->uuid, g_build_id, sizeof g_build_id);
        } else if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *const segment =
                (const struct segment_command_64 *)(const void *)at;
            if (strncmp(segment->segname, "__TEXT", sizeof segment->segname) == 0) {
                text = segment->vmaddr;
                have_text = true;
            }
            if (strncmp(segment->segname, "__PAGEZERO", sizeof segment->segname) != 0 &&
                segment->vmaddr + segment->vmsize > extent) {
                extent = segment->vmaddr + segment->vmsize;
            }
        }
        at += command->cmdsize;
    }
    if (have_text && extent > text) {
        (void)snprintf(g_image_size, sizeof g_image_size, "0x%llx",
                       (unsigned long long)(extent - text));
    }
#elif defined(__linux__)
    (void)dl_iterate_phdr(crash_find_build_id, NULL);
#endif
}

int inkwell_crash_install(const struct inkwell_crash_config *config) {
    /* A rejected configuration leaves the previous report and handler intact. */
    const int prepared = inkwell_crash_internal_prepare(config);
    if (prepared != 0) {
        return prepared;
    }

    /* The probe pipe and the load base are properties of the process rather than of the
       directory, so they are taken once however many times this is called - otherwise a second
       install would leak a pair of descriptors for nothing. */
    if (g_probe_fd[0] < 0 && inkwell_fd_pipe(g_probe_fd) != 0) {
        /* No probe means no safe stack walk. Everything else in a report still works, so this
           is not a reason to refuse the install - crash_readable() simply always says no. */
        g_probe_fd[0] = -1;
        g_probe_fd[1] = -1;
    }
    if (!g_installed) {
        crash_capture_load_base();
        crash_capture_build_id();
    }

    /*
     * Before the handlers, because SA_ONSTACK below is a reference to it. A failure is not fatal
     * to the install: the handler then runs on the ordinary stack, which is what it did before
     * this existed and is right for every crash except the overflow.
     *
     * On every install rather than the first, unlike the handlers: the alternate stack belongs to
     * a thread, not to the process, and macOS clears it in a forked child where Linux keeps it.
     * A child that re-aims its reports with a second install is then covered on both, which is
     * exactly what the exhausted-stack case does.
     */
    stack_t alt;
    memset(&alt, 0, sizeof alt);
    alt.ss_sp = g_sig_stack;
    alt.ss_size = sizeof g_sig_stack;
    alt.ss_flags = 0;
    const bool have_alt_stack = sigaltstack(&alt, NULL) == 0;

    if (g_installed) {
        return 0;
    }

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART | (have_alt_stack ? SA_ONSTACK : 0);
    /* Block the other fault signals while one is being reported, so two arriving together are
       one report rather than two interleaved into the same file. */
    sigemptyset(&action.sa_mask);
    for (size_t i = 0U; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
        sigaddset(&action.sa_mask, k_signals[i]);
    }

    for (size_t i = 0U; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
        if (sigaction(k_signals[i], &action, NULL) != 0) {
            return -errno;
        }
    }

    g_installed = true;
    return 0;
}

void inkwell_crash_write_report(int fd, int signal_number) {
    const CrashFault fault = {
        .signal_number = signal_number,
        .signal_name = crash_signal_name(signal_number),
        .load_base = g_load_base,
        .build_id = g_build_id,
        .image_size = g_image_size,
    };
    inkwell_crash_internal_write_report(fd, &fault, NULL);
}
