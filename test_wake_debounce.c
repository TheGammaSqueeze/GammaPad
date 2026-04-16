/*****************************************************
 * test_wake_debounce.c
 *
 * Unit test harness for the Issue #249 wake debounce fix.
 *
 * This is a standalone test that can be compiled and run on any
 * Linux system (or cross-compiled for Android) to verify the
 * debounce logic WITHOUT requiring actual hardware.
 *
 * Compile:
 *   gcc -o test_wake_debounce test_wake_debounce.c -lpthread
 *
 * Run:
 *   ./test_wake_debounce
 *
 * Expected output: all tests PASS.
 *****************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>

/* ── Mock the minimal GammaPad environment ─────────────── */

#define ABS_MAX     0x3f
#define ABS_X       0x00
#define ABS_Y       0x01
#define ABS_Z       0x02
#define ABS_RX      0x03
#define ABS_RY      0x04
#define ABS_RZ      0x05
#define ABS_GAS     0x09
#define ABS_BRAKE   0x0a
#define ABS_HAT0X   0x10
#define ABS_HAT0Y   0x11

#define EV_SYN      0x00
#define EV_KEY      0x01
#define EV_ABS      0x03
#define SYN_REPORT  0x00

#define KEY_MAX     0x2ff
#define BTN_A       0x130

struct input_event {
    struct timeval time;
    unsigned short type;
    unsigned short code;
    int value;
};

/* Simulated globals */
volatile unsigned long long g_wakeTimestampMs = 0;
int g_wakeDebounceMs = 500;
int controllerFd = 1; /* fake valid fd */

/* Simulated axis ranges */
static int phys_abs_min[ABS_MAX+1];
static int phys_abs_max[ABS_MAX+1];
static int last_processed_abs[ABS_MAX+1];

/* Track what gets forwarded */
static int forwarded_abs_count = 0;
static int forwarded_key_count = 0;
static int last_forwarded_abs_code = -1;
static int last_forwarded_abs_value = -1;
static int centered_axes_count = 0;

/* Mock time: we control it for deterministic tests */
static unsigned long long mock_time_ms = 1000000ULL; /* start at 1,000,000ms */

static unsigned long long getTimeMs(void)
{
    return mock_time_ms;
}

static void advance_time(unsigned long long ms)
{
    mock_time_ms += ms;
}

int getPhysicalAbsMin(int sc)
{
    if (sc < 0 || sc > ABS_MAX) return -32768;
    return phys_abs_min[sc];
}

int getPhysicalAbsMax(int sc)
{
    if (sc < 0 || sc > ABS_MAX) return 32767;
    return phys_abs_max[sc];
}

/* ── Simulated forward_physical_event (matches our fix) ── */

static void forward_physical_event(const struct input_event* ev)
{
    if (!ev || controllerFd < 0) return;

    /* Issue #249 fix: Wake debounce */
    if (g_wakeTimestampMs > 0 && ev->type == EV_ABS) {
        unsigned long long now = getTimeMs();
        unsigned long long elapsed = now - g_wakeTimestampMs;
        if (elapsed < (unsigned long long)g_wakeDebounceMs) {
            /* Suppressed — stale event dropped */
            return;
        }
        g_wakeTimestampMs = 0;
    }

    if (ev->type == EV_KEY) {
        forwarded_key_count++;
    }
    else if (ev->type == EV_ABS) {
        forwarded_abs_count++;
        last_forwarded_abs_code = ev->code;
        last_forwarded_abs_value = ev->value;
    }
}

/* ── Simulated recapture_primary_post_actions (matches fix) ── */

static void recapture_primary_post_actions(void)
{
    g_wakeTimestampMs = getTimeMs();

    /* Force axes to center */
    if (controllerFd >= 0) {
        static const int axes_to_center[] = {
            ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_RX, ABS_RY,
            ABS_GAS, ABS_BRAKE, ABS_HAT0X, ABS_HAT0Y
        };
        int num_axes = (int)(sizeof(axes_to_center) / sizeof(axes_to_center[0]));
        centered_axes_count = num_axes;
        for (int i = 0; i < num_axes; i++) {
            int sc = axes_to_center[i];
            int mn = getPhysicalAbsMin(sc);
            int mx = getPhysicalAbsMax(sc);
            int center = (mn + mx) / 2;
            /* In real code this does write(controllerFd, ...) */
            last_processed_abs[sc] = center;
        }
    }

    /* Reset deadzone state */
    for (int i = 0; i <= ABS_MAX; i++) {
        int mn = getPhysicalAbsMin(i);
        int mx = getPhysicalAbsMax(i);
        last_processed_abs[i] = (mn + mx) / 2;
    }
}

