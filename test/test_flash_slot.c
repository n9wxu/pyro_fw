/*
 * The log writer's flash operations inside its slots (flash_slot.c), the
 * datasheet figures they are planned by (flash_timing.c), and core1's frame
 * (core1_frame.h), against a chip that keeps its datasheet's rules: a
 * suspend issued sooner than tRS after a start or resume is ignored, and a
 * suspend takes up to tSUS to land.
 *
 * Verifies [FLT-LOG-09, FLT-LOG-10, LUA-RUN-04].
 */
#include "unity.h"
#include "core1_frame.h"
#include "flash_slot.h"
#include "flash_timing.h"
#include <string.h>

#define IRQ_US 30u /* the slot-start interrupt, after the period's start */
/* The writer's task behind core1's other tasks of its priority, which its
 * notification does not preempt: as late as this on MK1B-8MB. */
#define TASK_WAKE_US 900u
/* What a piece costs beside the chip's own time, as measured on MK1C: each
 * command through the SDK's flash_do_cmd() hands XIP back, about 100 us. */
#define START_US 200u   /* write enable and the command */
#define POLL_US 100u    /* a status poll: the end is seen this late at most */
#define SUSPEND_US 400u /* the suspend, its settle and the polls after it */

static const uint8_t W25Q16JV[3] = {0xEF, 0x40, 0x15};
static const uint8_t W25Q128JV[3] = {0xEF, 0x40, 0x18};
static const uint8_t XT25F128F[3] = {0x0B, 0x40, 0x18};
static const uint8_t BY25Q64ES[3] = {0x68, 0x40, 0x17};
static const uint8_t UNLISTED[3] = {0xC8, 0x40, 0x17};

typedef struct {
    const flash_timing_t *t; /* the chip's own behaviour */
    uint32_t now, t0;
    uint32_t work_left;  /* the operation's time still to run */
    uint32_t work_total; /* what each new operation takes */
    uint32_t resumed_at;
    bool suspended, flight;
    uint32_t leave_flight_after; /* pieces; 0: never */
    int fail_at;                 /* the piece that fails; 0: none */
    /* what happened */
    uint32_t pieces, suspends, slots, wholes, starts;
    uint32_t over_us;       /* the most a piece ran past its slot's end */
    uint32_t ignored;       /* suspends the chip ignored */
    uint32_t piece_in_slot; /* pieces that began outside the writer's slot */
} chip_t;

static chip_t chip;

static uint32_t now_us(void *c) {
    return ((chip_t *)c)->now;
}
static uint32_t slot_t0(void *c) {
    return ((chip_t *)c)->t0;
}
static bool in_flight(void *c) {
    return ((chip_t *)c)->flight;
}

/* The writer gives the frame up: Lua has the rest. The next frame's slot
 * begins at its period, where an interrupt runs the request on time; the
 * writer's task wakes later. */
static int at_slot_start(void *v, const flash_slot_req_t *r) {
    chip_t *c = v;
    while ((int32_t)(c->t0 + LOOP_PERIOD_US - c->now) <= 0)
        c->t0 += LOOP_PERIOD_US;
    c->t0 += LOOP_PERIOD_US;
    c->now = c->t0 + IRQ_US;
    c->slots++;
    int rc = flash_slot_try(r, c->t0);
    if ((int32_t)(c->t0 + TASK_WAKE_US - c->now) > 0)
        c->now = c->t0 + TASK_WAKE_US;
    return rc;
}

static void note_end(chip_t *c) {
    uint32_t end = c->t0 + FRAME_WRITER_US;
    if ((int32_t)(c->now - end) > 0 && c->now - end > c->over_us)
        c->over_us = c->now - end;
}

