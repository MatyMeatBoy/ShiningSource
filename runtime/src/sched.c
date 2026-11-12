/* Cooperative task scheduler of the Shining Soul engine, as native coroutines.
 *
 * The cartridge implements this in hand-written ARM (asm/sched.s in the decompilation): tasks live in a list
 * ordered by priority, each with its own stack, and switch by saving and restoring registers. Here every task
 * is a fiber with a native stack; the task record in GBA memory keeps the same fields and states, because game
 * code reads them.
 *
 * Task record (offsets as in the original):
 *   +0x00 next   +0x04 state   +0x08 priority   +0x0C entry / resume address   +0x10 stack pointer
 *   +0x14 signal it waits for  +0x18 previous   +0x20 memory to free on exit   +0x24 destructor
 * States: 0 not scheduled, 1 ready, 2 running, 3 ran this frame, 4 waiting for a signal.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

typedef struct Task {
    uint32_t next, state, priority, entry, sp, signal, prev, unused1C, memory, destructor;
} Task;

#define TASK(address) ((Task *)(uintptr_t)(address))
#define HEAD     (*(uint32_t *)(uintptr_t)0x03001240u)   /* first task of the list */
#define CURRENT  (*(uint32_t *)(uintptr_t)0x0300123Cu)   /* task being run */
#define COUNT    (*(uint32_t *)(uintptr_t)0x03001244u)   /* scheduled tasks */

enum { DEAD = 0, READY = 1, RUNNING = 2, RAN = 3, WAITING = 4 };
enum { MAX_FIBERS = 256, STACK_BYTES = 512 * 1024 };

/* provided by the game */
void sub_08001204(void);            /* end of frame: wait for VBlank, wake signal 1, flush sprites */
void sub_08001538(uint32_t memory); /* free */

static struct Fiber { uint32_t task; void *fiber; uint32_t a, b; int finished; } fibers[MAX_FIBERS];
static void *scheduler_fiber;
static int trace = -1;
#define TRACE(...) do { if (trace < 0) trace = getenv("SS_TRACE") != NULL; if (trace) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

static struct Fiber *fiber_of(uint32_t task, int create)
{
    struct Fiber *free_slot = NULL;
    int i;
    for (i = 0; i < MAX_FIBERS; i++) {
        if (fibers[i].task == task && fibers[i].fiber)
            return &fibers[i];
        if (!fibers[i].fiber && !free_slot)
            free_slot = &fibers[i];
    }
    if (!create)
        return NULL;
    if (!free_slot) {
        fprintf(stderr, "scheduler: more than %d live tasks\n", MAX_FIBERS);
        exit(1);
    }
    free_slot->task = task;
    free_slot->finished = 0;
    return free_slot;
}

static void unlink_task(Task *task)
{
    uint32_t prev = task->prev, next = task->next;
    COUNT--;
    task->state = DEAD;
    TASK(prev)->next = next;
    if (next)
        TASK(next)->prev = prev;
}

/* Common end of a task: destructor, free its memory, leave the list. */
static void finish_task(uint32_t address)
{
    Task *task = TASK(address);
    if (task->destructor)
        ((void (*)(uint32_t))(uintptr_t)task->destructor)(address);
    sub_08001538(task->memory);
    unlink_task(task);
}

static void CALLBACK fiber_main(void *parameter)
{
    struct Fiber *self = parameter;
    TRACE("task %08X starts at %08X\n", (unsigned)self->task, (unsigned)TASK(self->task)->entry);
    ((void (*)(uint32_t, uint32_t))(uintptr_t)TASK(self->task)->entry)(self->a, self->b);
    TRACE("task %08X returned\n", (unsigned)self->task);
    /* returning from the entry function is the original's "lr = exit routine" */
    finish_task(self->task);
    self->finished = 1;
    SwitchToFiber(scheduler_fiber);
}

static void prepare_task(uint32_t address, uint32_t a, uint32_t b)
{
    struct Fiber *slot = fiber_of(address, 1);
    if (slot->fiber)            /* restarted while an old fiber of the same record still exists */
        DeleteFiber(slot->fiber);
    slot->a = a;
    slot->b = b;
    slot->finished = 0;
    slot->fiber = CreateFiber(STACK_BYTES, fiber_main, slot);
    TASK(address)->sp -= 0x30;  /* the original builds its first register frame here; kept for any reader */
}

static void reap(void)
{
    int i;
    for (i = 0; i < MAX_FIBERS; i++) {
        if (fibers[i].fiber && (fibers[i].finished || TASK(fibers[i].task)->state == DEAD)) {
            DeleteFiber(fibers[i].fiber);
            fibers[i].fiber = NULL;
            fibers[i].task = 0;
        }
    }
}

