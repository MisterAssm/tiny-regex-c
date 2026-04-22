#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <stdatomic.h>
#include "re.h"

/* Test configuration */
#define NUM_THREADS        8       /* Number of concurrent threads */
#define ITERATIONS         10000   /* Iterations per thread */
#define PATTERNS_PER_ITER  5       /* Patterns compiled per iteration */

/* Test patterns of varying complexity */
static const char* test_patterns[] = {
    "\\d+",                              /* Simple digit matching */
    "[a-zA-Z]+",                         /* Word characters */
    "^\\w+@\\w+\\.\\w+$",                /* Simple email-like */
    "\\s+",                              /* Whitespace */
    "[0-9]{2,4}-[0-9]{2}-[0-9]{2,4}",    /* Date-like pattern */
    "\\d+\\.\\d+",                       /* Decimal numbers */
    "[a-z]+\\s+[a-z]+",                  /* Two words */
    "^\\d+$",                            /* Only digits */
    "\\w*\\d\\w*",                       /* Word with digit */
    "[aeiou]+",                          /* Vowels */
    "[^aeiou]+",                         /* Non-vowels */
    "\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}",    /* IP-like */
    "^[A-Z][a-z]+$",                     /* Capitalized word */
    "\\s*\\w+\\s*",                      /* Word with optional spaces */
    "[a-z]*[0-9][a-z]*",                 /* Mixed alphanumeric */
};

#define NUM_TEST_PATTERNS (sizeof(test_patterns) / sizeof(test_patterns[0]))

/* Test texts */
static const char* test_texts[] = {
    "Hello world 123 test",
    "The quick brown fox jumps over the lazy dog",
    "Email: test@example.com",
    "123-45-6789",
    "192.168.1.1",
    "   whitespace   everywhere   ",
    "UPPER lower MiXeD",
    "abc123def456ghi789",
    "no digits here",
    "42 is the answer",
    "AeIoU vowels",
    "BCDFGHJK consonants",
    "Mixed123With456Numbers",
    "test@domain.org",
    "  surrounded  ",
};

#define NUM_TEST_TEXTS (sizeof(test_texts) / sizeof(test_texts[0]))

/* Statistics per thread */
typedef struct {
    int thread_id;
    unsigned long patterns_compiled;
    unsigned long patterns_matched;
    unsigned long match_failures;
    unsigned long null_compiles;
} thread_stats_t;

static thread_stats_t stats[NUM_THREADS];

/* Simple barrier implementation using atomics (macOS compatible) */
typedef struct {
    atomic_int count;
    atomic_int generation;
    int target;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} simple_barrier_t;

static simple_barrier_t barrier;

static void barrier_init(simple_barrier_t* b, int count)
{
    b->target = count;
    atomic_init(&b->count, count);
    atomic_init(&b->generation, 0);
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
}

