/*
 * System statistics for the console
 *
 * The task list and the CPU load are based on the FreeRTOS run time stats,
 * so CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS has to be enabled (it is, see
 * sdkconfig). The run time counter is driven by the ESP timer (1 MHz), so
 * its unit is the same as the one used by esp_timer_get_time().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "sys_stats.h"

static const char *TAG = "sys_stats";

/* Length of the run time of the CPU measurement, in milliseconds */
#define CPU_SAMPLE_MS 1000

/* Maximum number of tasks taken into account */
#define TASKS_MAX 64

/* Length of the task name buffer */
#define TASK_NAME_LEN 32

typedef struct {
    TaskHandle_t handle;
    char         name[TASK_NAME_LEN];
    uint32_t     runtime_us;
} task_runtime_t;

/* The CPU usage of a task during the measurement window */
typedef struct {
    char     name[TASK_NAME_LEN];
    double   percent;
    uint32_t runtime_ms;
} task_usage_t;

static const char *state_str(eTaskState state)
{
    switch (state) {
    case eRunning:      return "running";
    case eReady:        return "ready";
    case eBlocked:      return "blocked";
    case eSuspended:    return "suspended";
    case eDeleted:      return "deleted";
    case eInvalid:      return "invalid";
    default:            return "?";
    }
}

