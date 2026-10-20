/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include <common/test.h>
#include <common/test_helpers.h>
#include <pthread.h>
#include <unistd.h>

extern "C" {
#include <ucm/api/ucm.h>
#include <ucm/bistro/bistro.h>
#include <ucs/arch/atomic.h>
#include <ucs/time/time.h>
}


#define BISTRO_BATCH_NUM_FUNCS 8u

typedef int (*bistro_batch_func_t)(int);

/* The volatile loop makes the body longer than the longest patch */
template<int N> UCS_F_NOINLINE int bistro_batch_func(int x)
{
    volatile int value = x;

    for (int i = 0; i < 4; ++i) {
        value = (value * 3) + N;
    }

    return value;
}

static volatile uint32_t bistro_batch_hook_calls = 0;

template<int N> int bistro_batch_hook(int x)
{
    UCM_BISTRO_PROLOGUE;
    ucs_atomic_add32(&bistro_batch_hook_calls, 1);
    UCM_BISTRO_EPILOGUE;
    return -1000 - N;
}

/* Volatile so that the calls are not inlined or folded */
static bistro_batch_func_t volatile bistro_batch_funcs[] = {
    bistro_batch_func<0>, bistro_batch_func<1>, bistro_batch_func<2>,
    bistro_batch_func<3>, bistro_batch_func<4>, bistro_batch_func<5>,
    bistro_batch_func<6>, bistro_batch_func<7>
};

static bistro_batch_func_t bistro_batch_hooks[] = {
    bistro_batch_hook<0>, bistro_batch_hook<1>, bistro_batch_hook<2>,
    bistro_batch_hook<3>, bistro_batch_hook<4>, bistro_batch_hook<5>,
    bistro_batch_hook<6>, bistro_batch_hook<7>
};

static_assert(ucs_static_array_size(bistro_batch_funcs) ==
                      BISTRO_BATCH_NUM_FUNCS, "bistro_batch_funcs");
static_assert(ucs_static_array_size(bistro_batch_hooks) ==
                      BISTRO_BATCH_NUM_FUNCS, "bistro_batch_hooks");

/* Patches are applied immediately where there is no patch lock (ppc64) */
#define BISTRO_BATCH_SKIP \
    (RUNNING_ON_VALGRIND || !ucm_bistro_batch_is_deferred())


class test_bistro_batch : public ucs::test {
protected:
    typedef std::vector<ucm_bistro_restore_point_t*> restore_points_t;

    virtual void init()
    {
        ucs::test::init();
        m_grace_duration = ucm_global_opts.bistro_grace_duration;

        for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
            for (unsigned j = 0; j < i; ++j) {
                if (labs(UCS_PTR_BYTE_DIFF(func_ptr(i), func_ptr(j))) <
                    UCM_BISTRO_PATCH_MAX_LEN) {
                    UCS_TEST_SKIP_R("test functions are too close");
                }
            }
        }
    }

    virtual void cleanup()
    {
        ucm_global_opts.bistro_grace_duration = m_grace_duration;
        ucs::test::cleanup();
    }

    static int call(unsigned index)
    {
        return bistro_batch_funcs[index](1);
    }

    static bool is_hooked(unsigned index)
    {
        return call(index) == (-1000 - (int)index);
    }

    static void *func_ptr(unsigned index)
    {
        return (void*)bistro_batch_funcs[index];
    }

    /* Returns the commit time in seconds, or -1 on failure */
    static double patch_batch(restore_points_t &rps)
    {
        ucm_bistro_batch_entry_t entries[BISTRO_BATCH_NUM_FUNCS];
        ucm_bistro_batch_t batch;
        ucs_status_t status;
        double start;

        rps.assign(BISTRO_BATCH_NUM_FUNCS, NULL);
        ucm_bistro_batch_init(&batch, entries, BISTRO_BATCH_NUM_FUNCS);
        for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
            status = ucm_bistro_batch_add(&batch, func_ptr(i),
                                          (void*)bistro_batch_hooks[i],
                                          "bistro_batch_func", NULL, &rps[i]);
            if (status != UCS_OK) {
                ucm_bistro_batch_cancel(&batch);
                return -1;
            }
        }

        start  = ucs_get_accurate_time();
        status = ucm_bistro_batch_commit(&batch);
        return (status == UCS_OK) ? (ucs_get_accurate_time() - start) : -1;
    }

    static double patch_single(restore_points_t &rps)
    {
        double start = ucs_get_accurate_time();

        rps.assign(BISTRO_BATCH_NUM_FUNCS, NULL);
        for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
            if (ucm_bistro_patch(func_ptr(i), (void*)bistro_batch_hooks[i],
                                 "bistro_batch_func", NULL,
                                 &rps[i]) != UCS_OK) {
                return -1;
            }
        }

        return ucs_get_accurate_time() - start;
    }

    static bool restore(restore_points_t &rps)
    {
        bool success = true;

        for (unsigned i = 0; i < rps.size(); ++i) {
            if ((rps[i] != NULL) && (ucm_bistro_restore(rps[i]) != UCS_OK)) {
                success = false;
            }
        }

        rps.clear();
        return success;
    }

    double m_grace_duration;
};

