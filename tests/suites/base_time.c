#define _POSIX_C_SOURCE 200809L

/* The two clocks: the monotonic one everything schedules against, and the wall clock a machine
   with no RTC battery cannot be trusted about until it has been somewhere with a network. */

#include "framework/inkwell_test.h"

#include "inkwell/base/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The wall clock's override, which is what makes a rendered frame reproducible.
 *
 * Zero has to mean "follow the real clock" rather than "it is 1970": a caller that clears the
 * pin - every test above does, in its cleanup - would otherwise leave every later case drawing
 * ages of fifty-five years.
 */
INKWELL_TEST_CASE(time_wall_clock_pins_and_releases, unit) {
    const uint32_t pinned = 1767200000U;
    inkwell_time_wall_set_fixed(pinned);
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_time_wall_s() != pinned, inkwell_time_wall_set_fixed(0U),
                                 "the pinned clock did not read back");

    inkwell_time_wall_set_fixed(0U);
    const uint32_t live = inkwell_time_wall_s();
    /* Any date after this file was written; the point is that it is the real clock again and
       not the epoch. */
    INKWELL_TEST_FAIL_IF(live < 1700000000U, "clearing the pin did not restore the real clock");
    record_success(test_name);
}

/*
 * The difference between "what the machine says" and "a date I can do arithmetic with".
 *
 * The Brick has no RTC battery, so with no network it boots into the epoch and time(NULL) is a
 * small positive number. An *age* measured against that comes out negative or enormous and every
 * caller that draws one already refuses to; a *deadline* does not have that safety - "does this
 * expire before now" and "how long has it got" both read as confident answers whichever way the
 * arithmetic lands, which is how a waypoint's expiry came to report tens of thousands of days.
 */
INKWELL_TEST_CASE(time_wall_clock_credibility, unit) {
    /* 1970, which is where a machine that has not been told the date starts. */
    inkwell_time_wall_set_fixed(42U);
    const uint32_t machine = inkwell_time_wall_s();
    const uint32_t credible = inkwell_time_wall_credible_s();
    inkwell_time_wall_set_fixed(0U);
    INKWELL_TEST_FAIL_IF(machine != 42U, "the machine's clock is whatever it says");
    INKWELL_TEST_FAIL_IF(credible != 0U, "1970 is not a date to measure a deadline against");

    /* One second under the floor is still not a clock, and the floor itself is not either. */
    inkwell_time_wall_set_fixed(INKWELL_TIME_CLOCK_MIN_EPOCH);
    const uint32_t at_floor = inkwell_time_wall_credible_s();
    inkwell_time_wall_set_fixed(INKWELL_TIME_CLOCK_MIN_EPOCH + 1U);
    const uint32_t over_floor = inkwell_time_wall_credible_s();
    inkwell_time_wall_set_fixed(0U);
    INKWELL_TEST_FAIL_IF(at_floor != 0U, "the floor itself is not a credible clock");
    INKWELL_TEST_FAIL_IF(over_floor != INKWELL_TIME_CLOCK_MIN_EPOCH + 1U,
                         "a second past the floor is a clock, and is passed through unchanged");

    record_success(test_name);
}
