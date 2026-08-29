/*
 * libctest.c - the libc battery (phase 14, plan item 74).
 *
 * Exercises every libc subsystem headlessly and exits 0 only if all
 * checks pass: string/memory semantics, printf formatting (vs
 * snprintf golden strings), malloc patterns (bump, reuse, split,
 * free-list churn, realloc growth), the brk window, and pthread-lite
 * (two threads hammering a shared counter under a mutex, joined).
 *
 * The kernel's usertest spawns this program and asserts exit code 0
 * -- the same reaped-exit-code proof style as the phase-5 hello.
 */

#include "libc.h"

static int checks, fails;

#define CHECK(cond, name)                                     \
    do {                                                      \
        checks++;                                             \
        if (cond) {                                           \
            printf("libctest: %-38s ok\n", name);             \
        } else {                                              \
            printf("libctest: %-38s FAIL\n", name);           \
            fails++;                                          \
        }                                                     \
    } while (0)

static void test_string(void)
{
    char buf[64];

    memset(buf, 0, sizeof(buf));
    strcpy(buf, "abc");
    strcat(buf, "def");
    CHECK(strcmp(buf, "abcdef") == 0, "strcpy/strcat/strcmp");
    CHECK(strlen(buf) == 6, "strlen");
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp");
    CHECK(strchr(buf, 'd') && *strchr(buf, 'd') == 'd', "strchr");
    CHECK(memcmp("xyz", "xyz", 3) == 0 &&
          memcmp("xyz", "xyZ", 3) != 0, "memcmp");
    CHECK(strtoul("42", 0, 10) == 42 &&
          strtoul("0x1f", 0, 16) == 31, "strtoul");
    CHECK(strnlen("abc", 2) == 2, "strnlen");
}

static void test_printf(void)
{
    char b[128];

    snprintf(b, sizeof(b), "%d %u %x %s %c", -7, 42, 255, "hi", 'Z');
    CHECK(strcmp(b, "-7 42 ff hi Z") == 0, "snprintf basics");
    snprintf(b, sizeof(b), "%llu", 123456789012345ull);
    CHECK(strcmp(b, "123456789012345") == 0, "snprintf %llu");
    snprintf(b, sizeof(b), "[%5d]", 42);
    CHECK(strcmp(b, "[   42]") == 0, "snprintf width");
    snprintf(b, sizeof(b), "%p", (void *)0x1234);
    CHECK(strncmp(b, "0x", 2) == 0, "snprintf %p");
}

static void test_malloc(void)
{
    char *a, *b, *c, *d;

    a = malloc(100);
    CHECK(a != 0, "malloc: first block");
    memset(a, 'A', 100);
    CHECK(a[0] == 'A' && a[99] == 'A', "malloc: writable arena");

    b = malloc(200);
    CHECK(b && b != a, "malloc: disjoint blocks");

    free(a);
    c = malloc(64);
    CHECK(c == a, "malloc: free-list reuse");

    d = malloc(300);
    CHECK(d != 0, "malloc: grow arena");
    free(b);
    free(c);
    free(d);

    a = calloc(10, 10);
    CHECK(a != 0 && a[0] == 0 && a[99] == 0, "calloc: zeroed");
    a = realloc(a, 150);
    CHECK(a != 0 && a[0] == 0, "realloc: grow + preserved");
    free(a);
}

static void test_brk(void)
{
    char *cur = sbrk(0);
    char *nxt = sbrk(32);

    CHECK(nxt != (void *)-1 && nxt > cur, "sbrk: grow");
    CHECK(sbrk(0) >= nxt + 32, "sbrk: top advanced");
}

struct counter_work {
    pthread_mutex_t *lock;
    int *counter;
};

static void *counter_thread(void *arg)
{
    struct counter_work *w = arg;

    for (int i = 0; i < 500; i++) {
        pthread_mutex_lock(w->lock);
        (*w->counter)++;
        pthread_mutex_unlock(w->lock);
        sleep_ms(1);            /* yield so both threads progress  */
    }
    return w;
}

static void test_pthread(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_t t1, t2;
    int counter = 0;
    struct counter_work w = { &lock, &counter };
    void *r1 = 0, *r2 = 0;

    CHECK(pthread_create(&t1, 0, counter_thread, &w) == 0,
          "pthread: create t1");
    CHECK(pthread_create(&t2, 0, counter_thread, &w) == 0,
          "pthread: create t2");

    /* mutex: uncontended lock/unlock round trip                    */
    pthread_mutex_lock(&lock);
    counter++;
    pthread_mutex_unlock(&lock);

    pthread_join(t1, &r1);
    pthread_join(t2, &r2);
    CHECK(counter == 1001, "pthread: joined, counter exact");
    CHECK(r1 == &w && r2 == &w, "pthread: retvals");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("libctest: libc battery online (pid %d)\n", getpid());
    test_string();
    test_printf();
    test_malloc();
    test_brk();
    test_pthread();

    printf("libctest: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