/* sub_0800023C: make `root` the only task and run the scheduler forever. */
void sub_0800023C(uint32_t root, uint32_t a, uint32_t b)
{
    Task *task = TASK(root);
    uint32_t address;

    task->next = 0;
    task->priority = 0;
    task->signal = 0;
    task->prev = 0;
    HEAD = root;
    CURRENT = 0;
    task->state = READY;
    COUNT = 1;
    scheduler_fiber = ConvertThreadToFiber(NULL);
    TRACE("scheduler starts, root task %08X\n", (unsigned)root);
    prepare_task(root, a, b);

    for (;;) {
        for (CURRENT = HEAD; CURRENT; CURRENT = TASK(address)->next) {
            address = CURRENT;
            if (TASK(address)->state != READY)
                continue;
            TASK(address)->state = RUNNING;
            SwitchToFiber(fiber_of(address, 0)->fiber);
            /* back in the scheduler: a task that did not start waiting has run for this frame */
            if (TASK(address)->state != WAITING && TASK(address)->state != DEAD)
                TASK(address)->state = RAN;
        }
        reap();
        sub_08001204();
        for (address = HEAD; address; address = TASK(address)->next)
            if (TASK(address)->state != WAITING)
                TASK(address)->state = READY;
    }
}

/* sub_08000274: schedule a task. Returns 1 if it is already scheduled, 0 otherwise. */
uint32_t sub_08000274(uint32_t address, uint32_t a, uint32_t b)
{
    Task *task = TASK(address), *at;
    uint32_t cursor, last = 0;

    for (cursor = HEAD; cursor; cursor = TASK(cursor)->next)
        if (cursor == address)
            return 1;
    if (task->entry < 0x01800000u || task->entry >= 0x02000000u) {
        fprintf(stderr, "scheduler: task %08X has entry %08X, which is not native code; started from %p%c",
                (unsigned)address, (unsigned)task->entry, __builtin_return_address(0), 10);
        fflush(stderr);
    }
    if (task->priority == 0)
        task->priority = 1;
    task->state = READY;
    for (cursor = HEAD; cursor; last = cursor, cursor = TASK(cursor)->next)
        if ((int32_t)(TASK(cursor)->priority - task->priority) >= 0)
            break;
    if (cursor) {               /* insert before the first task of equal or higher priority value */
        at = TASK(cursor);
        TASK(at->prev)->next = address;
        task->prev = at->prev;
        task->next = cursor;
        at->prev = address;
    } else {                    /* append */
        TASK(last)->next = address;
        task->next = 0;
        task->prev = last;
    }
    COUNT++;
    prepare_task(address, a, b);
    return 0;
}

/* sub_080003F4: the running task waits for `signal`. sub_080003F0 waits for signal 1 (the next frame). */
void sub_080003F4(uint32_t signal)
{
    Task *task = TASK(CURRENT);
    task->signal = signal;
    task->state = WAITING;
    SwitchToFiber(scheduler_fiber);
}

void sub_080003F0(void)
{
    sub_080003F4(1);
}

/* sub_0800035C: the running task ends itself. */
void sub_0800035C(void)
{
    struct Fiber *self = fiber_of(CURRENT, 0);
    finish_task(CURRENT);
    self->finished = 1;
    SwitchToFiber(scheduler_fiber);
}

/* sub_080003A8: end another task. */
void sub_080003A8(uint32_t address)
{
    Task *task = TASK(address);
    uint32_t prev = task->prev, next = task->next;
    finish_task(address);
    task->next = next ? next : prev;    /* the original leaves a link an iterating caller can still follow */
}

/* sub_08000420: wake every task waiting for `signal`. */
void sub_08000420(uint32_t signal)
{
    uint32_t address;
    for (address = HEAD; address; address = TASK(address)->next) {
        if (TASK(address)->signal == signal) {
            TASK(address)->signal = 0;
            TASK(address)->state = READY;
        }
    }
}

/* sub_08000440: is any task waiting for `signal`? */
uint32_t sub_08000440(uint32_t signal)
{
    uint32_t address;
    for (address = HEAD; address; address = TASK(address)->next)
        if (TASK(address)->state == WAITING && TASK(address)->signal == signal)
            return 1;
    return 0;
}

uint32_t sub_08000464(void) { return COUNT; }
uint32_t sub_0800048C(void) { return CURRENT; }

/* sub_08000470: is this task in the list? */
uint32_t sub_08000470(uint32_t address)
{
    uint32_t cursor;
    for (cursor = HEAD; cursor; cursor = TASK(cursor)->next)
        if (cursor == address)
            return 1;
    return 0;
}

/* sub_08000498: *value becomes the remainder, the quotient is returned (BIOS Div with both results). */
int32_t sub_08000498(int32_t *value, int32_t divisor)
{
    int32_t quotient;
    if (divisor == 0)
        return 0;
    quotient = *value / divisor;
    *value %= divisor;
    return quotient;
}
