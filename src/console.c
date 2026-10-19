#include "console.h"
#include "arch/irq.h"
#include "arch/systick.h"
#include "common.h"
#include "lib/timer.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mini_uart.h"
#include "mm.h"
#include "mm/frame_alloc.h"
#include "mm/kmalloc.h"
#include "arch/percpu.h"
#include "arch/smp.h"
#include "proc/task.h"
#include "utils.h"

void console_init(void) {

  // Print welcome banner
  uart_puts("\n");
  uart_puts("======================================\n");
  uart_puts("   KoraOS Debug Console\n");
  uart_puts("======================================\n");
  uart_puts("Type 'help' for available commands.\n");
}

void console_cmd_help(const char *args) {
  uart_puts("Available commands:\n");
  uart_puts("  help - Show available commands\n");
  uart_puts("  get_el - Get current Exception Level\n");
  uart_puts("  version - Print current KoraOS version\n");
  uart_puts("  irqs - Show interrupt counters and the system tick\n");
  uart_puts("  heap - Show kernel heap and page pool usage\n");
  uart_puts("  heaptest [rounds] - Stress the kernel heap (default 2000 rounds)\n");
  uart_puts("  tasks - List tasks with their state and kernel stack use\n");
  uart_puts("  cpus - List cores with their timer ticks and current task\n");
  uart_puts("Ctrl-T switches the serial line between this console and the\n");
  uart_puts("screen terminal (the shell).\n");
}

void console_cmd_get_el(const char *args) {
  printf("Current Exception Level: %d\n", get_el());
}

void console_cmd_version(const char *args) {
  printf("KoraOS version %s (%s)\n", KORAOS_VERSION, koraos_build_id);
}

// Interrupt health at a glance: the tick count should track the uptime (100 per
// second), and every connected IRQ is listed with how often it fired.
void console_cmd_irqs(const char *args) {
  uint64_t ms = timer_us() / 1000;
  printf("controller: %s\n", irq_controller());
  printf("systick: %lu ticks, uptime %lu.%03lu s\n",
         (unsigned long)systick_count(), (unsigned long)(ms / 1000),
         (unsigned long)(ms % 1000));
  printf("  IRQ        hits  source\n");
  for (unsigned irq = 0; irq < irq_lines(); irq++) {
    unsigned long hits = irq_hits(irq);
    const char *name = irq_name(irq);
    if (hits == 0 && name[0] == '\0') {
      continue;
    }
    printf("  %3u  %10lu  %s\n", irq, hits, name[0] ? name : "(no handler)");
  }
}

// Kernel heap usage per size class, page runs, and the frame pool under both.
void console_cmd_heap(const char *args) {
  (void)args;
  struct kmalloc_stats st;
  kmalloc_get_stats(&st);
  printf("  class  slabs   used / capacity\n");
  for (unsigned i = 0; i < KMALLOC_CLASSES; i++) {
    printf("  %5u  %5u  %5u / %u\n", (unsigned)st.classes[i].block_size,
           (unsigned)st.classes[i].slabs, (unsigned)st.classes[i].used,
           (unsigned)st.classes[i].capacity);
  }
  printf("large: %u allocations in %u pages\n", (unsigned)st.large_allocs,
         (unsigned)st.large_pages);
  printf("heap: %u live, %u allocated, %u failed, %u bad frees\n",
         (unsigned)st.live_allocs, (unsigned)st.total_allocs,
         (unsigned)st.failed_allocs, (unsigned)st.bad_frees);
  printf("pages: %u of %u free\n", (unsigned)frame_alloc_free_count(),
         (unsigned)frame_alloc_total_count());
}

// Run the deterministic heap stress; the seed follows the uptime so repeated
// runs cover different sequences. Runs with interrupts masked (the console is
// fed from the UART interrupt), so keep the round count modest.
void console_cmd_heaptest(const char *args) {
  unsigned rounds = 0;
  for (; *args >= '0' && *args <= '9'; args++) {
    rounds = rounds * 10 + (unsigned)(*args - '0');
    if (rounds > 100000) {
      rounds = 100000;
    }
  }
  if (rounds == 0) {
    rounds = 2000;
  }
  size_t free_before = frame_alloc_free_count();
  uint32_t seed = (uint32_t)timer_us() | 1u;
  int rc = kmalloc_stress(rounds, seed);
  if (rc != 0) {
    printf("heaptest: FAILED check %d with seed 0x%x\n", rc, (unsigned)seed);
    return;
  }
  printf("heaptest: %u rounds ok, seed 0x%x, pages free %u -> %u\n", rounds,
         (unsigned)seed, (unsigned)free_before, (unsigned)frame_alloc_free_count());
}