/* ── Test Helpers ──────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        printf("  TEST: %-55s ", name); \
        fflush(stdout); \
    } while(0)

#define PASS() \
    do { \
        printf("✓ PASS\n"); \
        tests_passed++; \
    } while(0)

#define FAIL(reason) \
    do { \
        printf("✗ FAIL: %s\n", reason); \
        tests_failed++; \
    } while(0)

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        if ((actual) != (expected)) { \
            char _buf[256]; \
            snprintf(_buf, sizeof(_buf), "%s (got %d, expected %d)", msg, (int)(actual), (int)(expected)); \
            FAIL(_buf); \
            return; \
        } \
    } while(0)

static void reset_counters(void)
{
    forwarded_abs_count = 0;
    forwarded_key_count = 0;
    last_forwarded_abs_code = -1;
    last_forwarded_abs_value = -1;
    centered_axes_count = 0;
    g_wakeTimestampMs = 0;
    g_wakeDebounceMs = 500;
    mock_time_ms = 1000000ULL;
}

static void init_axes(void)
{
    for (int i = 0; i <= ABS_MAX; i++) {
        phys_abs_min[i] = -32768;
        phys_abs_max[i] = 32767;
        last_processed_abs[i] = 0;
    }
    /* Typical trigger range */
    phys_abs_min[ABS_GAS] = 0;
    phys_abs_max[ABS_GAS] = 255;
    phys_abs_min[ABS_BRAKE] = 0;
    phys_abs_max[ABS_BRAKE] = 255;
    /* Typical HAT range */
    phys_abs_min[ABS_HAT0X] = -1;
    phys_abs_max[ABS_HAT0X] = 1;
    phys_abs_min[ABS_HAT0Y] = -1;
    phys_abs_max[ABS_HAT0Y] = 1;
}

static struct input_event make_abs_event(int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_ABS;
    ev.code = code;
    ev.value = value;
    return ev;
}

static struct input_event make_key_event(int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    return ev;
}

/* ── TEST CASES ────────────────────────────────────────── */

/*
 * Test 1: Normal operation — no debounce active, events pass through.
 */
static void test_normal_forwarding(void)
{
    TEST("Normal ABS events pass through without debounce");
    reset_counters();
    init_axes();

    struct input_event ev = make_abs_event(ABS_X, 16000);
    forward_physical_event(&ev);

    ASSERT_EQ(forwarded_abs_count, 1, "ABS should be forwarded");
    ASSERT_EQ(last_forwarded_abs_code, ABS_X, "Code should be ABS_X");
    ASSERT_EQ(last_forwarded_abs_value, 16000, "Value should be 16000");
    PASS();
}

/*
 * Test 2: Normal KEY events pass through without debounce.
 */
static void test_normal_key_forwarding(void)
{
    TEST("Normal KEY events pass through without debounce");
    reset_counters();
    init_axes();

    struct input_event ev = make_key_event(BTN_A, 1);
    forward_physical_event(&ev);

    ASSERT_EQ(forwarded_key_count, 1, "KEY should be forwarded");
    PASS();
}

/*
 * Test 3: ABS events are SUPPRESSED during debounce window.
 */
static void test_abs_suppressed_during_debounce(void)
{
    TEST("ABS events suppressed during 500ms debounce window");
    reset_counters();
    init_axes();

    /* Simulate wake */
    recapture_primary_post_actions();

    /* Send ABS event immediately (0ms after wake) */
    struct input_event ev = make_abs_event(ABS_X, -20000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "ABS should be suppressed at +0ms");

    /* 100ms later — still suppressed */
    advance_time(100);
    ev = make_abs_event(ABS_Y, 30000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "ABS should be suppressed at +100ms");

    /* 400ms total — still within 500ms window */
    advance_time(300);
    ev = make_abs_event(ABS_RZ, 15000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "ABS should be suppressed at +400ms");

    PASS();
}

/*
 * Test 4: ABS events are ALLOWED after debounce window expires.
 */