static int piece(void *v, flash_slot_kind_t kind, uint32_t addr, const uint8_t *data, uint32_t len,
                 const flash_piece_t *p) {
    (void)kind;
    (void)addr;
    (void)data;
    (void)len;
    chip_t *c = v;
    c->pieces++;
    if (frame_writer_left_us(c->t0, c->now) == 0)
        c->piece_in_slot++;
    if (c->fail_at && (int)c->pieces == c->fail_at)
        return -5;
    if (p->resume) {
        TEST_ASSERT_TRUE_MESSAGE(c->suspended, "a resume with nothing suspended");
        c->suspended = false;
    } else {
        TEST_ASSERT_FALSE_MESSAGE(c->suspended, "a start over a suspended operation");
        c->work_left = c->work_total;
        c->starts++;
    }
    c->now += START_US;
    c->resumed_at = c->now;
    int rc = PIECE_DONE;
    uint32_t at = p->suspend_at;
    if ((int32_t)(p->suspend_from - at) > 0)
        at = p->suspend_from;
    if (!p->may_suspend || (int32_t)(c->now + c->work_left - at) <= 0) {
        c->now += c->work_left + POLL_US;
        c->work_left = 0;
    } else {
        uint32_t ran = (int32_t)(at - c->now) > 0 ? at - c->now : 0u;
        c->work_left -= ran;
        c->now += ran + POLL_US;
        bool can = (kind == SLOT_ERASE) ? c->t->erase_suspends : c->t->program_suspends;
        if (!can || c->now - c->resumed_at < c->t->resume_hold_us) {
            c->ignored++;
            c->now += c->work_left;
            c->work_left = 0;
        } else {
            c->now += c->t->suspend_us + SUSPEND_US;
            c->suspended = true;
            c->suspends++;
            rc = PIECE_SUSPENDED;
        }
    }
    note_end(c);
    if (c->leave_flight_after && c->pieces >= c->leave_flight_after)
        c->flight = false;
    return rc;
}

static int whole(void *v, flash_slot_kind_t kind, bool resume, uint32_t addr, const uint8_t *data, uint32_t len) {
    (void)kind;
    (void)addr;
    (void)data;
    (void)len;
    chip_t *c = v;
    c->wholes++;
    if (resume) {
        TEST_ASSERT_TRUE(c->suspended);
        c->suspended = false;
    } else {
        c->work_left = c->work_total;
        c->starts++;
    }
    c->now += START_US + c->work_left;
    c->work_left = 0;
    return PIECE_DONE;
}

static const flash_slot_io_t io = {&chip, now_us, slot_t0, at_slot_start, in_flight, piece, whole};

static void fresh(const uint8_t jedec[3], uint32_t work_us, uint32_t start_in_frame_us) {
    memset(&chip, 0, sizeof(chip));
    chip.t = flash_timing_for(jedec);
    chip.work_total = work_us;
    chip.flight = true;
    chip.t0 = 1000000u;
    chip.now = chip.t0 + start_in_frame_us;
}

void setUp(void) {}
void tearDown(void) {}

/* ── The table ────────────────────────────────────────────────────── */

void test_FLT_LOG_09_every_fitted_part_is_known(void) {
    TEST_ASSERT_EQUAL_STRING("W25Q16JV", flash_timing_for(W25Q16JV)->part);
    TEST_ASSERT_EQUAL_STRING("W25Q128JV", flash_timing_for(W25Q128JV)->part);
    TEST_ASSERT_EQUAL_STRING("XT25F128F", flash_timing_for(XT25F128F)->part);
    TEST_ASSERT_EQUAL_STRING("BY25Q64ES", flash_timing_for(BY25Q64ES)->part);
}

void test_FLT_LOG_09_an_unlisted_part_has_no_figures(void) {
    TEST_ASSERT_NULL(flash_timing_for(UNLISTED));
}

/* Each listed part's figures are its datasheet's worst case: a suspend
 * that lands, and a program that ends, inside an empty slot. */
void test_FLT_LOG_09_every_listed_program_fits_an_empty_slot(void) {
    const uint8_t *all[] = {W25Q16JV, W25Q128JV, XT25F128F, BY25Q64ES};
    for (unsigned i = 0; i < 4; i++) {
        const flash_timing_t *t = flash_timing_for(all[i]);
        uint32_t need = t->program_suspends
                            ? FLASH_SLOT_START_US + t->resume_hold_us + t->suspend_us + FLASH_SLOT_SUSPEND_US
                            : FLASH_SLOT_START_US + t->program_max_us;
        TEST_ASSERT_TRUE_MESSAGE(IRQ_US + need <= FRAME_WRITER_US, t->part);
    }
}

/* ── The frame ────────────────────────────────────────────────────── */

