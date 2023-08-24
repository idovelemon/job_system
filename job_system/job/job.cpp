#include "job.h"
#include <assert.h>
#include <malloc.h>
#include <memory.h>
#include <stdio.h>
#include <Windows.h>
#include "../../tracy/src/tracy-0.9.1/public/tracy/TracyC.h"

//---------------------------------------------------
// atomic

// atomic_word must be 8 bytes aligned if you want to use it with atomic_* ops.
typedef __int64 atomic_word;

typedef enum memory_order {
    memory_order_relaxed,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst,
} memory_order;

atomic_word
atomic_load(const volatile atomic_word* p, memory_order mo) {
    (void)mo;

    assert(mo == memory_order_relaxed
        || mo == memory_order_consume
        || mo == memory_order_acquire
        || mo == memory_order_seq_cst);

    atomic_word v = *p;
    _ReadWriteBarrier();
    return v;
}

void
atomic_store(volatile atomic_word* p, atomic_word v, memory_order mo) {
    assert(mo == memory_order_relaxed
        || mo == memory_order_release
        || mo == memory_order_seq_cst);

    if (mo == memory_order_seq_cst) {
        _InterlockedExchange64((volatile LONG64*)p, (LONG64)v);
    } else {
        _ReadWriteBarrier();
        *p = v;
    }
}

bool
atomic_compare_exchange_weak(volatile atomic_word* p, atomic_word* old, atomic_word v) {
    atomic_word tmp = (atomic_word)_InterlockedCompareExchange64((volatile LONG64*)p, (LONG64)v, (LONG64)*old);
    if (*old == tmp)
        return true;

    *old = tmp;
    return false;
}

atomic_word
atomic_increment(volatile atomic_word* p) {
    return (atomic_word)_InterlockedIncrement64((volatile LONG64*)p);
}

atomic_word
atomic_decrement(volatile atomic_word* p) {
    return (atomic_word)_InterlockedDecrement64((volatile LONG64*)p);
}

//---------------------------------------------------
// atomic queue

static size_t const cacheline_size = 64;
typedef char        cacheline_pad_t[cacheline_size];

template<typename T>
struct cell {
    volatile atomic_word    sequence;
    T                       data;
};

template<typename T>
struct atomic_queue {
    cacheline_pad_t         pad0;

    cell<T>*                data;
    uint32_t                mask;
    cacheline_pad_t         pad1;

    volatile atomic_word    enqueue_pos;
    cacheline_pad_t         pad2;

    volatile atomic_word    dequeue_pos;
    cacheline_pad_t         pad3;

    atomic_queue(uint32_t count)
        : mask(count - 1) {
        data = (cell<T>*)malloc(sizeof(cell<T>) * count);
        memset(data, 0, sizeof(T) * count);

        for (uint32_t i = 0; i < count; i++) {
            atomic_store(&data[i].sequence, i, memory_order_relaxed);
        }

        atomic_store(&enqueue_pos, 0, memory_order_relaxed);
        atomic_store(&dequeue_pos, 0, memory_order_relaxed);
    }

    ~atomic_queue() {
        free(data);
    }
};

template<typename T> bool
atomic_queue_enqueue(atomic_queue<T>& queue, T& data) {
    cell<T>* c = nullptr;
    atomic_word pos = atomic_load(&queue.enqueue_pos, memory_order_relaxed);

    while (true) {
        c = &queue.data[pos & queue.mask];
        atomic_word seq = atomic_load(&c->sequence, memory_order_acquire);
        atomic_word diff = seq - pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak(&queue.enqueue_pos, &pos, pos + 1)) {
                break;
            }
        } else if (diff < 0) {
            return false;
        } else {
            pos = atomic_load(&queue.enqueue_pos, memory_order_relaxed);
        }
    }

    c->data = data;
    atomic_store(&c->sequence, pos + 1, memory_order_release);

    return true;
}

