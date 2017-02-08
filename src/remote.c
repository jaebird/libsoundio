/*
 * Copyright (c) 2017 Jae Stutzman
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "remote.h"
#include "soundio_private.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// TODO: not windows portable yet
#include <sys/socket.h>
//#include <netinet/in.h> 
#include <arpa/inet.h>
//#include <sys/ioctl.h>

#define SERVER_IP "127.0.0.1"
//#define SERVER_IP "192.168.56.102"
#define BUFLEN 512  //Max length of buffer
#define PORT 8888   //The port on which to send data

struct sockaddr_in si_me;
struct sockaddr_in si_other;
int sock = 0;
time_t last_recv_time = 0;

static void die(char *s)
{
    perror(s);
//    exit(1);
}

static void playback_thread_run(void *arg) {
    struct SoundIoOutStreamPrivate *os = (struct SoundIoOutStreamPrivate *)arg;
    struct SoundIoOutStream *outstream = &os->pub;
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;

    int fill_bytes = soundio_ring_buffer_fill_count(&osd->ring_buffer);
    int free_bytes = soundio_ring_buffer_capacity(&osd->ring_buffer) - fill_bytes;
    int free_frames = free_bytes / outstream->bytes_per_frame;
    osd->frames_left = free_frames;
    if (free_frames > 0)
        outstream->write_callback(outstream, 0, free_frames);
    double start_time = soundio_os_get_time();
    long frames_consumed = 0;

    char buf[BUFLEN];

    while (SOUNDIO_ATOMIC_FLAG_TEST_AND_SET(osd->abort_flag)) {
        double now = soundio_os_get_time();
        double time_passed = now - start_time;
        double next_period = start_time +
            ceil_dbl(time_passed / osd->period_duration) * osd->period_duration;
        double relative_time = next_period - now;
        soundio_os_cond_timed_wait(osd->cond, NULL, relative_time);
        if (!SOUNDIO_ATOMIC_FLAG_TEST_AND_SET(osd->clear_buffer_flag)) {
            soundio_ring_buffer_clear(&osd->ring_buffer);
            int free_bytes = soundio_ring_buffer_capacity(&osd->ring_buffer);
            int free_frames = free_bytes / outstream->bytes_per_frame;
            osd->frames_left = free_frames;
            if (free_frames > 0)
                outstream->write_callback(outstream, 0, free_frames);
            frames_consumed = 0;
            start_time = soundio_os_get_time();
            continue;
        }

        if (SOUNDIO_ATOMIC_LOAD(osd->pause_requested)) {
            start_time = now;
            frames_consumed = 0;
            continue;
        }

        int fill_bytes = soundio_ring_buffer_fill_count(&osd->ring_buffer);
        int fill_frames = fill_bytes / outstream->bytes_per_frame;
        int free_bytes = soundio_ring_buffer_capacity(&osd->ring_buffer) - fill_bytes;
        int free_frames = free_bytes / outstream->bytes_per_frame;

        double total_time = soundio_os_get_time() - start_time;


        if (difftime( time(NULL), last_recv_time) < 5.0) {
	        unsigned int slen = sizeof(si_other);
          sprintf(buf, "Time: %f", total_time);
          if (sendto(sock, buf, strlen(buf), 0, (struct sockaddr*) &si_other, slen) == -1) {
              die("sendto()");
          }
        }

        long total_frames = total_time * outstream->sample_rate;
        int frames_to_kill = total_frames - frames_consumed;
        int read_count = soundio_int_min(frames_to_kill, fill_frames);
        int byte_count = read_count * outstream->bytes_per_frame;
        soundio_ring_buffer_advance_read_ptr(&osd->ring_buffer, byte_count);
        frames_consumed += read_count;

        if (frames_to_kill > fill_frames) {
            outstream->underflow_callback(outstream);
            osd->frames_left = free_frames;
            if (free_frames > 0)
                outstream->write_callback(outstream, 0, free_frames);
            frames_consumed = 0;
            start_time = soundio_os_get_time();
        } else if (free_frames > 0) {
            osd->frames_left = free_frames;
            outstream->write_callback(outstream, 0, free_frames);
        }
    }
}

static void capture_thread_run(void *arg) {
    struct SoundIoInStreamPrivate *is = (struct SoundIoInStreamPrivate *)arg;
    struct SoundIoInStream *instream = &is->pub;
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;

    long frames_consumed = 0;
    double start_time = soundio_os_get_time();
    while (SOUNDIO_ATOMIC_FLAG_TEST_AND_SET(isd->abort_flag)) {
        double now = soundio_os_get_time();
        double time_passed = now - start_time;
        double next_period = start_time +
            ceil_dbl(time_passed / isd->period_duration) * isd->period_duration;
        double relative_time = next_period - now;
        soundio_os_cond_timed_wait(isd->cond, NULL, relative_time);

        if (SOUNDIO_ATOMIC_LOAD(isd->pause_requested)) {
            start_time = now;
            frames_consumed = 0;
            continue;
        }

        int fill_bytes = soundio_ring_buffer_fill_count(&isd->ring_buffer);
        int free_bytes = soundio_ring_buffer_capacity(&isd->ring_buffer) - fill_bytes;
        int fill_frames = fill_bytes / instream->bytes_per_frame;
        int free_frames = free_bytes / instream->bytes_per_frame;

        double total_time = soundio_os_get_time() - start_time;
        long total_frames = total_time * instream->sample_rate;
        int frames_to_kill = total_frames - frames_consumed;
        int write_count = soundio_int_min(frames_to_kill, free_frames);
        int byte_count = write_count * instream->bytes_per_frame;
        soundio_ring_buffer_advance_write_ptr(&isd->ring_buffer, byte_count);
        frames_consumed += write_count;

        if (frames_to_kill > free_frames) {
            instream->overflow_callback(instream);
            frames_consumed = 0;
            start_time = soundio_os_get_time();
        }
        if (fill_frames > 0) {
            isd->frames_left = fill_frames;
            instream->read_callback(instream, 0, fill_frames);
        }
    }
}

static void destroy_remote(struct SoundIoPrivate *si) {
    struct SoundIoRemote *sid = &si->backend_data.remote;

    if (sid->cond)
        soundio_os_cond_destroy(sid->cond);

    if (sid->mutex)
        soundio_os_mutex_destroy(sid->mutex);
}

static void flush_events_remote(struct SoundIoPrivate *si) {
    struct SoundIo *soundio = &si->pub;
    struct SoundIoRemote *sid = &si->backend_data.remote;
    if (sid->devices_emitted)
        return;
    sid->devices_emitted = true;
    soundio->on_devices_change(soundio);
}

static void wait_events_remote(struct SoundIoPrivate *si) {
    struct SoundIoRemote *sid = &si->backend_data.remote;
    flush_events_remote(si);
    soundio_os_cond_wait(sid->cond, NULL);
}

static void wakeup_remote(struct SoundIoPrivate *si) {
    struct SoundIoRemote *sid = &si->backend_data.remote;
    soundio_os_cond_signal(sid->cond, NULL);
}

static void force_device_scan_remote(struct SoundIoPrivate *si) {
    // nothing to do; remote devices never change
}

static void outstream_destroy_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;

    if (osd->thread) {
        SOUNDIO_ATOMIC_FLAG_CLEAR(osd->abort_flag);
        soundio_os_cond_signal(osd->cond, NULL);
        soundio_os_thread_destroy(osd->thread);
        osd->thread = NULL;
    }
    soundio_os_cond_destroy(osd->cond);
    osd->cond = NULL;

    soundio_ring_buffer_deinit(&osd->ring_buffer);
}

static enum SoundIoError outstream_open_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;
    struct SoundIoOutStream *outstream = &os->pub;
    struct SoundIoDevice *device = outstream->device;

    SOUNDIO_ATOMIC_FLAG_TEST_AND_SET(osd->clear_buffer_flag);
    SOUNDIO_ATOMIC_STORE(osd->pause_requested, false);

    if (outstream->software_latency == 0.0) {
        outstream->software_latency = soundio_double_clamp(
                device->software_latency_min, 1.0, device->software_latency_max);
    }

    osd->period_duration = outstream->software_latency / 2.0;

    enum SoundIoError err;
    int buffer_size = outstream->bytes_per_frame * outstream->sample_rate * outstream->software_latency;
    if ((err = soundio_ring_buffer_init(&osd->ring_buffer, buffer_size))) {
        outstream_destroy_remote(si, os);
        return err;
    }
    int actual_capacity = soundio_ring_buffer_capacity(&osd->ring_buffer);
    osd->buffer_frame_count = actual_capacity / outstream->bytes_per_frame;
    outstream->software_latency = osd->buffer_frame_count / (double) outstream->sample_rate;

    osd->cond = soundio_os_cond_create();
    if (!osd->cond) {
        outstream_destroy_remote(si, os);
        return SoundIoErrorNoMem;
    }


    return 0;
}

static enum SoundIoError outstream_pause_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os,
        bool pause)
{
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;
    SOUNDIO_ATOMIC_STORE(osd->pause_requested, pause);
    return SoundIoErrorNone;
}

static enum SoundIoError outstream_start_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;
    struct SoundIo *soundio = &si->pub;
    assert(!osd->thread);
    SOUNDIO_ATOMIC_FLAG_TEST_AND_SET(osd->abort_flag);
    enum SoundIoError err;
    if ((err = soundio_os_thread_create(playback_thread_run, os, soundio, &osd->thread))) {
        return err;
    }
    return SoundIoErrorNone;
}

static enum SoundIoError outstream_begin_write_remote(struct SoundIoPrivate *si,
        struct SoundIoOutStreamPrivate *os, struct SoundIoChannelArea **out_areas, int *frame_count)
{
    struct SoundIoOutStream *outstream = &os->pub;
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;

    if (*frame_count > osd->frames_left)
        return SoundIoErrorInvalid;

    char *write_ptr = soundio_ring_buffer_write_ptr(&osd->ring_buffer);
    for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
        osd->areas[ch].ptr = write_ptr + outstream->bytes_per_sample * ch;
        osd->areas[ch].step = outstream->bytes_per_frame;
    }

    osd->write_frame_count = *frame_count;
    *out_areas = osd->areas;
    return SoundIoErrorNone;
}

static enum SoundIoError outstream_end_write_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;
    struct SoundIoOutStream *outstream = &os->pub;
    int byte_count = osd->write_frame_count * outstream->bytes_per_frame;
    soundio_ring_buffer_advance_write_ptr(&osd->ring_buffer, byte_count);
    osd->frames_left -= osd->write_frame_count;
    return SoundIoErrorNone;
}

static enum SoundIoError outstream_clear_buffer_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;
    SOUNDIO_ATOMIC_FLAG_CLEAR(osd->clear_buffer_flag);
    soundio_os_cond_signal(osd->cond, NULL);
    return SoundIoErrorNone;
}

static enum SoundIoError outstream_get_latency_remote(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os, double *out_latency) {
    struct SoundIoOutStream *outstream = &os->pub;
    struct SoundIoOutStreamRemote *osd = &os->backend_data.remote;
    int fill_bytes = soundio_ring_buffer_fill_count(&osd->ring_buffer);

    *out_latency = (fill_bytes / outstream->bytes_per_frame) / (double)outstream->sample_rate;
    return SoundIoErrorNone;
}

static void instream_destroy_remote(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;

    if (isd->thread) {
        SOUNDIO_ATOMIC_FLAG_CLEAR(isd->abort_flag);
        soundio_os_cond_signal(isd->cond, NULL);
        soundio_os_thread_destroy(isd->thread);
        isd->thread = NULL;
    }
    soundio_os_cond_destroy(isd->cond);
    isd->cond = NULL;

    soundio_ring_buffer_deinit(&isd->ring_buffer);
}

static enum SoundIoError instream_open_remote(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;
    struct SoundIoInStream *instream = &is->pub;
    struct SoundIoDevice *device = instream->device;

    SOUNDIO_ATOMIC_STORE(isd->pause_requested, false);

    if (instream->software_latency == 0.0) {
        instream->software_latency = soundio_double_clamp(
                device->software_latency_min, 1.0, device->software_latency_max);
    }

    isd->period_duration = instream->software_latency;

    double target_buffer_duration = isd->period_duration * 4.0;

    int err;
    int buffer_size = instream->bytes_per_frame * instream->sample_rate * target_buffer_duration;
    if ((err = soundio_ring_buffer_init(&isd->ring_buffer, buffer_size))) {
        instream_destroy_remote(si, is);
        return err;
    }

    int actual_capacity = soundio_ring_buffer_capacity(&isd->ring_buffer);
    isd->buffer_frame_count = actual_capacity / instream->bytes_per_frame;

    isd->cond = soundio_os_cond_create();
    if (!isd->cond) {
        instream_destroy_remote(si, is);
        return SoundIoErrorNoMem;
    }


    return 0;
}

static enum SoundIoError instream_pause_remote(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is, bool pause) {
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;
    SOUNDIO_ATOMIC_STORE(isd->pause_requested, pause);
    return 0;
}

static enum SoundIoError instream_start_remote(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;
    struct SoundIo *soundio = &si->pub;
    assert(!isd->thread);
    SOUNDIO_ATOMIC_FLAG_TEST_AND_SET(isd->abort_flag);
    enum SoundIoError err;
    if ((err = soundio_os_thread_create(capture_thread_run, is, soundio, &isd->thread))) {
        return err;
    }
    return 0;
}

static enum SoundIoError instream_begin_read_remote(struct SoundIoPrivate *si,
        struct SoundIoInStreamPrivate *is, struct SoundIoChannelArea **out_areas, int *frame_count)
{
    struct SoundIoInStream *instream = &is->pub;
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;

    assert(*frame_count <= isd->frames_left);

    char *read_ptr = soundio_ring_buffer_read_ptr(&isd->ring_buffer);
    for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
        isd->areas[ch].ptr = read_ptr + instream->bytes_per_sample * ch;
        isd->areas[ch].step = instream->bytes_per_frame;
    }

    isd->read_frame_count = *frame_count;
    *out_areas = isd->areas;

    return 0;
}

static enum SoundIoError instream_end_read_remote(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    struct SoundIoInStreamRemote *isd = &is->backend_data.remote;
    struct SoundIoInStream *instream = &is->pub;
    int byte_count = isd->read_frame_count * instream->bytes_per_frame;
    soundio_ring_buffer_advance_read_ptr(&isd->ring_buffer, byte_count);
    isd->frames_left -= isd->read_frame_count;
    return 0;
}

static enum SoundIoError instream_get_latency_remote(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is, double *out_latency) {
    struct SoundIoInStream *instream = &is->pub;
    struct SoundIoInStreamRemote *osd = &is->backend_data.remote;
    int fill_bytes = soundio_ring_buffer_fill_count(&osd->ring_buffer);

    *out_latency = (fill_bytes / instream->bytes_per_frame) / (double)instream->sample_rate;
    return 0;
}

static enum SoundIoError set_all_device_formats(struct SoundIoDevice *device) {
    device->format_count = 22;
    device->formats = ALLOCATE(enum SoundIoFormat, device->format_count);
    if (!device->formats)
        return SoundIoErrorNoMem;

    device->formats[0] = SoundIoFormatFloat32NE;
    device->formats[1] = SoundIoFormatFloat32FE;
    device->formats[2] = SoundIoFormatS32NE;
    device->formats[3] = SoundIoFormatS32FE;
    device->formats[4] = SoundIoFormatU32NE;
    device->formats[5] = SoundIoFormatU32FE;
    device->formats[6] = SoundIoFormatS24NE;
    device->formats[7] = SoundIoFormatS24FE;
    device->formats[8] = SoundIoFormatU24NE;
    device->formats[9] = SoundIoFormatU24FE;
    device->formats[10] = SoundIoFormatS24PackedNE;
    device->formats[11] = SoundIoFormatS24PackedFE;
    device->formats[12] = SoundIoFormatU24PackedNE;
    device->formats[13] = SoundIoFormatU24PackedFE;
    device->formats[14] = SoundIoFormatFloat64NE;
    device->formats[15] = SoundIoFormatFloat64FE;
    device->formats[16] = SoundIoFormatS16NE;
    device->formats[17] = SoundIoFormatS16FE;
    device->formats[18] = SoundIoFormatU16NE;
    device->formats[19] = SoundIoFormatU16FE;
    device->formats[20] = SoundIoFormatS8;
    device->formats[21] = SoundIoFormatU8;

    return 0;
}

static void set_all_device_sample_rates(struct SoundIoDevice *device) {
    struct SoundIoDevicePrivate *dev = (struct SoundIoDevicePrivate *)device;
    device->sample_rate_count = 1;
    device->sample_rates = &dev->prealloc_sample_rate_range;
    device->sample_rates[0].min = SOUNDIO_MIN_SAMPLE_RATE;
    device->sample_rates[0].max = SOUNDIO_MAX_SAMPLE_RATE;
}

static enum SoundIoError set_all_device_channel_layouts(struct SoundIoDevice *device) {
    device->layout_count = soundio_channel_layout_builtin_count();
    device->layouts = ALLOCATE(struct SoundIoChannelLayout, device->layout_count);
    if (!device->layouts)
        return SoundIoErrorNoMem;
    for (int i = 0; i < device->layout_count; i += 1)
        device->layouts[i] = *soundio_channel_layout_get_builtin(i);
    return 0;
}


static void socket_input_thread(void *arg) {
    printf("input thread\n");

    //int i;
    unsigned int slen = sizeof(si_other);
    int recv_len;
    char buf[BUFLEN];

    //keep listening for data
    while(1)
    {
//        printf("Waiting for data...");
//        fflush(stdout);
         
        //try to receive some data, this is a blocking call
        if ((recv_len = recvfrom(sock, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen)) == -1)
        {
            die("recvfrom()");
        }

        
        
        char str[INET6_ADDRSTRLEN];
        //print details of the client/peer and the data received
        inet_ntop(AF_INET, &(si_other.sin_addr), str, INET_ADDRSTRLEN);

        printf("Received packet from %s:%d  Data:%s\n", str, ntohs(si_other.sin_port), buf);
//        printf("Data: %s\n" , buf);
         
        //now reply the client with the same data
//        if (sendto(sock, buf, recv_len, 0, (struct sockaddr*) &si_other, slen) == -1)
//      {
//            die("sendto()");
//        }
        last_recv_time = time(NULL);
    }
}

// init the remote
enum SoundIoError soundio_remote_init(struct SoundIoPrivate *si) {
    struct SoundIo *soundio = &si->pub;
    struct SoundIoRemote *sid = &si->backend_data.remote;

    sid->mutex = soundio_os_mutex_create();
    if (!sid->mutex) {
        destroy_remote(si);
        return SoundIoErrorNoMem;
    }

    sid->cond = soundio_os_cond_create();
    if (!sid->cond) {
        destroy_remote(si);
        return SoundIoErrorNoMem;
    }

    assert(!si->safe_devices_info);
    si->safe_devices_info = ALLOCATE(struct SoundIoDevicesInfo, 1);
    if (!si->safe_devices_info) {
        destroy_remote(si);
        return SoundIoErrorNoMem;
    }

    si->safe_devices_info->default_input_index = 0;
    si->safe_devices_info->default_output_index = 0;

    // create network socket

    // open socket


    //create a UDP socket
    if ((sock=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }
     
    // zero out the structure
    memset((char *) &si_me, 0, sizeof(si_me));
     
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(PORT);

    //si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    if (inet_pton(AF_INET, SERVER_IP, &si_me.sin_addr) == 0) 
    {
        die("inet_pton() failed");
    }

    //bind socket to port
    if( bind(sock, (struct sockaddr*)&si_me, sizeof(si_me) ) == -1)
    {
        die("bind");
    } else {
      printf("socket created\n");
    }

    struct SoundIoOsThread *thread;
    int err;

    if ((err = soundio_os_thread_create(socket_input_thread, NULL, soundio, &thread))) {
        return err;
    }




    // create output device
    {
        struct SoundIoDevicePrivate *dev = ALLOCATE(struct SoundIoDevicePrivate, 1);
        if (!dev) {
            destroy_remote(si);
            return SoundIoErrorNoMem;
        }
        struct SoundIoDevice *device = &dev->pub;

        device->ref_count = 1;
        device->soundio = soundio;
        device->id = strdup("remote-out");
        device->name = strdup("Remote Output Device");
        if (!device->id || !device->name) {
            soundio_device_unref(device);
            destroy_remote(si);
            return SoundIoErrorNoMem;
        }

        int err;
        if ((err = set_all_device_channel_layouts(device))) {
            soundio_device_unref(device);
            destroy_remote(si);
            return err;
        }
        if ((err = set_all_device_formats(device))) {
            soundio_device_unref(device);
            destroy_remote(si);
            return err;
        }
        set_all_device_sample_rates(device);

        device->software_latency_current = 0.1;
        device->software_latency_min = 0.01;
        device->software_latency_max = 4.0;

        device->sample_rate_current = 48000;
        device->aim = SoundIoDeviceAimOutput;

        if (SoundIoListDevicePtr_append(&si->safe_devices_info->output_devices, device)) {
            soundio_device_unref(device);
            destroy_remote(si);
            return SoundIoErrorNoMem;
        }
    }

    // create input device
    {
        struct SoundIoDevicePrivate *dev = ALLOCATE(struct SoundIoDevicePrivate, 1);
        if (!dev) {
            destroy_remote(si);
            return SoundIoErrorNoMem;
        }
        struct SoundIoDevice *device = &dev->pub;

        device->ref_count = 1;
        device->soundio = soundio;
        device->id = strdup("remote-in");
        device->name = strdup("Remote Input Device");
        if (!device->id || !device->name) {
            soundio_device_unref(device);
            destroy_remote(si);
            return SoundIoErrorNoMem;
        }

        int err;
        if ((err = set_all_device_channel_layouts(device))) {
            soundio_device_unref(device);
            destroy_remote(si);
            return err;
        }

        if ((err = set_all_device_formats(device))) {
            soundio_device_unref(device);
            destroy_remote(si);
            return err;
        }
        set_all_device_sample_rates(device);
        device->software_latency_current = 0.1;
        device->software_latency_min = 0.01;
        device->software_latency_max = 4.0;
        device->sample_rate_current = 48000;
        device->aim = SoundIoDeviceAimInput;

        if (SoundIoListDevicePtr_append(&si->safe_devices_info->input_devices, device)) {
            soundio_device_unref(device);
            destroy_remote(si);
            return SoundIoErrorNoMem;
        }
    }


    si->destroy = destroy_remote;
    si->flush_events = flush_events_remote;
    si->wait_events = wait_events_remote;
    si->wakeup = wakeup_remote;
    si->force_device_scan = force_device_scan_remote;

    si->outstream_open = outstream_open_remote;
    si->outstream_destroy = outstream_destroy_remote;
    si->outstream_start = outstream_start_remote;
    si->outstream_begin_write = outstream_begin_write_remote;
    si->outstream_end_write = outstream_end_write_remote;
    si->outstream_clear_buffer = outstream_clear_buffer_remote;
    si->outstream_pause = outstream_pause_remote;
    si->outstream_get_latency = outstream_get_latency_remote;

    si->instream_open = instream_open_remote;
    si->instream_destroy = instream_destroy_remote;
    si->instream_start = instream_start_remote;
    si->instream_begin_read = instream_begin_read_remote;
    si->instream_end_read = instream_end_read_remote;
    si->instream_pause = instream_pause_remote;
    si->instream_get_latency = instream_get_latency_remote;

    return 0;
}