void test_LUA_RUN_04_lua_has_6_5_ms_ending_by_9_5(void) {
    uint32_t t0 = 0xFFFFF000u; /* across the timer's wrap */
    TEST_ASSERT_EQUAL_UINT32(6500u, frame_lua_budget_us(t0, t0 + 3000u));
    TEST_ASSERT_EQUAL_UINT32(6500u, frame_lua_budget_us(t0, t0 + 400u)); /* the writer gave its slot up */
    TEST_ASSERT_EQUAL_UINT32(6000u, frame_lua_budget_us(t0, t0 + 3500u));
    TEST_ASSERT_EQUAL_UINT32(1u, frame_lua_budget_us(t0, t0 + 9499u));
    TEST_ASSERT_EQUAL_UINT32(0u, frame_lua_budget_us(t0, t0 + 9500u));
    TEST_ASSERT_EQUAL_UINT32(0u, frame_lua_budget_us(t0, t0 + 9999u));
}

void test_FLT_LOG_09_the_writer_has_the_first_3_ms(void) {
    uint32_t t0 = 0xFFFFFF00u;
    TEST_ASSERT_EQUAL_UINT32(3000u, frame_writer_left_us(t0, t0));
    TEST_ASSERT_EQUAL_UINT32(1000u, frame_writer_left_us(t0, t0 + 2000u));
    TEST_ASSERT_EQUAL_UINT32(0u, frame_writer_left_us(t0, t0 + 3000u));
    TEST_ASSERT_EQUAL_UINT32(0u, frame_writer_left_us(t0, t0 + 7000u));
}

/* ── The plan ─────────────────────────────────────────────────────── */

/* BY25Q64ES cannot suspend a program (its page 62 lists erase suspend
 * alone): one starts only where its 2.4 ms fits. */
void test_FLT_LOG_09_a_program_that_cannot_suspend_starts_only_where_it_fits(void) {
    const flash_timing_t *t = flash_timing_for(BY25Q64ES);
    uint32_t t0 = 5000u, end = t0 + FRAME_WRITER_US;
    flash_piece_t p;
    TEST_ASSERT_TRUE(flash_slot_plan(t, SLOT_PROGRAM, false, t0 + 100u, end, &p));
    TEST_ASSERT_FALSE(p.may_suspend);
    uint32_t last = end - t->program_max_us - FLASH_SLOT_START_US;
    TEST_ASSERT_TRUE(flash_slot_plan(t, SLOT_PROGRAM, false, last, end, &p));
    TEST_ASSERT_FALSE(flash_slot_plan(t, SLOT_PROGRAM, false, last + 1u, end, &p));
}

/* A piece that can suspend starts while its hold and its suspend still fit,
 * and is told to suspend in time for the suspend to land inside the slot. */
/* [FLT-LOG-09] A program that cannot suspend, asked for too late in a slot,
 * starts at the next slot's start however late the writer's task wakes. */
void test_FLT_LOG_09_a_piece_that_waited_starts_at_its_slots_start(void) {
    fresh(BY25Q64ES, 2000u, 2500u);
    TEST_ASSERT_EQUAL(0, flash_slot_run(&io, chip.t, SLOT_PROGRAM, 0, NULL, 256));
    TEST_ASSERT_EQUAL(1, chip.slots);
    TEST_ASSERT_EQUAL(1, chip.pieces);
    TEST_ASSERT_EQUAL(0, chip.piece_in_slot);
    TEST_ASSERT_EQUAL(0, chip.over_us);
}

void test_FLT_LOG_09_a_piece_that_can_suspend_is_told_when(void) {
    const flash_timing_t *t = flash_timing_for(XT25F128F);
    uint32_t t0 = 5000u, end = t0 + FRAME_WRITER_US;
    flash_piece_t p;
    TEST_ASSERT_TRUE(flash_slot_plan(t, SLOT_ERASE, true, t0 + 50u, end, &p));
    TEST_ASSERT_TRUE(p.may_suspend && p.resume);
    TEST_ASSERT_EQUAL_UINT32(end - t->suspend_us - FLASH_SLOT_SUSPEND_US, p.suspend_at);
    TEST_ASSERT_EQUAL_UINT32(t0 + 50u + FLASH_SLOT_START_US + t->resume_hold_us, p.suspend_from);
    uint32_t last = end - t->suspend_us - FLASH_SLOT_SUSPEND_US - t->resume_hold_us - FLASH_SLOT_START_US;
    TEST_ASSERT_TRUE(flash_slot_plan(t, SLOT_ERASE, true, last, end, &p));
    TEST_ASSERT_FALSE(flash_slot_plan(t, SLOT_ERASE, true, last + 1u, end, &p));
}