template<typename T> bool
atomic_queue_dequeue(atomic_queue<T>& queue, T& data) {
    cell<T>* c = nullptr;
    atomic_word pos = atomic_load(&queue.dequeue_pos, memory_order_relaxed);

    while (true) {
        c = &queue.data[pos & queue.mask];
        atomic_word seq = atomic_load(&c->sequence, memory_order_acquire);
        atomic_word diff = seq - (pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak(&queue.dequeue_pos, &pos, pos + 1)) {
                break;
            }
        } else if (diff < 0) {
            return false;
        } else {
            pos = atomic_load(&queue.dequeue_pos, memory_order_relaxed);
        }
    }

    data = c->data;
    atomic_store(&c->sequence, pos + queue.mask + 1, memory_order_release);

    return true;
}

//---------------------------------------------------
// fiber

typedef void* fiber;
typedef void (*fiber_func)(void* param);

fiber
create_fiber(size_t stack_size, fiber_func func, void* param) {
    return CreateFiber(stack_size, func, param);
}

fiber
convert_thread_to_fiber() {
    return ConvertThreadToFiber(nullptr);
}

void
switch_fiber(fiber f) {
    SwitchToFiber(f);
}

void
delete_fiber(fiber f) {
    DeleteFiber(f);
}

fiber
get_current_fiber() {
    return GetCurrentFiber();
}

//---------------------------------------------------

typedef struct job_counter {
    volatile atomic_word count;
    volatile atomic_word gen;
} job_counter;

typedef struct job_queue_node {
    job_decal               decal;
    uint32_t                counter_index;
    job_counter*            counter;
} job_queue_node;

typedef struct job_system {
    atomic_queue<job_queue_node>*   job_queue;

    DWORD                           main_thread_id;
    fiber                           main_thread_fiber;
    fiber*                          fiber_pool;
    fiber*                          switch_fiber_pool;  // which fiber switch to fiber in fiber_pool
    job_queue_node*                 fiber_exec_job_pool;  // which job will be executed by fiber
    uint32_t                        fiber_count;
    atomic_queue<uint32_t>*         free_fiber_indices_queue;

    job_counter*                    counter_pool;
    uint32_t                        counter_count;
    atomic_queue<uint32_t>*         free_counter_indices_queue;

    volatile atomic_word            worker_thread_ready_count;
    HANDLE*                         worker_thread_pool;
    DWORD*                          worker_thread_id;
    fiber*                          worker_thread_fiber;
    uint32_t                        worker_thread_count;
    volatile atomic_word            worker_thread_active;
} job_system;

job_system* g_job_system = nullptr;

//---------------------------------------------------

void
run_free_fiber() {
    // fetch a job from job queue
    job_queue_node node;
    if (atomic_queue_dequeue<job_queue_node>(*g_job_system->job_queue, node)) {
        while (true) {
            // fetch a free fiber and run it
            uint32_t fiber_index = 0;
            if (atomic_queue_dequeue<uint32_t>(*g_job_system->free_fiber_indices_queue, fiber_index)) {
                g_job_system->switch_fiber_pool[fiber_index] = get_current_fiber();
                g_job_system->fiber_exec_job_pool[fiber_index] = node;
                fiber f = g_job_system->fiber_pool[fiber_index];
                switch_fiber(f);

                // release current fiber
                atomic_queue_enqueue<uint32_t>(*g_job_system->free_fiber_indices_queue, fiber_index);
                break;
            }
        }
    }
}

DWORD
job_worker_thread(void* param) {
    // wait for thread ready
    while (true) {
        atomic_word tmp = atomic_load(&g_job_system->worker_thread_active, memory_order_acquire);
        if (tmp == 1) {
            break;
        } else {
            _mm_pause();
        }
    }

    DWORD thread_id = GetCurrentThreadId();
    fiber f = convert_thread_to_fiber();

    int32_t found = -1;
    for (uint32_t i = 0; i < g_job_system->worker_thread_count; i++) {
        if (thread_id == g_job_system->worker_thread_id[i]) {
            g_job_system->worker_thread_fiber[i] = f;
            found = i;
            break;
        }
    }

    atomic_increment(&g_job_system->worker_thread_ready_count);

    assert(found != -1);

    char thread_name[64];
    sprintf_s(thread_name, "job worker %d", found);
    TracyCSetThreadName(thread_name);

    while (true) {
        atomic_word tmp = atomic_load(&g_job_system->worker_thread_active, memory_order_acquire);
        if (tmp == 0) {
            break;
        }

        run_free_fiber();
    }

    return 0;
}