static void test_abs_allowed_after_debounce(void)
{
    TEST("ABS events allowed after debounce window expires");
    reset_counters();
    init_axes();

    /* Simulate wake */
    recapture_primary_post_actions();

    /* Advance past debounce window (500ms + 1ms) */
    advance_time(501);

    struct input_event ev = make_abs_event(ABS_X, 5000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "ABS should be forwarded at +501ms");
    ASSERT_EQ(last_forwarded_abs_value, 5000, "Correct value forwarded");

    PASS();
}

/*
 * Test 5: KEY events STILL pass through during debounce.
 */
static void test_keys_pass_during_debounce(void)
{
    TEST("KEY events pass through during debounce window");
    reset_counters();
    init_axes();

    /* Simulate wake */
    recapture_primary_post_actions();

    /* Send KEY event immediately */
    struct input_event ev = make_key_event(BTN_A, 1);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_key_count, 1, "KEY should pass at +0ms");

    /* ABS should still be blocked */
    struct input_event abs_ev = make_abs_event(ABS_X, -10000);
    forward_physical_event(&abs_ev);
    ASSERT_EQ(forwarded_abs_count, 0, "ABS should be blocked at +0ms");

    PASS();
}

/*
 * Test 6: Debounce timestamp clears after first allowed event.
 */
static void test_timestamp_clears_after_debounce(void)
{
    TEST("Debounce timestamp clears after window expires");
    reset_counters();
    init_axes();

    /* Simulate wake */
    recapture_primary_post_actions();

    /* Advance past window */
    advance_time(600);

    /* First event clears the timestamp */
    struct input_event ev = make_abs_event(ABS_X, 100);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "First ABS after window");
    ASSERT_EQ(g_wakeTimestampMs, 0ULL, "Timestamp should be cleared");

    /* Subsequent events should pass without any timing check */
    ev = make_abs_event(ABS_Y, 200);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 2, "Second ABS should pass freely");

    PASS();
}

/*
 * Test 7: Zero debounce (disabled) — events always pass.
 */
static void test_zero_debounce_disabled(void)
{
    TEST("Setting debounce to 0 disables suppression");
    reset_counters();
    init_axes();
    g_wakeDebounceMs = 0;

    /* Simulate wake */
    recapture_primary_post_actions();

    /* Send ABS immediately — should pass because 0ms window */
    struct input_event ev = make_abs_event(ABS_X, -30000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "ABS should pass with 0ms debounce");

    PASS();
}

/*
 * Test 8: Custom debounce value (1000ms).
 */
static void test_custom_debounce_value(void)
{
    TEST("Custom debounce value (1000ms) works correctly");
    reset_counters();
    init_axes();
    g_wakeDebounceMs = 1000;

    recapture_primary_post_actions();

    /* 500ms — still suppressed with 1000ms window */
    advance_time(500);
    struct input_event ev = make_abs_event(ABS_X, 10000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "Suppressed at +500ms (1000ms window)");

    /* 999ms — still suppressed */
    advance_time(499);
    ev = make_abs_event(ABS_X, 10000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "Suppressed at +999ms");

    /* 1001ms — allowed */
    advance_time(2);
    ev = make_abs_event(ABS_X, 10000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "Allowed at +1001ms");

    PASS();
}

/*
 * Test 9: Recapture resets the debounce window.
 */
static void test_recapture_resets_debounce(void)
{
    TEST("Second recapture resets the debounce window");
    reset_counters();
    init_axes();

    recapture_primary_post_actions();
    advance_time(300); /* 300ms into first window */

    /* Second recapture at +300ms */
    recapture_primary_post_actions();

    /* Now we need another 500ms from the SECOND recapture */
    advance_time(400); /* +700ms from start, +400ms from second recapture */
    struct input_event ev = make_abs_event(ABS_X, 5000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "Still suppressed (400ms since 2nd recapture)");

    /* +600ms from second recapture — allowed */
    advance_time(200);
    ev = make_abs_event(ABS_X, 5000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "Allowed (600ms since 2nd recapture)");

    PASS();
}

/*
 * Test 10: Axis centering on recapture works correctly.
 */