static void print_task(const task_t *t, void *ctx) {
  (void)ctx;
  static const char *const states[] = {"unused  ", "runnable", "blocked ", "exited  "};
  printf("  %3d  %s  %5u  %s%s\n", t->pid, states[t->state],
         (unsigned)task_kstack_peak(t), t->name, t->forbid ? " (forbid)" : "");
}

// Every task with its state and kernel stack high-water mark (bytes).
void console_cmd_tasks(const char *args) {
  (void)args;
  printf("  pid  state     stack  name\n");
  task_for_each(print_task, NULL);
  printf("kernel stack peak: %u of %u bytes\n", (unsigned)task_kstack_peak_max(),
         (unsigned)(KSTACK_PAGES * PAGE_SIZE));
  int holder = bkl_holder_pid();
  if (holder < 0) {
    printf("big kernel lock: free\n");
  } else {
    printf("big kernel lock: pid %d\n", holder);
  }
}

// Every core: online, its own timer interrupts, and what it is running.
void console_cmd_cpus(const char *args) {
  (void)args;
  printf("  cpu  state     ticks  running\n");
  for (unsigned i = 0; i < MAX_CPUS; i++) {
    const struct cpu *c = &cpus[i];
    bool up = i == 0 || __atomic_load_n(&c->online, __ATOMIC_ACQUIRE);
    if (!up) {
      printf("  %3u  offline\n", i);
      continue;
    }
    const struct task *t = c->curr;
    printf("  %3u  online  %7lu  %s\n", i, (unsigned long)c->ticks,
           t != NULL ? t->name : "-");
  }
  printf("cpus: %u online, this is cpu %u\n", smp_cores_online(), this_cpu()->id);
}

console_command_t commands[] = {
    {"help", "Show available commands", console_cmd_help},
    {"get_el", "Get the current Exception Level", console_cmd_get_el},
    {"version", "Get current KoraOS version", console_cmd_version},
    {"irqs", "Show interrupt counters and the system tick", console_cmd_irqs},
    {"heap", "Show kernel heap and page pool usage", console_cmd_heap},
    {"heaptest", "Stress the kernel heap", console_cmd_heaptest},
    {"tasks", "List tasks and kernel stack use", console_cmd_tasks},
    {"cpus", "List cores, their ticks and current tasks", console_cmd_cpus},
    {NULL, NULL, NULL},
};

// The first word names the command; the handler gets the rest of the line
// with leading spaces skipped.
void console_parse_and_execute(const char *input) {
  size_t len = 0;
  while (input[len] != '\0' && input[len] != ' ') {
    len++;
  }
  const char *args = input + len;
  while (*args == ' ') {
    args++;
  }
  for (int i = 0; commands[i].name != NULL; i++) {
    if ((size_t)strlen(commands[i].name) == len &&
        strncmp(input, commands[i].name, len) == 0) {
      commands[i].handler(args);
      return;
    }
  }

  uart_puts("Unknown command: '");
  uart_puts(input);
  uart_puts("'\nType 'help' for available commands.\n\n");
}

static char line[CONSOLE_MAX_CMD_LEN];
static int line_len;
static bool last_was_cr;

void console_prompt(void) {
  uart_puts("koraos> ");
  for (int i = 0; i < line_len; i++) {
    uart_putc(line[i]);
  }
}

void console_input(char c) {
  // Serial terminals send CR, LF or CR LF for Enter: treat the LF of a CR LF
  // pair as part of the same keypress.
  if (c == '\n' && last_was_cr) {
    last_was_cr = false;
    return;
  }
  last_was_cr = (c == '\r');

  if (c == '\r' || c == '\n') {
    uart_puts("\n");
    line[line_len] = '\0';
    if (line_len > 0) {
      console_parse_and_execute(line);
    }
    line_len = 0;
    console_prompt();
    return;
  }
  if (c == 8 || c == 127) {  // backspace / delete
    if (line_len > 0) {
      line_len--;
      uart_puts("\b \b");
    }
    return;
  }
  if (c >= 32 && c < 127 && line_len < CONSOLE_MAX_CMD_LEN - 1) {
    line[line_len++] = c;
    uart_putc(c);
  }
}

void console_log(const char *message) {
  uart_puts("[LOG] ");
  uart_puts(message);
  uart_puts("\n");
}