/* ── Runs ─────────────────────────────────────────────────────────── */

/* Every listed part, at its datasheet's worst program and erase, begun
 * anywhere in a frame: no piece runs past the writer's slot, none begins
 * outside it, and the chip ignores no suspend. An erase that does not end
 * in pieces runs none [FLT-LOG-10]. */
void test_FLT_LOG_09_no_piece_runs_past_the_writers_slot(void) {
    const uint8_t *all[] = {W25Q16JV, W25Q128JV, XT25F128F, BY25Q64ES};
    for (unsigned i = 0; i < 4; i++) {
        const flash_timing_t *t = flash_timing_for(all[i]);
        for (uint32_t start = 0; start < LOOP_PERIOD_US; start += 37u) {
            for (int k = 0; k < 2; k++) {
                flash_slot_kind_t kind = k ? SLOT_ERASE : SLOT_PROGRAM;
                bool refused = kind == SLOT_ERASE && !t->erase_suspends;
                fresh(all[i], k ? t->erase_max_us : t->program_max_us, start);
                TEST_ASSERT_EQUAL_INT_MESSAGE(refused ? FLASH_SLOT_EREFUSED : 0,
                                              flash_slot_run(&io, chip.t, kind, 0, NULL, 0), t->part);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, chip.over_us, t->part);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, chip.piece_in_slot, t->part);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, chip.ignored, t->part);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(refused ? 0u : 1u, chip.starts, t->part);
            }
        }
    }
}

/* XT25F128F's slowest erase, 800 ms: suspended at the end of every slot,
 * resumed in the next, and finished, with every frame's rest left to Lua. */
