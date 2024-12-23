#include <linux/input.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DURATION 6000 // Duration of each effect in milliseconds

void play_effect(int fd, struct ff_effect *effect) {
    struct input_event play, stop;

    // Play the effect
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = effect->id;
    play.value = 1;

    if (write(fd, &play, sizeof(play)) < 0) {
        perror("Failed to play effect");
        return;
    }

    printf("Playing effect: %d\n", effect->id);
    usleep(DURATION * 1000); // Wait for the effect duration

    // Stop the effect
    memset(&stop, 0, sizeof(stop));
    stop.type = EV_FF;
    stop.code = effect->id;
    stop.value = 0;

    if (write(fd, &stop, sizeof(stop)) < 0) {
        perror("Failed to stop effect");
    }

    printf("Stopped effect: %d\n", effect->id);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <event_number>\n", argv[0]);
        return 1;
    }

    char device_path[64];
    snprintf(device_path, sizeof(device_path), "/dev/input/event%s", argv[1]);

    int fd = open(device_path, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    struct ff_effect effect;
    memset(&effect, 0, sizeof(effect));

    // Test Constant Force
    effect.type = FF_CONSTANT;
    effect.id = -1; // Request a new effect ID
    effect.u.constant.level = 0x4000; // Medium intensity
    effect.replay.length = DURATION; // Duration in ms
    effect.replay.delay = 0;
    if (ioctl(fd, EVIOCSFF, &effect) == 0) {
        play_effect(fd, &effect);
    } else {
        perror("FF_CONSTANT not supported");
    }

    // Test Periodic Effects (e.g., sine wave)
    effect.type = FF_PERIODIC;
    effect.id = -1; // Request a new effect ID
    effect.u.periodic.waveform = FF_SINE; // Sine wave
    effect.u.periodic.magnitude = 0x4000; // Medium intensity
    effect.u.periodic.offset = 0;
    effect.u.periodic.phase = 0;
    effect.u.periodic.period = 500; // Period in ms
    effect.u.periodic.envelope.attack_length = 500; // Attack time
    effect.u.periodic.envelope.attack_level = 0x2000;
    effect.u.periodic.envelope.fade_length = 500; // Fade time
    effect.u.periodic.envelope.fade_level = 0x1000;
    effect.replay.length = DURATION; // Duration in ms
    effect.replay.delay = 0;
    if (ioctl(fd, EVIOCSFF, &effect) == 0) {
        play_effect(fd, &effect);
    } else {
        perror("FF_PERIODIC not supported");
    }

    // Test Ramp Effect
    effect.type = FF_RAMP;
    effect.id = -1; // Request a new effect ID
    effect.u.ramp.start_level = 0x1000; // Start intensity
    effect.u.ramp.end_level = 0x4000;   // End intensity
    effect.replay.length = DURATION;   // Duration in ms
    effect.replay.delay = 0;
    if (ioctl(fd, EVIOCSFF, &effect) == 0) {
        play_effect(fd, &effect);
    } else {
        perror("FF_RAMP not supported");
    }

    // Test Spring Effect
    effect.type = FF_SPRING;
    effect.id = -1; // Request a new effect ID
    effect.u.condition[0].right_saturation = 0x4000;
    effect.u.condition[0].left_saturation = 0x4000;
    effect.u.condition[0].right_coeff = 0x2000;
    effect.u.condition[0].left_coeff = 0x2000;
    effect.u.condition[0].deadband = 0x1000;
    effect.u.condition[0].center = 0x2000;
    effect.replay.length = DURATION;
    effect.replay.delay = 0;
    if (ioctl(fd, EVIOCSFF, &effect) == 0) {
        play_effect(fd, &effect);
    } else {
        perror("FF_SPRING not supported");
    }

    // Test Damper Effect
    effect.type = FF_DAMPER;
    effect.id = -1; // Request a new effect ID
    effect.u.condition[0].right_saturation = 0x4000;
    effect.u.condition[0].left_saturation = 0x4000;
    effect.u.condition[0].right_coeff = 0x2000;
    effect.u.condition[0].left_coeff = 0x2000;
    effect.u.condition[0].deadband = 0x1000;
    effect.u.condition[0].center = 0x2000;
    effect.replay.length = DURATION;
    effect.replay.delay = 0;
    if (ioctl(fd, EVIOCSFF, &effect) == 0) {
        play_effect(fd, &effect);
    } else {
        perror("FF_DAMPER not supported");
    }

    // Test Inertia Effect
    effect.type = FF_INERTIA;
    effect.id = -1; // Request a new effect ID
    effect.u.condition[0].right_saturation = 0x4000;
    effect.u.condition[0].left_saturation = 0x4000;
    effect.u.condition[0].right_coeff = 0x2000;
    effect.u.condition[0].left_coeff = 0x2000;
    effect.u.condition[0].deadband = 0x1000;
    effect.u.condition[0].center = 0x2000;
    effect.replay.length = DURATION;
    effect.replay.delay = 0;
    if (ioctl(fd, EVIOCSFF, &effect) == 0) {
        play_effect(fd, &effect);
    } else {
        perror("FF_INERTIA not supported");
    }

    close(fd);
    return 0;
}