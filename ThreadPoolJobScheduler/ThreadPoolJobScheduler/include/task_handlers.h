/*
 * task_handlers.h - Function-pointer based registry mapping each TaskType
 *                   to the C function that executes it.
 *
 * Handlers never run shell commands or user-supplied code; they only
 * operate on the data stored in the Job's TaskInput.
 */
#ifndef TASK_HANDLERS_H
#define TASK_HANDLERS_H

#include <stddef.h>
#include <stdint.h>

#include "job.h"

/* Returns 0 on success (result filled) or -1 on failure (error filled). */
typedef int (*TaskHandler)(const Job *job, char *result, size_t result_len,
                           char *error, size_t error_len);

typedef struct TaskDescriptor {
    TaskType    type;
    const char *name;
    const char *description;
    TaskHandler handler;
} TaskDescriptor;

const TaskDescriptor *task_registry_get(TaskType type);
TaskHandler           task_registry_lookup(TaskType type);
size_t                task_registry_count(void);

/* Looks up the handler through the registry and calls it. */
int task_execute(const Job *job, char *result, size_t result_len,
                 char *error, size_t error_len);

/* Validates input before a job is accepted. 0 = valid. */
int task_validate_input(TaskType type, const TaskInput *input,
                        char *error, size_t error_len);

uint32_t crc32_compute(uint32_t crc, const void *data, size_t len);

/* Limits enforced by task_validate_input */
#define CALC_SUM_MAX        1000000000ULL
#define CALC_FACTORIAL_MAX  20ULL
#define CALC_FIBONACCI_MAX  93ULL
#define CALC_PRIME_MAX      20000000ULL

#endif /* TASK_HANDLERS_H */