void
job_fiber(void* param) {
    if (g_job_system == nullptr) {
        assert(false);
        return;
    }

    uint64_t fiber_index = reinterpret_cast<uint64_t>(param);
    if (fiber_index >= g_job_system->fiber_count) {
        assert(false);
        return;
    }

    while (true) {
        job_queue_node node = g_job_system->fiber_exec_job_pool[fiber_index];

        // run job function
        node.decal.job(node.decal.data);

        atomic_decrement(&node.counter->count);
        atomic_word tmp = atomic_load(&node.counter->count, memory_order_acquire);
        if (tmp <= 0) {
            TracyCZoneN(ctx, "job_fiber_finish", true);
            atomic_queue_enqueue<uint32_t>(*g_job_system->free_counter_indices_queue, node.counter_index);
            TracyCZoneEnd(ctx);
        }

        switch_fiber(g_job_system->switch_fiber_pool[fiber_index]);
    }
}

void
job_init(uint32_t job_count, uint32_t fiber_count, uint32_t worker_thread_count) {
    TracyCZoneN(ctx, "job_init", true);

    if (g_job_system == nullptr) {
        g_job_system = new job_system();
        g_job_system->job_queue = new atomic_queue<job_queue_node>(job_count);

        g_job_system->fiber_count = fiber_count;
        g_job_system->free_fiber_indices_queue = new atomic_queue<uint32_t>(fiber_count);
        g_job_system->fiber_pool = (fiber*)malloc(sizeof(fiber) * fiber_count);
        g_job_system->switch_fiber_pool = (fiber*)malloc(sizeof(fiber) * fiber_count);
        g_job_system->fiber_exec_job_pool = (job_queue_node*)malloc(sizeof(job_queue_node) * fiber_count);
        for (uint32_t i = 0; i < fiber_count; i++) {
            g_job_system->fiber_pool[i] = create_fiber(0, job_fiber, reinterpret_cast<void*>((uint64_t)i));
            g_job_system->switch_fiber_pool[i] = nullptr;
            atomic_queue_enqueue<uint32_t>(*g_job_system->free_fiber_indices_queue, i);
        }
        g_job_system->main_thread_fiber = convert_thread_to_fiber();
        g_job_system->main_thread_id = GetCurrentThreadId();

        g_job_system->counter_count = job_count;
        g_job_system->free_counter_indices_queue = new atomic_queue<uint32_t>(job_count);
        g_job_system->counter_pool = (job_counter*)malloc(sizeof(job_counter) * job_count);
        for (uint32_t i = 0; i < job_count; i++) {
            atomic_store(&g_job_system->counter_pool[i].count, 0, memory_order_relaxed);
            atomic_store(&g_job_system->counter_pool[i].gen, 0, memory_order_relaxed);
            atomic_queue_enqueue<uint32_t>(*g_job_system->free_counter_indices_queue, i);
        }

        g_job_system->worker_thread_count = worker_thread_count;
        g_job_system->worker_thread_pool = (HANDLE*)malloc(sizeof(HANDLE) * worker_thread_count);
        g_job_system->worker_thread_id = (DWORD*)malloc(sizeof(DWORD) * worker_thread_count);
        g_job_system->worker_thread_fiber = (fiber*)malloc(sizeof(fiber) * worker_thread_count);
        memset(g_job_system->worker_thread_fiber, sizeof(fiber) * worker_thread_count, 0);

        atomic_store(&g_job_system->worker_thread_active, 0, memory_order_seq_cst);

        DWORD affinity = 1;
        SetThreadAffinityMask(GetCurrentThread(), affinity);
        TracyCSetThreadName("main");

        for (uint32_t i = 0; i < worker_thread_count; i++) {
            SECURITY_ATTRIBUTES security_attr;
            security_attr.bInheritHandle = FALSE;
            security_attr.lpSecurityDescriptor = nullptr;
            security_attr.nLength = 0;
            g_job_system->worker_thread_pool[i] = CreateThread(&security_attr, 0, job_worker_thread, g_job_system, 0, &g_job_system->worker_thread_id[i]);

            affinity = (affinity << 1);
            SetThreadAffinityMask(g_job_system->worker_thread_pool[i], affinity);
        }

        atomic_store(&g_job_system->worker_thread_ready_count, 0, memory_order_seq_cst);
        atomic_store(&g_job_system->worker_thread_active, 1, memory_order_release);

        while (true) {
            atomic_word tmp = atomic_load(&g_job_system->worker_thread_ready_count, memory_order_acquire);
            if (tmp == g_job_system->worker_thread_count) {
                break;
            } else {
                _mm_pause();
            }
        }
    }

    TracyCZoneEnd(ctx);
}

