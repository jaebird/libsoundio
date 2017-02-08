/*
 * Copyright (c) 2017 Jae Stutzman
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef SOUNDIO_REMOTE_H
#define SOUNDIO_REMOTE_H

#include "soundio_internal.h"
#include "os.h"
#include "ring_buffer.h"
#include "atomics.h"

struct SoundIoPrivate;
enum SoundIoError soundio_remote_init(struct SoundIoPrivate *si);

struct SoundIoRemote {
    struct SoundIoOsMutex *mutex;
    struct SoundIoOsCond *cond;
    bool devices_emitted;
};

struct SoundIoDeviceRemote { int make_the_struct_not_empty; };

struct SoundIoOutStreamRemote {
    struct SoundIoOsThread *thread;
    struct SoundIoOsCond *cond;
    struct SoundIoAtomicFlag abort_flag;
    double period_duration;
    int buffer_frame_count;
    int frames_left;
    int write_frame_count;
    struct SoundIoRingBuffer ring_buffer;
    double playback_start_time;
    struct SoundIoAtomicFlag clear_buffer_flag;
    struct SoundIoAtomicBool pause_requested;
    struct SoundIoChannelArea areas[SOUNDIO_MAX_CHANNELS];
};

struct SoundIoInStreamRemote {
    struct SoundIoOsThread *thread;
    struct SoundIoOsCond *cond;
    struct SoundIoAtomicFlag abort_flag;
    double period_duration;
    int frames_left;
    int read_frame_count;
    int buffer_frame_count;
    struct SoundIoRingBuffer ring_buffer;
    struct SoundIoAtomicBool pause_requested;
    struct SoundIoChannelArea areas[SOUNDIO_MAX_CHANNELS];
};

#endif