void test_FLT_LOG_10_an_erase_is_suspended_at_each_slot_end_and_resumed(void) {
    const flash_timing_t *t = flash_timing_for(XT25F128F);
    fresh(XT25F128F, t->erase_max_us, IRQ_US);
    TEST_ASSERT_EQUAL_INT(0, flash_slot_run(&io, t, SLOT_ERASE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, chip.work_left);
    TEST_ASSERT_TRUE(chip.suspends > 250u);
    TEST_ASSERT_EQUAL_UINT32(chip.suspends + 1u, chip.pieces);
    TEST_ASSERT_EQUAL_UINT32(chip.suspends, chip.slots);
    TEST_ASSERT_EQUAL_UINT32(0u, chip.over_us);
    TEST_ASSERT_EQUAL_UINT32(0u, chip.ignored);
}

/* [FLT-LOG-10] BY25Q64ES's erase, resumed and suspended in each slot on
 * MK1B-8MB, had not ended after 2,000 slots, and the chip held it suspended
 * after: refused in flight. MK1C's XT25F128F ends its erase in about ten
 * slots. */
void test_FLT_LOG_10_a_part_whose_erase_does_not_end_in_pieces_does_not_erase_in_flight(void) {
    TEST_ASSERT_FALSE(flash_timing_for(BY25Q64ES)->erase_suspends);
    TEST_ASSERT_TRUE(flash_timing_for(XT25F128F)->erase_suspends);
    fresh(BY25Q64ES, 50000u, IRQ_US);
    TEST_ASSERT_EQUAL_INT(FLASH_SLOT_EREFUSED, flash_slot_run(&io, chip.t, SLOT_ERASE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, chip.starts);
}

/* An unlisted part's erase cannot be planned into a slot: refused in
 * flight rather than run past it. */
void test_FLT_LOG_10_an_unlisted_parts_erase_is_refused_in_flight(void) {
    fresh(UNLISTED, 50000u, IRQ_US);
    TEST_ASSERT_EQUAL_INT(FLASH_SLOT_EREFUSED, flash_slot_run(&io, NULL, SLOT_ERASE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, chip.starts);
}

/* An unlisted part's program waits for the next slot and runs whole from
 * its start: the log is kept, at the cost of Lua's time in that frame. */
void test_FLT_LOG_09_an_unlisted_parts_program_runs_whole_from_a_slots_start(void) {
    fresh(UNLISTED, 3000u, 1200u);
    uint32_t t0 = chip.t0;
    TEST_ASSERT_EQUAL_INT(0, flash_slot_run(&io, NULL, SLOT_PROGRAM, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(1u, chip.slots);
    TEST_ASSERT_EQUAL_UINT32(t0 + LOOP_PERIOD_US, chip.t0);
    TEST_ASSERT_EQUAL_UINT32(1u, chip.pieces);
    TEST_ASSERT_EQUAL_UINT32(0u, chip.suspends);
    TEST_ASSERT_EQUAL_UINT32(chip.t0 + IRQ_US + START_US + 3000u + POLL_US, chip.now);
}

/* Landing in the middle of an erase: the rest runs whole, under the lockout
 * the pad's operations use, and the erase is finished. */
void test_FLT_LOG_10_an_erase_suspended_at_landing_is_finished_whole(void) {
    const flash_timing_t *t = flash_timing_for(W25Q128JV);
    fresh(W25Q128JV, t->erase_max_us, IRQ_US);
    chip.leave_flight_after = 3u;
    TEST_ASSERT_EQUAL_INT(0, flash_slot_run(&io, t, SLOT_ERASE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(3u, chip.pieces);
    TEST_ASSERT_EQUAL_UINT32(1u, chip.wholes);
    TEST_ASSERT_EQUAL_UINT32(0u, chip.work_left);
    TEST_ASSERT_EQUAL_UINT32(1u, chip.starts);
}

/* On the pad nothing is sliced: the operation runs whole. */
void test_FLT_LOG_09_on_the_pad_an_operation_runs_whole(void) {
    fresh(BY25Q64ES, 400000u, 9000u);
    chip.flight = false;
    TEST_ASSERT_EQUAL_INT(0, flash_slot_run(&io, chip.t, SLOT_ERASE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, chip.pieces);
    TEST_ASSERT_EQUAL_UINT32(1u, chip.wholes);
}

void test_FLT_LOG_09_a_failed_piece_is_the_operations_failure(void) {
    const flash_timing_t *t = flash_timing_for(W25Q16JV);
    fresh(W25Q16JV, t->erase_max_us, IRQ_US);
    chip.fail_at = 4;
    TEST_ASSERT_EQUAL_INT(-5, flash_slot_run(&io, t, SLOT_ERASE, 0, NULL, 0));
}

/* A chip that never finishes is given up, never waited on for good. */
void test_FLT_LOG_10_a_chip_that_never_finishes_is_given_up(void) {
    const flash_timing_t *t = flash_timing_for(W25Q16JV);
    fresh(W25Q16JV, 1000000000u, IRQ_US);
    TEST_ASSERT_EQUAL_INT(FLASH_SLOT_ETIME, flash_slot_run(&io, t, SLOT_ERASE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(FLASH_SLOT_MAX_PIECES, chip.pieces);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LOG_09_every_fitted_part_is_known);
    RUN_TEST(test_FLT_LOG_09_an_unlisted_part_has_no_figures);
    RUN_TEST(test_FLT_LOG_09_every_listed_program_fits_an_empty_slot);
    RUN_TEST(test_LUA_RUN_04_lua_has_6_5_ms_ending_by_9_5);
    RUN_TEST(test_FLT_LOG_09_the_writer_has_the_first_3_ms);
    RUN_TEST(test_FLT_LOG_09_a_program_that_cannot_suspend_starts_only_where_it_fits);
    RUN_TEST(test_FLT_LOG_09_a_piece_that_waited_starts_at_its_slots_start);
    RUN_TEST(test_FLT_LOG_09_a_piece_that_can_suspend_is_told_when);
    RUN_TEST(test_FLT_LOG_09_no_piece_runs_past_the_writers_slot);
    RUN_TEST(test_FLT_LOG_10_an_erase_is_suspended_at_each_slot_end_and_resumed);
    RUN_TEST(test_FLT_LOG_10_a_part_whose_erase_does_not_end_in_pieces_does_not_erase_in_flight);
    RUN_TEST(test_FLT_LOG_10_an_unlisted_parts_erase_is_refused_in_flight);
    RUN_TEST(test_FLT_LOG_09_an_unlisted_parts_program_runs_whole_from_a_slots_start);
    RUN_TEST(test_FLT_LOG_10_an_erase_suspended_at_landing_is_finished_whole);
    RUN_TEST(test_FLT_LOG_09_on_the_pad_an_operation_runs_whole);
    RUN_TEST(test_FLT_LOG_09_a_failed_piece_is_the_operations_failure);
    RUN_TEST(test_FLT_LOG_10_a_chip_that_never_finishes_is_given_up);
    return UNITY_END();
}