void
job_shutdown() {
    TracyCZoneN(ctx, "job_shutdown", true);

    if (g_job_system != nullptr) {
        // inactive all worker threads
        atomic_store(&g_job_system->worker_thread_active, 0, memory_order_seq_cst);

        for (uint32_t i = 0; i < g_job_system->worker_thread_count; i++) {
            DWORD hr = WaitForSingleObject(g_job_system->worker_thread_pool[i], INFINITE);
            assert(hr == WAIT_OBJECT_0);
            CloseHandle(g_job_system->worker_thread_pool[i]);
        }
        free(g_job_system->worker_thread_pool);
        g_job_system->worker_thread_pool = nullptr;

        if (g_job_system->fiber_pool != nullptr) {
            for (uint32_t i = 0; i < g_job_system->fiber_count; i++) {
                delete_fiber(g_job_system->fiber_pool[i]);
            }
        }

        free(g_job_system->worker_thread_id);
        g_job_system->worker_thread_id = nullptr;

        free((void*)g_job_system->counter_pool);
        g_job_system->counter_pool = nullptr;

        delete g_job_system->free_counter_indices_queue;
        g_job_system->free_counter_indices_queue = nullptr;

        free(g_job_system->switch_fiber_pool);
        g_job_system->switch_fiber_pool = nullptr;

        free(g_job_system->fiber_exec_job_pool);
        g_job_system->fiber_exec_job_pool = nullptr;

        free(g_job_system->fiber_pool);
        g_job_system->fiber_pool = nullptr;

        delete g_job_system->free_fiber_indices_queue;
        g_job_system->free_fiber_indices_queue = nullptr;

        delete g_job_system->job_queue;
        g_job_system->job_queue = nullptr;
    }

    delete g_job_system;
    g_job_system = nullptr;

    TracyCZoneEnd(ctx);
}

job_fence
job_kick(job_decal* decal, uint32_t count) {
    TracyCZoneN(ctx, "job_kick", true);

    if (g_job_system == nullptr || decal == nullptr || count == 0) {
        return job_fence();
    }

    // fetch a free counter
    uint32_t counter_index = 0;
    if (!atomic_queue_dequeue<uint32_t>(*g_job_system->free_counter_indices_queue, counter_index)) {
        assert(false);  // not enough counter
        return job_fence();
    }

    job_counter* counter = &g_job_system->counter_pool[counter_index];
    atomic_store(&counter->count, count, memory_order_release);
    atomic_word gen = atomic_increment(&counter->gen);

    // enqueue job
    for (uint32_t i = 0; i < count; i++) {
        job_queue_node node;
        node.decal = decal[i];
        node.counter_index = counter_index;
        node.counter = counter;
        if (!atomic_queue_enqueue<job_queue_node>(*g_job_system->job_queue, node)) {
            assert(false);  // full of job queue
            atomic_store(&counter->count, i + 1, memory_order_release);
            break;
        }
    }

    job_fence fence;
    fence.counter = counter;
    fence.gen = gen;

    TracyCZoneEnd(ctx);

    return fence;
}

void
job_wait_for_complete(job_fence fence) {
    TracyCZoneN(ctx, "job_wait_for_complete", true);

    if (fence.counter == nullptr) return;

    atomic_word gen = atomic_load(&fence.counter->gen, memory_order_acquire);
    if (gen != fence.gen) return;

    while (true) {
        // check if job finished
        atomic_word tmp = atomic_load(&fence.counter->count, memory_order_acquire);
        if (tmp == 0) {
            break;
        }

        run_free_fiber();
    }

    TracyCZoneEnd(ctx);
}
