// SPDX-License-Identifier: GPL-3.0-or-later
// libbbmp-out: sound output for the systems without PulseAudio (macOS: CoreAudio), through
// miniaudio. The player writes 16-bit stereo at one rate into a ring buffer; miniaudio's
// device thread takes from it, and plays silence when it runs dry. The calls mirror
// PulseAudio's simple API, which the player uses on Linux: a blocking write, the latency,
// flush (drop what is waiting) and drain (wait until it is heard).
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#if defined(__APPLE__)
#define MA_NO_RUNTIME_LINKING       // CoreAudio linked as frameworks, not opened at run time
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ma_device device;
    ma_pcm_rb rb;
    ma_uint32 rate;
    volatile int flush;             // the writer asks the device thread to drop what waits
    char error[256];
} Out;

static char last_error[256];

static void callback(ma_device *dev, void *output, const void *input, ma_uint32 frames) {
    (void)input;
    Out *o = (Out *)dev->pUserData;
    ma_int16 *dst = (ma_int16 *)output;
    if (o->flush) {
        ma_uint32 waiting = ma_pcm_rb_available_read(&o->rb);
        ma_pcm_rb_seek_read(&o->rb, waiting);
        o->flush = 0;
    }
    ma_uint32 done = 0;
    while (done < frames) {
        ma_uint32 n = frames - done;
        void *src;
        if (ma_pcm_rb_acquire_read(&o->rb, &n, &src) != MA_SUCCESS || n == 0)
            break;
        memcpy(dst + done * 2, src, n * 4);
        ma_pcm_rb_commit_read(&o->rb, n);
        done += n;
    }
    if (done < frames)
        memset(dst + done * 2, 0, (frames - done) * 4);
}

const char *bbmp_out_error(void) { return last_error; }

// rate: frames a second; buffer_ms: how much may wait; null_device: 1 plays into miniaudio's
// silent null device (for tests: nothing reaches the speakers)
void *bbmp_out_open(unsigned rate, unsigned buffer_ms, int null_device) {
    Out *o = calloc(1, sizeof *o);
    if (!o) { strcpy(last_error, "out of memory"); return NULL; }
    o->rate = rate;
    ma_uint32 frames = rate * buffer_ms / 1000;
    if (ma_pcm_rb_init(ma_format_s16, 2, frames, NULL, NULL, &o->rb) != MA_SUCCESS) {
        strcpy(last_error, "can't make the sound buffer");
        free(o);
        return NULL;
    }
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = rate;
    cfg.dataCallback = callback;
    cfg.pUserData = o;
    ma_result r;
    if (null_device) {
        ma_backend backends[] = { ma_backend_null };
        r = ma_device_init_ex(backends, 1, NULL, &cfg, &o->device);
    } else {
        r = ma_device_init(NULL, &cfg, &o->device);
    }
    if (r != MA_SUCCESS) {
        snprintf(last_error, sizeof last_error, "can't open the sound device: %s",
                 ma_result_description(r));
        ma_pcm_rb_uninit(&o->rb);
        free(o);
        return NULL;
    }
    if ((r = ma_device_start(&o->device)) != MA_SUCCESS) {
        snprintf(last_error, sizeof last_error, "can't start the sound device: %s",
                 ma_result_description(r));
        ma_device_uninit(&o->device);
        ma_pcm_rb_uninit(&o->rb);
        free(o);
        return NULL;
    }
    return o;
}

// waits while the buffer is full; 0 when all is written
int bbmp_out_write(void *h, const void *pcm, size_t bytes) {
    Out *o = (Out *)h;
    const char *p = (const char *)pcm;
    ma_uint32 left = (ma_uint32)(bytes / 4);
    while (left) {
        ma_uint32 n = left;
        void *dst;
        if (ma_pcm_rb_acquire_write(&o->rb, &n, &dst) != MA_SUCCESS)
            return -1;
        if (n == 0) {
            ma_sleep(5);
            continue;
        }
        memcpy(dst, p, n * 4);
        ma_pcm_rb_commit_write(&o->rb, n);
        p += n * 4;
        left -= n;
    }
    return 0;
}

// seconds between what was written and what is heard: what waits in the buffer, and the
// device's own period
double bbmp_out_latency(void *h) {
    Out *o = (Out *)h;
    ma_uint32 waiting = ma_pcm_rb_available_read(&o->rb);
    ma_uint32 period = o->device.playback.internalPeriodSizeInFrames;
    ma_uint32 rate = o->device.playback.internalSampleRate ? o->device.playback.internalSampleRate
                                                            : o->rate;
    return (double)waiting / o->rate + (double)period / rate;
}

void bbmp_out_flush(void *h) {
    Out *o = (Out *)h;
    o->flush = 1;
    for (int i = 0; o->flush && i < 200; i++)     // the device thread takes it within a period
        ma_sleep(2);
}

void bbmp_out_drain(void *h) {
    Out *o = (Out *)h;
    for (int i = 0; ma_pcm_rb_available_read(&o->rb) && i < 2000; i++)
        ma_sleep(5);
}

void bbmp_out_close(void *h) {
    Out *o = (Out *)h;
    if (!o) return;
    ma_device_uninit(&o->device);
    ma_pcm_rb_uninit(&o->rb);
    free(o);
}
