#include "job/job.h"
#include <stdio.h>
#include <math.h>
#include <Windows.h>
#include "../tracy/src/tracy-0.9.1/public/tracy/TracyC.h"

struct test_2_job_data {
    job_fence fence;
    void* data;
};

struct test_3_job_data {
    job_fence fence;
    void* data;
};

void
test_job(void* job_data) {
    TracyCZoneN(ctx, "test_job", true);

    for (uint32_t i = 0; i < 100; i++) {
        *(uint8_t*)job_data = sinf(*(uint8_t*)job_data + 100);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = sinf(*(uint8_t*)job_data + 100);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = sinf(*(uint8_t*)job_data + 100);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = sinf(*(uint8_t*)job_data + 100);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
        *(uint8_t*)job_data = cosf(*(uint8_t*)job_data);
    }

    TracyCZoneEnd(ctx);
}

void
test_2_dep_job(void* job_data) {
    TracyCZoneN(ctx, "test_2_dep_job", true);

    Sleep(1000);
    printf("test job finish %d\n", *(uint8_t*)job_data);
    *(uint8_t*)job_data = *(uint8_t*)job_data + 100;

    TracyCZoneEnd(ctx);
}

void
test_2_job(void* job_data) {
    TracyCZoneN(ctx, "test_2_job", true);

    test_2_job_data* data = (test_2_job_data*)job_data;

    job_wait_for_complete(data->fence);

    printf("test job 2 finish %d\n", *(uint8_t*)data->data);

    TracyCZoneEnd(ctx);
}

void
test_3_dep_job(void* job_data) {
    TracyCZoneN(ctx, "test_3_dep_job", true);

    Sleep(1);
    *(uint8_t*)job_data = *(uint8_t*)job_data + 100;

    TracyCZoneEnd(ctx);
}

void
test_3_job(void* job_data) {
    TracyCZoneN(ctx, "test_3_job", true);

    test_2_job_data* data = (test_2_job_data*)job_data;

    job_wait_for_complete(data->fence);

    TracyCZoneEnd(ctx);
}

void
test_4_sub_job(void* job_data) {
    TracyCZoneN(ctx, "test_4_sub_job", true);
    Sleep(1);
    TracyCZoneEnd(ctx);
}

void
test_4_job(void* job_data) {
    TracyCZoneN(ctx, "test_4_job", true);

    constexpr uint32_t job_count = 100;

    char test_data[job_count];

    job_decal decal[job_count];
    for (uint32_t i = 0; i < job_count; i++) {
        test_data[i] = i;
        decal[i].job = test_4_sub_job;
        decal[i].data = &test_data[i];
    }

    job_fence fence = job_kick(decal, job_count);
    job_wait_for_complete(fence);

    TracyCZoneEnd(ctx);
}

void
test_1() {
    TracyCZoneN(ctx, "test_1", true);

    job_init(512, 512, 27);

    constexpr uint32_t job_count = 100;

    char test_data[job_count];

    job_decal decal[job_count];
    for (uint32_t i = 0; i < job_count; i++) {
        test_data[i] = i;
        decal[i].job = test_job;
        decal[i].data = &test_data[i];
    }

    job_fence fence = job_kick(decal, job_count);
    job_wait_for_complete(fence);

    job_shutdown();

    TracyCZoneEnd(ctx);
}

void
test_2() {
    TracyCZoneN(ctx, "test_2", true);

    job_init(512, 512, 27);

    constexpr uint32_t job_count = 2;
    uint8_t test_data[job_count];
    job_decal decal[job_count];
     
    for (uint32_t i = 0; i < job_count; i++) {
        test_data[i] = i;
        decal[i].job = test_2_dep_job;
        decal[i].data = &test_data[i];
    }

    job_fence fence = job_kick(decal, job_count);

    test_2_job_data data[job_count];
    for (uint32_t i = 0; i < job_count; i++) {
        data[i].fence = fence;
        data[i].data = &test_data[i];
        decal[i].job = test_2_job;
        decal[i].data = &data[i];
    }

    fence = job_kick(decal, job_count);
    job_wait_for_complete(fence);

    job_shutdown();

    TracyCZoneEnd(ctx);
}

void
test_3() {
    TracyCZoneN(ctx, "test_3", true);

    job_init(512, 512, 27);

    constexpr uint32_t job_count = 100;
    uint8_t test_data[job_count];
    job_decal decal[job_count];

    for (uint32_t i = 0; i < job_count; i++) {
        test_data[i] = i;
        decal[i].job = test_3_dep_job;
        decal[i].data = &test_data[i];
    }

    job_fence fence = job_kick(decal, job_count);

    test_3_job_data data;
    data.fence = fence;
    data.data = test_data;
    decal[0].job = test_3_job;
    decal[0].data = &data;

    fence = job_kick(decal, 1);
    job_wait_for_complete(fence);

    job_shutdown();

    TracyCZoneEnd(ctx);
}

void
test_4() {
    TracyCZoneN(ctx, "test_4", true);

    job_init(512, 512, 27);

    job_decal decal;
    decal.data = nullptr;
    decal.job = test_4_job;

    job_fence fence = job_kick(&decal, 1);
    job_wait_for_complete(fence);

    job_shutdown();

    TracyCZoneEnd(ctx);
}

int main() {
    TracyCZoneN(ctx, "main", true);

    //test_1();
    //test_2();
    //test_3();
    test_4();

    TracyCZoneEnd(ctx);

    return 0;
}