UCS_TEST_SKIP_COND_F(test_bistro_batch, patch_and_restore, BISTRO_BATCH_SKIP)
{
    ucm_bistro_batch_entry_t entries[BISTRO_BATCH_NUM_FUNCS + 1];
    restore_points_t rps(BISTRO_BATCH_NUM_FUNCS, NULL);
    void *orig_func = NULL;
    std::vector<int> orig_values;
    ucm_bistro_batch_t batch;
    ucs_status_t status;

    for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
        orig_values.push_back(call(i));
        ASSERT_FALSE(is_hooked(i));
    }

    ucm_global_opts.bistro_grace_duration = 1e-3;
    ucm_bistro_batch_init(&batch, entries, ucs_static_array_size(entries));
    for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
        /* A trampoline needs relocatable code, which depends on the compiler */
        status = ucm_bistro_batch_add(&batch, func_ptr(i),
                                      (void*)bistro_batch_hooks[i],
                                      "bistro_batch_func",
                                      (i == 0) ? &orig_func : NULL, &rps[i]);
        if ((i == 0) && (status == UCS_ERR_UNSUPPORTED)) {
            UCS_TEST_MESSAGE << "no trampoline for bistro_batch_func<0>";
            status = ucm_bistro_batch_add(&batch, func_ptr(i),
                                          (void*)bistro_batch_hooks[i],
                                          "bistro_batch_func", NULL, &rps[i]);
        }
        ASSERT_UCS_OK(status);
    }

    EXPECT_EQ(UCS_ERR_ALREADY_EXISTS,
              ucm_bistro_batch_add(&batch, func_ptr(3),
                                   (void*)bistro_batch_hooks[3], "dup", NULL,
                                   NULL));

    /* Nothing is modified or published before commit */
    EXPECT_EQ(BISTRO_BATCH_NUM_FUNCS, batch.count);
    for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
        EXPECT_FALSE(is_hooked(i)) << i;
        EXPECT_TRUE(rps[i] == NULL) << i;
    }
    EXPECT_TRUE(orig_func == NULL);

    bistro_batch_hook_calls = 0;
    ASSERT_UCS_OK(ucm_bistro_batch_commit(&batch));
    EXPECT_EQ(0u, batch.count);
    for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
        EXPECT_TRUE(is_hooked(i)) << i;
        EXPECT_TRUE(rps[i] != NULL) << i;
    }
    EXPECT_EQ(BISTRO_BATCH_NUM_FUNCS, bistro_batch_hook_calls);

    if (orig_func != NULL) {
        EXPECT_EQ(orig_values[0], ((bistro_batch_func_t)orig_func)(1));
    }

    ASSERT_TRUE(restore(rps));
    for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
        EXPECT_EQ(orig_values[i], call(i)) << i;
    }
}