static void barrier_destroy(simple_barrier_t* b)
{
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

static void barrier_wait(simple_barrier_t* b)
{
    pthread_mutex_lock(&b->mutex);
    int gen = atomic_load(&b->generation);
    int cnt = atomic_fetch_sub(&b->count, 1) - 1;

    if (cnt == 0)
    {
        /* Last thread - reset and signal */
        atomic_store(&b->count, b->target);
        atomic_fetch_add(&b->generation, 1);
        pthread_cond_broadcast(&b->cond);
    }
    else
    {
        /* Wait for generation to change */
        while (atomic_load(&b->generation) == gen)
        {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
    }
    pthread_mutex_unlock(&b->mutex);
}

/* Get current time in microseconds */
static unsigned long long get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* Worker thread function */
static void* worker_thread(void* arg)
{
    thread_stats_t* stat = (thread_stats_t*)arg;
    unsigned int seed = (unsigned int)(get_time_us() ^ (stat->thread_id * 1000));

    /* Wait for all threads to be ready */
    barrier_wait(&barrier);

    for (int iter = 0; iter < ITERATIONS; iter++)
    {
        /* Compile multiple patterns */
        re_t patterns[PATTERNS_PER_ITER];
        int pattern_indices[PATTERNS_PER_ITER];

        for (int p = 0; p < PATTERNS_PER_ITER; p++)
        {
            /* Each thread uses different patterns based on its ID */
            int pattern_idx = (stat->thread_id + iter + p) % NUM_TEST_PATTERNS;
            pattern_indices[p] = pattern_idx;

            patterns[p] = re_compile(test_patterns[pattern_idx]);
            stat->patterns_compiled++;

            if (patterns[p] == NULL)
            {
                stat->null_compiles++;
                fprintf(stderr, "Thread %d: Failed to compile pattern '%s'\n",
                        stat->thread_id, test_patterns[pattern_idx]);
            }
        }

        /* Match against random texts */
        for (int p = 0; p < PATTERNS_PER_ITER; p++)
        {
            if (patterns[p] == NULL) continue;

            int text_idx = rand_r(&seed) % NUM_TEST_TEXTS;
            int match_len;

            int result = re_matchp(patterns[p], test_texts[text_idx], &match_len);

            if (result >= 0)
            {
                stat->patterns_matched++;
            }
            else
            {
                stat->match_failures++;
            }
        }

        /* Free all patterns - this is where race conditions would occur
         * with the old static buffer implementation */
        for (int p = 0; p < PATTERNS_PER_ITER; p++)
        {
            free(patterns[p]);
        }

        /* Occasional progress report */
        if (iter % 1000 == 0 && iter > 0)
        {
            printf("Thread %d: Completed %d/%d iterations\n",
                   stat->thread_id, iter, ITERATIONS);
        }
    }

    return NULL;
}

/* Stress test: Rapid compile/free cycles */
static void* stress_compile_thread(void* arg)
{
    thread_stats_t* stat = (thread_stats_t*)arg;

    barrier_wait(&barrier);

    for (int i = 0; i < ITERATIONS * 2; i++)
    {
        /* Rapidly compile and free */
        int pattern_idx = (stat->thread_id + i) % NUM_TEST_PATTERNS;
        re_t regex = re_compile(test_patterns[pattern_idx]);

        if (regex)
        {
            /* Immediate free - tests allocator stress */
            free(regex);
        }

        stat->patterns_compiled++;
    }

    return NULL;
}

/* Concurrent compile test: All threads compile simultaneously */
static void* concurrent_compile_thread(void* arg)
{
    thread_stats_t* stat = (thread_stats_t*)arg;
    re_t regexes[20];

    barrier_wait(&barrier);

    for (int round = 0; round < 100; round++)
    {
        /* All threads compile at the same time (after barrier) */
        for (int i = 0; i < 20; i++)
        {
            int pattern_idx = (stat->thread_id + round + i) % NUM_TEST_PATTERNS;
            regexes[i] = re_compile(test_patterns[pattern_idx]);
            stat->patterns_compiled++;
        }

        /* Small delay to increase chance of overlap */
        usleep(1);

        /* Free all */
        for (int i = 0; i < 20; i++)
        {
            free(regexes[i]);
        }
    }

    return NULL;
}

/* Run a test with given worker function */
static int run_test(const char* test_name, void* (*worker)(void*))
{
    pthread_t threads[NUM_THREADS];
    unsigned long long start_time, end_time;

    printf("\n========================================\n");
    printf("Test: %s\n", test_name);
    printf("Threads: %d, Iterations: %d\n", NUM_THREADS, ITERATIONS);
    printf("========================================\n");

    /* Reset stats */
    memset(stats, 0, sizeof(stats));

    /* Initialize barrier */
    barrier_init(&barrier, NUM_THREADS);

    start_time = get_time_us();

    /* Create threads */
    for (int i = 0; i < NUM_THREADS; i++)
    {
        stats[i].thread_id = i;
        int ret = pthread_create(&threads[i], NULL, worker, &stats[i]);
        if (ret != 0)
        {
            perror("pthread_create");
            return 1;
        }
    }

    /* Wait for all threads */
    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }

    end_time = get_time_us();

    barrier_destroy(&barrier);

    /* Aggregate statistics */
    unsigned long total_compiled = 0;
    unsigned long total_matched = 0;
    unsigned long total_failures = 0;
    unsigned long total_null = 0;

    for (int i = 0; i < NUM_THREADS; i++)
    {
        total_compiled += stats[i].patterns_compiled;
        total_matched += stats[i].patterns_matched;
        total_failures += stats[i].match_failures;
        total_null += stats[i].null_compiles;
    }

    double elapsed_ms = (double)(end_time - start_time) / 1000.0;
    double ops_per_sec = (double)total_compiled / (elapsed_ms / 1000.0);

    printf("\nResults:\n");
    printf("  Time elapsed: %.2f ms\n", elapsed_ms);
    printf("  Patterns compiled: %lu\n", total_compiled);
    printf("  Matches found: %lu\n", total_matched);
    printf("  Match failures: %lu\n", total_failures);
    printf("  Null compiles: %lu\n", total_null);
    printf("  Operations/sec: %.0f\n", ops_per_sec);
    printf("  Result: %s\n", total_null == 0 ? "PASS" : "FAIL");

    return (total_null > 0) ? 1 : 0;
}

/* Thread data for isolation test */
typedef struct {
    int idx;
    const char* pattern;
    re_t* results;
} isolation_data_t;

static simple_barrier_t isolation_barrier;

static void* compile_pattern(void* arg)
{
    isolation_data_t* data = (isolation_data_t*)arg;
    barrier_wait(&isolation_barrier);
    data->results[data->idx] = re_compile(data->pattern);
    return NULL;
}

/* Test: Verify thread isolation - compile same pattern in multiple threads */
static int test_thread_isolation(void)
{
    printf("\n========================================\n");
    printf("Test: Thread Isolation\n");
    printf("========================================\n");

    pthread_t threads[NUM_THREADS];
    re_t results[NUM_THREADS];
    const char* pattern = "\\d+[a-z]+\\d+";  /* Complex pattern */
    isolation_data_t thread_data[NUM_THREADS];

    barrier_init(&isolation_barrier, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++)
    {
        thread_data[i].idx = i;
        thread_data[i].pattern = pattern;
        thread_data[i].results = results;
        pthread_create(&threads[i], NULL, compile_pattern, &thread_data[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }

    barrier_destroy(&isolation_barrier);

    /* Verify all compilations succeeded */
    int success = 1;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        if (results[i] == NULL)
        {
            fprintf(stderr, "Thread %d: Compilation failed!\n", i);
            success = 0;
            continue;
        }

        /* Each result should be independent (different pointers) */
        for (int j = i + 1; j < NUM_THREADS; j++)
        {
            if (results[i] == results[j])
            {
                fprintf(stderr, "Thread %d and %d: Same pointer! Race condition!\n", i, j);
                success = 0;
            }
        }

        /* Test that each regex works correctly */
        int match_len;
        int result = re_matchp(results[i], "123abc456", &match_len);
        if (result < 0)
        {
            fprintf(stderr, "Thread %d: Pattern didn't match!\n", i);
            success = 0;
        }

        free(results[i]);
    }

    printf("  Result: %s\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}

/* Test: Memory leak detection helper - allocate and don't free */
static int test_memory_leak_detection(void)
{
    printf("\n========================================\n");
    printf("Test: Memory Leak Detection (manual check)\n");
    printf("========================================\n");
    printf("Compile %d patterns and free them all...\n", 1000);

    for (int i = 0; i < 1000; i++)
    {
        re_t r = re_compile(test_patterns[i % NUM_TEST_PATTERNS]);
        if (r) free(r);
    }

    printf("  Result: PASS (check with valgrind --leak-check=full)\n");
    return 0;
}

int main(void)
{
    int total_failures = 0;

    printf("========================================\n");
    printf("tiny-regex-c Thread Safety Test\n");
    printf("========================================\n");
    printf("Build: %s %s\n", __DATE__, __TIME__);
    printf("Threads: %d\n", NUM_THREADS);
    printf("Iterations per thread: %d\n", ITERATIONS);
    printf("\n");

    total_failures += run_test("Standard Compile/Match/Free", worker_thread);
    total_failures += run_test("Rapid Compile/Free Stress", stress_compile_thread);
    total_failures += run_test("Concurrent Compilation", concurrent_compile_thread);
    total_failures += test_thread_isolation();
    total_failures += test_memory_leak_detection();

    printf("\n========================================\n");
    printf("Final Result: %s\n", total_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    printf("========================================\n");

    return total_failures;
}
