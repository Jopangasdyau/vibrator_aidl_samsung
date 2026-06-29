// ff_probe — diagnose how to drive a force-feedback LRA (qcom-hv-haptics).
// Usage: ff_probe [/dev/input/eventN]   (default: auto-detect, then event6)
// Prints supported FF effect types, then plays CONSTANT, PERIODIC(SINE) and
// RUMBLE in turn with gaps so you can feel which one(s) actually buzz.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int test_bit(const unsigned long *b, int n) {
    return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1UL;
}

static int play(int fd, struct ff_effect *e, const char *label, int ms) {
    e->id = -1;
    if (ioctl(fd, EVIOCSFF, e) < 0) { printf("  [%s] EVIOCSFF failed: %s\n", label, strerror(errno)); return -1; }
    struct input_event ev = {0};
    ev.type = EV_FF; ev.code = e->id; ev.value = 1;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { printf("  [%s] play write failed: %s\n", label, strerror(errno)); }
    else printf("  [%s] PLAYING for %d ms — feel for a buzz now...\n", label, ms);
    fflush(stdout);
    usleep(ms * 1000);
    ev.value = 0; (void)write(fd, &ev, sizeof(ev));
    ioctl(fd, EVIOCRMFF, e->id);
    usleep(700 * 1000);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/dev/input/event6";
    int fd = open(path, O_RDWR);
    if (fd < 0) { printf("open %s failed: %s\n", path, strerror(errno)); return 1; }

    char name[128] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    printf("Device: %s  (%s)\n", path, name);

    unsigned long ff[1 + FF_MAX / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) < 0) { printf("EVIOCGBIT(EV_FF) failed: %s\n", strerror(errno)); return 1; }
    printf("Supported FF effects:%s%s%s%s%s%s\n",
           test_bit(ff, FF_CONSTANT) ? " CONSTANT" : "",
           test_bit(ff, FF_PERIODIC) ? " PERIODIC" : "",
           test_bit(ff, FF_RUMBLE)   ? " RUMBLE"   : "",
           test_bit(ff, FF_SINE)     ? " SINE"     : "",
           test_bit(ff, FF_CUSTOM)   ? " CUSTOM"   : "",
           test_bit(ff, FF_GAIN)     ? " GAIN"     : "");

    // Set global gain to max (some drivers play at gain*level; default may be 0).
    if (test_bit(ff, FF_GAIN)) {
        struct input_event g = {0};
        g.type = EV_FF; g.code = FF_GAIN; g.value = 0xFFFF;
        printf("Setting FF_GAIN = max\n");
        (void)write(fd, &g, sizeof(g));
    }

    if (test_bit(ff, FF_CONSTANT)) {
        struct ff_effect e = {0};
        e.type = FF_CONSTANT;
        e.u.constant.level = 0x7FFF;
        e.replay.length = 800;
        play(fd, &e, "CONSTANT level=max 800ms", 800);
    }
    if (test_bit(ff, FF_PERIODIC)) {
        struct ff_effect e = {0};
        e.type = FF_PERIODIC;
        e.u.periodic.waveform = FF_SINE;
        e.u.periodic.period = 6;          // ~166 Hz
        e.u.periodic.magnitude = 0x7FFF;
        e.replay.length = 800;
        play(fd, &e, "PERIODIC sine 800ms", 800);
    }
    if (test_bit(ff, FF_RUMBLE)) {
        struct ff_effect e = {0};
        e.type = FF_RUMBLE;
        e.u.rumble.strong_magnitude = 0xFFFF;
        e.u.rumble.weak_magnitude = 0xFFFF;
        e.replay.length = 800;
        play(fd, &e, "RUMBLE strong=max 800ms", 800);
    }
    printf("Done. Note which label(s) you felt.\n");
    close(fd);
    return 0;
}