static void test_axis_centering_on_recapture(void)
{
    TEST("All axes forced to center on recapture");
    reset_counters();
    init_axes();

    /* Set some non-center values first */
    last_processed_abs[ABS_X] = -20000;
    last_processed_abs[ABS_Y] = 30000;
    last_processed_abs[ABS_GAS] = 200;
    last_processed_abs[ABS_HAT0X] = -1;

    recapture_primary_post_actions();

    /* Sticks should be centered: (-32768 + 32767) / 2 = -1 ≈ 0 */
    int stick_center = (-32768 + 32767) / 2;
    ASSERT_EQ(last_processed_abs[ABS_X], stick_center, "ABS_X centered");
    ASSERT_EQ(last_processed_abs[ABS_Y], stick_center, "ABS_Y centered");

    /* Trigger centered: (0 + 255) / 2 = 127 */
    ASSERT_EQ(last_processed_abs[ABS_GAS], 127, "ABS_GAS centered");

    /* HAT centered: (-1 + 1) / 2 = 0 */
    ASSERT_EQ(last_processed_abs[ABS_HAT0X], 0, "ABS_HAT0X centered");

    ASSERT_EQ(centered_axes_count, 10, "10 axes were centered");

    PASS();
}

/*
 * Test 11: NULL event doesn't crash.
 */
static void test_null_event_safety(void)
{
    TEST("NULL event pointer doesn't crash");
    reset_counters();
    init_axes();

    forward_physical_event(NULL);
    ASSERT_EQ(forwarded_abs_count, 0, "Nothing forwarded");
    ASSERT_EQ(forwarded_key_count, 0, "Nothing forwarded");
    PASS();
}

/*
 * Test 12: Invalid controllerFd doesn't crash.
 */
static void test_invalid_fd_safety(void)
{
    TEST("Invalid controllerFd (-1) doesn't crash");
    reset_counters();
    init_axes();
    controllerFd = -1;

    struct input_event ev = make_abs_event(ABS_X, 5000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "Nothing forwarded with fd=-1");

    controllerFd = 1; /* restore */
    PASS();
}

/*
 * Test 13: Rapid wake/sleep cycles don't accumulate weirdly.
 */
static void test_rapid_wake_sleep_cycles(void)
{
    TEST("Rapid wake/sleep cycles handle correctly");
    reset_counters();
    init_axes();

    /* 5 rapid recaptures with 50ms between each */
    for (int i = 0; i < 5; i++) {
        recapture_primary_post_actions();
        advance_time(50);
    }

    /* Should still need 500ms from the LAST recapture */
    struct input_event ev = make_abs_event(ABS_X, 10000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "Suppressed after rapid recaptures");

    /* Advance 500ms past the last recapture */
    advance_time(501);
    ev = make_abs_event(ABS_X, 10000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "Allowed 500ms after last recapture");

    PASS();
}

/*
 * Test 14: Triggers (ABS_GAS, ABS_BRAKE) are also suppressed.
 * (This directly tests the user report of "RT phantom press")
 */
static void test_trigger_suppression(void)
{
    TEST("Triggers (GAS/BRAKE) suppressed during debounce");
    reset_counters();
    init_axes();

    recapture_primary_post_actions();

    struct input_event ev = make_abs_event(ABS_GAS, 255);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "ABS_GAS suppressed");

    ev = make_abs_event(ABS_BRAKE, 200);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "ABS_BRAKE suppressed");

    PASS();
}

/*
 * Test 15: Maximum debounce value (2000ms) works.
 */
static void test_max_debounce_value(void)
{
    TEST("Maximum debounce value (2000ms) respected");
    reset_counters();
    init_axes();
    g_wakeDebounceMs = 2000;

    recapture_primary_post_actions();

    advance_time(1999);
    struct input_event ev = make_abs_event(ABS_X, 5000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 0, "Suppressed at +1999ms");

    advance_time(2);
    ev = make_abs_event(ABS_X, 5000);
    forward_physical_event(&ev);
    ASSERT_EQ(forwarded_abs_count, 1, "Allowed at +2001ms");

    PASS();
}

/* ── MAIN ──────────────────────────────────────────────── */

int main(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  GammaPad Wake Debounce Test Suite (Issue #249 Fix)         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    test_normal_forwarding();
    test_normal_key_forwarding();
    test_abs_suppressed_during_debounce();
    test_abs_allowed_after_debounce();
    test_keys_pass_during_debounce();
    test_timestamp_clears_after_debounce();
    test_zero_debounce_disabled();
    test_custom_debounce_value();
    test_recapture_resets_debounce();
    test_axis_centering_on_recapture();
    test_null_event_safety();
    test_invalid_fd_safety();
    test_rapid_wake_sleep_cycles();
    test_trigger_suppression();
    test_max_debounce_value();

    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  Results: %d PASSED, %d FAILED out of %d tests\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("══════════════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