static bool is_idle_task(const char *name)
{
    /* The idle tasks are named "IDLE", "IDLE0", "IDLE1", ... */
    if (strncasecmp(name, "IDLE", 4) != 0) {
        return false;
    }
    for (const char *p = name + 4; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

/* The core the task is running on right now, -1 if it is not running */
static int running_core(TaskHandle_t handle)
{
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        if (xTaskGetCurrentTaskHandleForCore(core) == handle) {
            return (int)core;
        }
    }
    return -1;
}

/* Description of the core a task is running on / pinned to */
static void core_str(const TaskStatus_t *task, char *dst, size_t len)
{
    int running = running_core(task->xHandle);

    if (running >= 0) {
        /* The task is running on that core at this moment */
        snprintf(dst, len, "%d*", running);
        return;
    }
#if configTASKLIST_INCLUDE_COREID == 1
    if (task->xCoreID >= 0) {
        /* The task is pinned to that core */
        snprintf(dst, len, "%d", (int)task->xCoreID);
        return;
    }
#endif
    /* The task is not pinned, it can run on any core */
    snprintf(dst, len, "-");
}


/* Take a snapshot of the run time of every task */
static size_t snapshot_tasks(task_runtime_t *out, size_t max)
{
    UBaseType_t num = uxTaskGetNumberOfTasks();
    if (num > max) {
        num = max;
    }
    if (num == 0) {
        return 0;
    }

    TaskStatus_t *status = calloc(num, sizeof(TaskStatus_t));
    if (status == NULL) {
        return 0;
    }

    UBaseType_t count = uxTaskGetSystemState(status, num, NULL);
    for (UBaseType_t i = 0; i < count; i++) {
        out[i].handle = status[i].xHandle;
        strlcpy(out[i].name, status[i].pcTaskName ? status[i].pcTaskName : "?",
                sizeof(out[i].name));
        out[i].runtime_us = (uint32_t)status[i].ulRunTimeCounter;
    }

    free(status);
    return count;
}

/* ------------------------------------------------------------------ */
/* tasks                                                               */
/* ------------------------------------------------------------------ */
static int cmd_tasks(int argc, char **argv)
{
    UBaseType_t num = uxTaskGetNumberOfTasks();
    if (num == 0) {
        printf("No task found.\n");
        return 0;
    }

    TaskStatus_t *status = calloc(num, sizeof(TaskStatus_t));
    if (status == NULL) {
        printf("Out of memory.\n");
        return 1;
    }

    UBaseType_t count = uxTaskGetSystemState(status, num, NULL);

    printf("\n%-18s %-9s %4s %5s %8s %10s\n",
           "Task", "State", "Prio", "Core", "StackHWM", "Runtime");
    for (UBaseType_t i = 0; i < count; i++) {
        const TaskStatus_t *task = &status[i];
        char core[8];
        core_str(task, core, sizeof(core));
        /* The stack sizes are given in words (4 bytes) */
        printf("%-18s %-9s %4u %5s %8u %9u ms\n",
               task->pcTaskName,
               state_str(task->eCurrentState),
               (unsigned)task->uxCurrentPriority,
               core,
               (unsigned)(task->usStackHighWaterMark * sizeof(StackType_t)),
               (unsigned)(task->ulRunTimeCounter / 1000));
    }

    free(status);
    printf("\n%u task(s). StackHWM is the minimum free stack size in bytes\n"
           "(the closer to 0, the closer the task is to an overflow).\n"
           "Core: '*' = the task is running on that core right now,\n"
           "      the number alone = the task is pinned to that core,\n"
           "      '-' = the task is not pinned, it can run on any core.\n",
           (unsigned)count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mem                                                                */
/* ------------------------------------------------------------------ */
static void print_mem_region(const char *name, uint32_t caps)
{
    printf("  %-10s total %8u  free %8u  min free %8u  largest %8u\n",
           name,
           (unsigned)heap_caps_get_total_size(caps),
           (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps));
}

static int cmd_mem(int argc, char **argv)
{
    printf("\nMemory\n");
    printf("  heap: %u free, %u minimum free (bytes)\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
    print_mem_region("internal", MALLOC_CAP_INTERNAL);
    print_mem_region("8bit", MALLOC_CAP_8BIT);
    print_mem_region("dma", MALLOC_CAP_DMA);
#if CONFIG_SPIRAM
    print_mem_region("psram", MALLOC_CAP_SPIRAM);
#endif
    printf("\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* cpu                                                                */
/* ------------------------------------------------------------------ */
static int cmd_cpu(int argc, char **argv)
{
#if !configGENERATE_RUN_TIME_STATS
    printf("Run time stats are disabled, enable CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS.\n");
    return 1;
#else
    task_runtime_t *before = calloc(TASKS_MAX, sizeof(task_runtime_t));
    task_runtime_t *after = calloc(TASKS_MAX, sizeof(task_runtime_t));
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        printf("Out of memory.\n");
        return 1;
    }

    size_t count = snapshot_tasks(before, TASKS_MAX);
    if (count == 0) {
        free(before);
        free(after);
        printf("No task found.\n");
        return 1;
    }

    int64_t start = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE idle_before[configNUMBER_OF_CORES];
    configRUN_TIME_COUNTER_TYPE idle_after[configNUMBER_OF_CORES];
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        idle_before[core] = ulTaskGetIdleRunTimeCounterForCore(core);
    }
    vTaskDelay(pdMS_TO_TICKS(CPU_SAMPLE_MS));
    int64_t elapsed = esp_timer_get_time() - start;
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        idle_after[core] = ulTaskGetIdleRunTimeCounterForCore(core);
    }

    size_t count_after = snapshot_tasks(after, TASKS_MAX);
    if (count_after == 0) {
        free(before);
        free(after);
        printf("No task found.\n");
        return 1;
    }

    printf("\nCPU usage over %d ms (%d core(s), 100%% = one core fully busy)\n",
           CPU_SAMPLE_MS, (int)configNUMBER_OF_CORES);

    /* Collect the result and sort it by CPU usage, the busiest task first */
    task_usage_t *usage = calloc(count_after, sizeof(task_usage_t));
    if (usage == NULL) {
        free(before);
        free(after);
        printf("Out of memory.\n");
        return 1;
    }

    size_t num = 0;
    double busy = 0.0;
    for (size_t i = 0; i < count_after; i++) {
        uint32_t previous = 0;
        bool found = false;
        for (size_t j = 0; j < count; j++) {
            if (before[j].handle == after[i].handle) {
                previous = before[j].runtime_us;
                found = true;
                break;
            }
        }
        if (!found) {
            /* The task was created during the measurement */
            continue;
        }
        if (after[i].runtime_us < previous) {
            /* The 32 bit run time counter wrapped around */
            continue;
        }

        uint32_t delta = after[i].runtime_us - previous;
        double percent = (double)delta * 100.0 / (double)elapsed;
        if (!is_idle_task(after[i].name)) {
            busy += percent;
        }
        strlcpy(usage[num].name, after[i].name, sizeof(usage[num].name));
        usage[num].percent = percent;
        usage[num].runtime_ms = delta / 1000;
        num++;
    }

    /* Insertion sort, the number of tasks is small */
    for (size_t i = 1; i < num; i++) {
        task_usage_t current = usage[i];
        size_t j = i;
        while (j > 0 && usage[j - 1].percent < current.percent) {
            usage[j] = usage[j - 1];
            j--;
        }
        usage[j] = current;
    }

    printf("\n%-18s %7s %10s\n", "Task", "CPU%", "Runtime");
    for (size_t i = 0; i < num; i++) {
        printf("%-18s %6.1f%% %9u ms\n",
               usage[i].name, usage[i].percent, (unsigned)usage[i].runtime_ms);
    }

    double capacity = 100.0 * configNUMBER_OF_CORES;
    printf("\nCPU load: %.1f%% of one core (%.1f%% of the total capacity of %d core(s))\n",
           busy, busy * 100.0 / capacity, (int)configNUMBER_OF_CORES);

    /* Per core load, based on the idle time of each core */
    printf("\nPer core load:\n");
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        double idle = 0.0;
        if (idle_after[core] >= idle_before[core]) {
            idle = (double)(idle_after[core] - idle_before[core]) * 100.0 / (double)elapsed;
        }
        printf("  core %d: %6.1f%% busy (idle %5.1f%%)\n",
               (int)core, 100.0 - idle, idle);
    }
    printf("\n");

    free(usage);
    free(before);
    free(after);
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */
esp_err_t sys_stats_register(void)
{
    const esp_console_cmd_t tasks_cmd = {
        .command = "tasks",
        .help = "List the FreeRTOS tasks",
        .hint = NULL,
        .func = &cmd_tasks,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&tasks_cmd), TAG,
                        "failed to register the 'tasks' command");

    const esp_console_cmd_t mem_cmd = {
        .command = "mem",
        .help = "Show the memory usage",
        .hint = NULL,
        .func = &cmd_mem,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&mem_cmd), TAG,
                        "failed to register the 'mem' command");

    const esp_console_cmd_t cpu_cmd = {
        .command = "cpu",
        .help = "Show the CPU load (measured over one second)",
        .hint = NULL,
        .func = &cmd_cpu,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cpu_cmd), TAG,
                        "failed to register the 'cpu' command");

    return ESP_OK;
}
