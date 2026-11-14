/*
 * PCM playback. The list parser is tested on every system against /proc/asound/pcm as kernels
 * write it. Opening is Linux's; elsewhere it must refuse, and on Linux a device that is not
 * there must say so rather than crash. Playing a sound needs a sound card, which a test machine
 * may not have: the virtual device's own check (an emulator with virtio-sound) covers that.
 */
#include "framework/inkwell_test.h"

#include "inkwell/io/pcm.h"

#include <errno.h>
#include <string.h>

static const char k_list[] = "00-00: virtio-snd : virtio-snd : playback 1\n"
                             "00-01: virtio-snd : virtio-snd : capture 1\n"
                             "01-03: HDMI 0 : HDMI 0 : playback 1 : capture 1\n";

INKWELL_TEST_CASE(pcm_parses_the_playback_devices, unit) {
    struct inkwell_pcm_device devices[4];
    const int found = inkwell_pcm_parse_list(k_list, devices, 4);
    INKWELL_TEST_FAIL_IF(found != 2, "two of the three are playback devices");
    INKWELL_TEST_FAIL_IF(devices[0].card != 0 || devices[0].device != 0, "first: card 0 device 0");
    INKWELL_TEST_FAIL_IF(strcmp(devices[0].id, "virtio-snd") != 0, "first id");
    INKWELL_TEST_FAIL_IF(strcmp(devices[0].name, "virtio-snd") != 0, "first name");
    INKWELL_TEST_FAIL_IF(devices[1].card != 1 || devices[1].device != 3, "second: card 1 dev 3");
    INKWELL_TEST_FAIL_IF(strcmp(devices[1].id, "HDMI 0") != 0, "an id with a space is kept");
    record_success(test_name);
}

INKWELL_TEST_CASE(pcm_list_stops_at_max_and_skips_junk, unit) {
    struct inkwell_pcm_device device;
    INKWELL_TEST_FAIL_IF(inkwell_pcm_parse_list(k_list, &device, 1) != 1, "max is a limit");
    INKWELL_TEST_FAIL_IF(device.card != 0, "the first one");
    static const char junk[] = "garbage\n"
                               "0x-00: a : b : playback 1\n"
                               "02-00: no fields\n"
                               "\n"
                               "02-01: id : name : playback 1";
    INKWELL_TEST_FAIL_IF(inkwell_pcm_parse_list(junk, &device, 1) != 1,
                         "malformed lines are skipped; a last line without a newline counts");
    INKWELL_TEST_FAIL_IF(device.card != 2 || device.device != 1, "the good line");
    INKWELL_TEST_FAIL_IF(inkwell_pcm_parse_list("", &device, 1) != 0, "empty: none");
    INKWELL_TEST_FAIL_IF(inkwell_pcm_parse_list(NULL, &device, 1) != -EINVAL, "NULL text");
    record_success(test_name);
}

INKWELL_TEST_CASE(pcm_open_refuses_a_bad_config, unit) {
    struct inkwell_pcm pcm;
    const struct inkwell_pcm_config one_period = {
        .rate = 48000, .channels = 2, .period_frames = 256, .periods = 1};
#if defined(__linux__)
    const int expected = -EINVAL;
#else
    const int expected = -ENOSYS;
#endif
    INKWELL_TEST_FAIL_IF(inkwell_pcm_open(&pcm, 0, 0, &one_period) != expected,
                         "a buffer of one period cannot be asked for");
    INKWELL_TEST_FAIL_IF(inkwell_pcm_fd(&pcm) != -1, "a refused handle is closed");
    inkwell_pcm_close(&pcm);
    INKWELL_TEST_FAIL_IF(inkwell_pcm_write(&pcm, NULL, 0) != -EINVAL, "a closed handle");
    record_success(test_name);
}

INKWELL_TEST_CASE(pcm_open_says_a_device_is_not_there, unit) {
    struct inkwell_pcm pcm;
    const struct inkwell_pcm_config want = {
        .rate = 48000, .channels = 2, .period_frames = 256, .periods = 4};
    const int opened = inkwell_pcm_open(&pcm, 97, 97, &want);
#if defined(__linux__)
    INKWELL_TEST_FAIL_IF(opened != -ENOENT, "card 97 is not there");
#else
    INKWELL_TEST_FAIL_IF(opened != -ENOSYS, "off Linux every open refuses");
    struct inkwell_pcm_device device;
    INKWELL_TEST_FAIL_IF(inkwell_pcm_find_playback(&device) != -ENOSYS, "and so does the find");
#endif
    INKWELL_TEST_FAIL_IF(inkwell_pcm_fd(&pcm) != -1, "closed");
    record_success(test_name);
}