UCS_TEST_SKIP_COND_F(test_bistro_batch, cancel, BISTRO_BATCH_SKIP)
{
    ucm_bistro_batch_entry_t entries[2];
    ucm_bistro_restore_point_t *rp = NULL;
    ucm_bistro_batch_t batch;

    ucm_bistro_batch_init(&batch, entries, 2);
    ASSERT_UCS_OK(ucm_bistro_batch_add(&batch, func_ptr(0),
                                       (void*)bistro_batch_hooks[0], "f0",
                                       NULL, &rp));
    ASSERT_UCS_OK(ucm_bistro_batch_add(&batch, func_ptr(1),
                                       (void*)bistro_batch_hooks[1], "f1",
                                       NULL, NULL));
    EXPECT_EQ(UCS_ERR_EXCEEDS_LIMIT,
              ucm_bistro_batch_add(&batch, func_ptr(2),
                                   (void*)bistro_batch_hooks[2], "f2", NULL,
                                   NULL));

    ucm_bistro_batch_cancel(&batch);
    EXPECT_EQ(0u, batch.count);
    EXPECT_TRUE(rp == NULL);
    EXPECT_FALSE(is_hooked(0));
    EXPECT_FALSE(is_hooked(1));

    /* An empty batch commits without waiting */
    ucm_global_opts.bistro_grace_duration = 10.0;
    EXPECT_UCS_OK(ucm_bistro_batch_commit(&batch));
}

UCS_TEST_SKIP_COND_F(test_bistro_batch, one_grace_period, BISTRO_BATCH_SKIP)
{
    const double grace = 50e-3;
    double single_time, batch_time;
    restore_points_t rps;

    ucm_global_opts.bistro_grace_duration = grace;

    single_time = patch_single(rps);
    ASSERT_GE(single_time, 0);
    ASSERT_TRUE(restore(rps));

    batch_time = patch_batch(rps);
    ASSERT_GE(batch_time, 0);
    for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
        EXPECT_TRUE(is_hooked(i)) << i;
    }
    ASSERT_TRUE(restore(rps));

    UCS_TEST_MESSAGE << "patching " << BISTRO_BATCH_NUM_FUNCS
                     << " functions: single " << (single_time * 1e3)
                     << " ms, batch " << (batch_time * 1e3) << " ms";
    EXPECT_GE(single_time, BISTRO_BATCH_NUM_FUNCS * grace);
    EXPECT_GE(batch_time, grace);
    EXPECT_LT(batch_time, single_time / 2);
}

class bistro_batch_caller {
public:
    bistro_batch_caller() : m_stop(false), m_hooked_mask(0)
    {
        m_started = (pthread_create(&m_thread, NULL, thread_func, this) == 0);
    }

    ~bistro_batch_caller()
    {
        stop();
    }

    bool started() const
    {
        return m_started;
    }

    void stop()
    {
        if (m_started && !m_stop) {
            m_stop = true;
            pthread_join(m_thread, NULL);
        }
    }

    bool all_hooked() const
    {
        return m_hooked_mask == UCS_MASK(BISTRO_BATCH_NUM_FUNCS);
    }

private:
    static void *thread_func(void *arg)
    {
        bistro_batch_caller *self = (bistro_batch_caller*)arg;

        while (!self->m_stop) {
            for (unsigned i = 0; i < BISTRO_BATCH_NUM_FUNCS; ++i) {
                if (bistro_batch_funcs[i](1) < 0) {
                    self->m_hooked_mask |= UCS_BIT(i);
                }
            }
        }
        return NULL;
    }

    pthread_t         m_thread;
    bool              m_started;
    volatile bool     m_stop;
    volatile unsigned m_hooked_mask;
};

UCS_TEST_SKIP_COND_F(test_bistro_batch, patch_while_called, BISTRO_BATCH_SKIP)
{
    const int num_threads = 4;
    bistro_batch_caller callers[num_threads];
    restore_points_t rps;
    ucs_time_t deadline;
    bool all_hooked;

    for (int i = 0; i < num_threads; ++i) {
        ASSERT_TRUE(callers[i].started());
    }

    /* Default grace period, while threads call the patched functions */
    ASSERT_GE(patch_batch(rps), 0);

    deadline = ucs_get_time() + ucs_time_from_sec(10.0);
    do {
        all_hooked = true;
        for (int i = 0; i < num_threads; ++i) {
            all_hooked = all_hooked && callers[i].all_hooked();
        }
    } while (!all_hooked && (ucs_get_time() < deadline));

    for (int i = 0; i < num_threads; ++i) {
        callers[i].stop();
    }

    EXPECT_TRUE(all_hooked);
    ASSERT_TRUE(restore(rps));
}
