#pragma once

#include <stdint.h>

//---------------------------------------------------

typedef void (*job_func)(void*);

//---------------------------------------------------
struct job_counter;

typedef struct job_decal {
    job_func    job;
    void*       data;
} job_decal;

typedef struct job_fence {
    job_counter*    counter;
    int64_t         gen;

    job_fence()
        : counter(nullptr)
        , gen(0) {
    }
} job_fence;

//---------------------------------------------------

void
job_init(uint32_t job_count, uint32_t fiber_count, uint32_t worker_thread_count);

void
job_shutdown();

job_fence
job_kick(job_decal* decal, uint32_t count);

void
job_wait_for_complete(job_fence fence);