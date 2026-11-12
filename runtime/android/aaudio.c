// libaaudio.so on SDL2 audio: the AAudio (Android AudioStream) C API Unity 6's FMOD looks up by
// dlsym. Output only; an input stream is refused as AAUDIO_ERROR_UNIMPLEMENTED.
//
// The streams are built on the OpenSL ES shim next door (opensles.c): each AAudioStream is one of
// its PCM buffer-queue players, so AAudio's data callback runs from that player's buffer-queue
// callback (on SDL's audio thread, or the pipe / no-output writer's), filling the block the device
// just used up, and audio goes through the same output choice as OpenSL (ANDROID_SLES_OUTPUT,
// ANDROID_SLES_* and SDL's own variables, ua_host's Wwise mix). The builder's settings map onto that
// player: format PCM_I16 -> SL_DATAFORMAT_PCM 16-bit, PCM_FLOAT -> SL_ANDROID_DATAFORMAT_PCM_EX
// float; framesPerDataCallback (default 512) is one buffer of the queue, the buffer capacity a
// count of them (2 by default); rate defaults to UA_AUDIO_RATE, else 48000, channels to 2.
//
// AAudioStream_write()/read() and the timing/tee APIs are not provided: this serves the callback
// model (AAudioStreamBuilder_setDataCallback), as FMOD uses it.
#define _GNU_SOURCE
#include "sles_min.h"
#include "shim_internal.h"
#include "android_shims.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- AAudio constants (NDK aaudio/AAudio.h) -------------------------------------------------
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_stream_state_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_usage_t;
typedef int32_t aaudio_content_type_t;
typedef int32_t aaudio_data_callback_result_t;
typedef int32_t aaudio_session_id_t;

#define AAUDIO_OK 0
#define AAUDIO_ERROR_DISCONNECTED (-899)
#define AAUDIO_ERROR_ILLEGAL_ARGUMENT (-898)
#define AAUDIO_ERROR_INTERNAL (-896)
#define AAUDIO_ERROR_INVALID_STATE (-895)
#define AAUDIO_ERROR_INVALID_HANDLE (-892)
#define AAUDIO_ERROR_UNIMPLEMENTED (-890)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define AAUDIO_ERROR_NO_FREE_HANDLES (-888)
#define AAUDIO_ERROR_NO_MEMORY (-887)
#define AAUDIO_ERROR_NULL (-886)
#define AAUDIO_ERROR_TIMEOUT (-885)
#define AAUDIO_ERROR_WOULD_BLOCK (-884)
#define AAUDIO_ERROR_INVALID_FORMAT (-883)
#define AAUDIO_ERROR_OUT_OF_RANGE (-882)
#define AAUDIO_ERROR_NO_SERVICE (-881)
#define AAUDIO_ERROR_INVALID_RATE (-880)

#define AAUDIO_DIRECTION_OUTPUT 0
#define AAUDIO_DIRECTION_INPUT 1

#define AAUDIO_FORMAT_INVALID (-1)
#define AAUDIO_FORMAT_UNSPECIFIED 0
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_FORMAT_PCM_FLOAT 2

#define AAUDIO_STREAM_STATE_UNINITIALIZED 0
#define AAUDIO_STREAM_STATE_UNKNOWN 1
#define AAUDIO_STREAM_STATE_OPEN 2
#define AAUDIO_STREAM_STATE_STARTING 3
#define AAUDIO_STREAM_STATE_STARTED 4
#define AAUDIO_STREAM_STATE_PAUSING 5
#define AAUDIO_STREAM_STATE_PAUSED 6
#define AAUDIO_STREAM_STATE_FLUSHING 7
#define AAUDIO_STREAM_STATE_FLUSHED 8
#define AAUDIO_STREAM_STATE_STOPPING 9
#define AAUDIO_STREAM_STATE_STOPPED 10
#define AAUDIO_STREAM_STATE_CLOSING 11
#define AAUDIO_STREAM_STATE_CLOSED 12
#define AAUDIO_STREAM_STATE_DISCONNECTED 13

#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
#define AAUDIO_CALLBACK_RESULT_STOP 1

#define AAUDIO_SHARING_MODE_EXCLUSIVE 0
#define AAUDIO_SHARING_MODE_SHARED 1

#define AAUDIO_PERFORMANCE_MODE_NONE 10
#define AAUDIO_PERFORMANCE_MODE_POWER_SAVING 11
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12

#define AAUDIO_SESSION_ID_NONE (-1)
#define AAUDIO_UNSPECIFIED 0

#define AAUDIO_DEFAULT_FRAMES 512            // framesPerDataCallback when the app sets none

typedef struct AAudioStream AAudioStream;
typedef struct AAudioStreamBuilder AAudioStreamBuilder;
typedef aaudio_data_callback_result_t (*aaudio_data_callback_t)(AAudioStream *stream, void *userData,
                                                                void *audioData, int32_t numFrames);
typedef void (*aaudio_error_callback_t)(AAudioStream *stream, aaudio_result_t error, void *userData);

struct AAudioStreamBuilder {
    int32_t device_id;
    aaudio_direction_t direction;
    aaudio_sharing_mode_t sharing;
    int32_t sample_rate;
    int32_t channels;
    aaudio_format_t format;
    int32_t capacity_frames;
    aaudio_performance_mode_t perf_mode;
    aaudio_usage_t usage;
    aaudio_content_type_t content_type;
    int32_t session_id;
    int32_t frames_per_cb;
    aaudio_data_callback_t data_cb;
    void *data_user;
    aaudio_error_callback_t error_cb;
    void *error_user;
};

struct AAudioStream {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int refs;                    // API calls in flight (close waits for them)
    volatile int closing;
    aaudio_stream_state_t state;
    // immutable after open
    int32_t device_id, sharing, sample_rate, channels, format, perf_mode, session_id, usage, content_type;
    int32_t frames_per_cb, capacity_frames, buffer_frames, xruns;
    int bytes_per_frame, frame_bytes_total, nbuf;
    unsigned next;               // buffers taken from the queue so far (round robin)
    uint8_t *block;              // nbuf buffers of frame_bytes_total
    aaudio_data_callback_t data_cb;
    void *data_user;
    aaudio_error_callback_t error_cb;
    void *error_user;
    // the OpenSL player behind it
    SLObjectItf engine_obj, mix_obj, player_obj;
    SLPlayItf play;
    SLBufferQueueItf bq;
};

static void set_state(AAudioStream *s, aaudio_stream_state_t st)
{
    pthread_mutex_lock(&s->lock);
    s->state = st;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static AAudioStream *s_ref(AAudioStream *s)
{
    if (!s) return NULL;
    pthread_mutex_lock(&s->lock);
    s->refs++;
    pthread_mutex_unlock(&s->lock);
    return s;
}

static void s_unref(AAudioStream *s)
{
    pthread_mutex_lock(&s->lock);
    if (--s->refs == 0 && s->closing) pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void report_error(AAudioStream *s, aaudio_result_t err)
{
    if (s->error_cb) s->error_cb(s, err, s->error_user);
}

// One buffer was used up: the app fills it, and it goes back on the queue. Runs on the player's
// output thread; opensles.c calls it with its own lock released.
static void stream_bq_callback(SLBufferQueueItf bq, void *ctx)
{
    AAudioStream *s = ctx;
    (void)bq;
    if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) return;
    unsigned i = s->next % (unsigned)s->nbuf;
    s->next++;
    uint8_t *buf = s->block + (size_t)i * (size_t)s->frame_bytes_total;
    memset(buf, 0, (size_t)s->frame_bytes_total);
    if (!s->data_cb) return;                    // nothing to play: the queue drains to silence
    aaudio_data_callback_result_t r = s->data_cb(s, s->data_user, buf, s->frames_per_cb);
    if (r == AAUDIO_CALLBACK_RESULT_STOP) {
        set_state(s, AAUDIO_STREAM_STATE_PAUSED);
        if (s->play) (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PAUSED);
        return;
    }
    if ((*s->bq)->Enqueue(s->bq, buf, (SLuint32)s->frame_bytes_total) != SL_RESULT_SUCCESS)
        __atomic_fetch_add(&s->xruns, 1, __ATOMIC_RELAXED);
}

static void stream_player_destroy(AAudioStream *s)
{
    if (s->player_obj) { (*s->player_obj)->Destroy(s->player_obj); s->player_obj = NULL; }
    s->play = NULL;
    s->bq = NULL;
    if (s->mix_obj) { (*s->mix_obj)->Destroy(s->mix_obj); s->mix_obj = NULL; }
    if (s->engine_obj) { (*s->engine_obj)->Destroy(s->engine_obj); s->engine_obj = NULL; }
}

// the OpenSL side: engine, output mix and one PCM buffer-queue player carrying `s`
static aaudio_result_t stream_player_open(AAudioStream *s)
{
    SLEngineItf e = NULL;
    SLresult r;
    if ((r = slCreateEngine(&s->engine_obj, 0, NULL, 0, NULL, NULL)) != SL_RESULT_SUCCESS || !s->engine_obj)
        return r == SL_RESULT_MEMORY_FAILURE ? AAUDIO_ERROR_NO_MEMORY : AAUDIO_ERROR_UNAVAILABLE;
    if ((*s->engine_obj)->Realize(s->engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return AAUDIO_ERROR_UNAVAILABLE;
    if ((*s->engine_obj)->GetInterface(s->engine_obj, SL_IID_ENGINE, &e) != SL_RESULT_SUCCESS || !e)
        return AAUDIO_ERROR_UNAVAILABLE;
    if ((*e)->CreateOutputMix(e, &s->mix_obj, 0, NULL, NULL) != SL_RESULT_SUCCESS || !s->mix_obj)
        return AAUDIO_ERROR_UNAVAILABLE;
    if ((*s->mix_obj)->Realize(s->mix_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return AAUDIO_ERROR_UNAVAILABLE;

    SLDataLocator_BufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, (SLuint32)s->nbuf };
    SLuint32 mask = s->channels == 1 ? SL_SPEAKER_FRONT_CENTER
                   : s->channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                                      : SL_SPEAKER_FRONT_LEFT;
    SLAndroidDataFormat_PCM_EX ex;
    SLDataFormat_PCM pcm;
    void *fmt;
    if (s->format == AAUDIO_FORMAT_PCM_FLOAT) {
        memset(&ex, 0, sizeof ex);
        ex.formatType = SL_ANDROID_DATAFORMAT_PCM_EX;
        ex.numChannels = (SLuint32)s->channels;
        ex.sampleRate = (SLuint32)s->sample_rate * 1000u;
        ex.bitsPerSample = ex.containerSize = 32;
        ex.channelMask = mask;
        ex.endianness = SL_BYTEORDER_LITTLEENDIAN;
        ex.representation = SL_ANDROID_PCM_REPRESENTATION_FLOAT;
        fmt = &ex;
    } else {
        memset(&pcm, 0, sizeof pcm);
        pcm.formatType = SL_DATAFORMAT_PCM;
        pcm.numChannels = (SLuint32)s->channels;
        pcm.samplesPerSec = (SLuint32)s->sample_rate * 1000u;
        pcm.bitsPerSample = pcm.containerSize = 16;
        pcm.channelMask = mask;
        pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;
        fmt = &pcm;
    }
    SLDataSource src = { &loc, fmt };
    SLDataLocator_OutputMix outloc = { SL_DATALOCATOR_OUTPUTMIX, s->mix_obj };
    SLDataSink snk = { &outloc, NULL };
    const SLInterfaceID ids[2] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    if ((*e)->CreateAudioPlayer(e, &s->player_obj, &src, &snk, 2, ids, req) != SL_RESULT_SUCCESS || !s->player_obj)
        return AAUDIO_ERROR_UNAVAILABLE;
    SLAndroidConfigurationItf cfg = NULL;
    if ((*s->player_obj)->GetInterface(s->player_obj, SL_IID_ANDROIDCONFIGURATION, &cfg) == SL_RESULT_SUCCESS && cfg) {
        SLint32 stream_type = SL_ANDROID_STREAM_MEDIA;
        (*cfg)->SetConfiguration(cfg, (const SLchar *)SL_ANDROID_KEY_STREAM_TYPE, &stream_type, sizeof stream_type);
    }
    if ((*s->player_obj)->Realize(s->player_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return AAUDIO_ERROR_UNAVAILABLE;
    if ((*s->player_obj)->GetInterface(s->player_obj, SL_IID_PLAY, &s->play) != SL_RESULT_SUCCESS || !s->play)
        return AAUDIO_ERROR_UNAVAILABLE;
    if ((*s->player_obj)->GetInterface(s->player_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &s->bq) != SL_RESULT_SUCCESS || !s->bq)
        return AAUDIO_ERROR_UNAVAILABLE;
    if ((*s->bq)->RegisterCallback(s->bq, stream_bq_callback, s) != SL_RESULT_SUCCESS) return AAUDIO_ERROR_UNAVAILABLE;

    s->block = calloc((size_t)s->nbuf, (size_t)s->frame_bytes_total);
    if (!s->block) return AAUDIO_ERROR_NO_MEMORY;
    // prime the queue with silence: the app's callback fills the buffers as the output takes them
    // (state STARTED), as AAudio calls it on its thread, not here
    for (int i = 0; i < s->nbuf; i++) {
        uint8_t *buf = s->block + (size_t)i * (size_t)s->frame_bytes_total;
        memset(buf, 0, (size_t)s->frame_bytes_total);
        if ((*s->bq)->Enqueue(s->bq, buf, (SLuint32)s->frame_bytes_total) != SL_RESULT_SUCCESS) {
            __atomic_fetch_add(&s->xruns, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    return AAUDIO_OK;
}

// ---- builder --------------------------------------------------------------------------------
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder)
{
    if (!builder) return AAUDIO_ERROR_NULL;
    *builder = NULL;
    AAudioStreamBuilder *b = calloc(1, sizeof *b);
    if (!b) return AAUDIO_ERROR_NO_MEMORY;
    b->direction = AAUDIO_DIRECTION_OUTPUT;
    b->sharing = AAUDIO_SHARING_MODE_SHARED;
    b->format = AAUDIO_FORMAT_UNSPECIFIED;
    b->perf_mode = AAUDIO_PERFORMANCE_MODE_NONE;
    b->session_id = AAUDIO_SESSION_ID_NONE;
    b->capacity_frames = AAUDIO_UNSPECIFIED;
    *builder = b;
    return AAUDIO_OK;
}

void AAudioStreamBuilder_delete(AAudioStreamBuilder *builder) { free(builder); }

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *b, int32_t device_id)
{ if (b) b->device_id = device_id; }
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t direction)
{ if (b) b->direction = direction; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t mode)
{ if (b) b->sharing = mode; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t sample_rate)
{ if (b) b->sample_rate = sample_rate; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t channels)
{ if (b) b->channels = channels; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t format)
{ if (b) b->format = format; }
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *b, int32_t frames)
{ if (b) b->capacity_frames = frames; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t mode)
{ if (b) b->perf_mode = mode; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t usage)
{ if (b) b->usage = usage; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, aaudio_content_type_t type)
{ if (b) b->content_type = type; }
void AAudioStreamBuilder_setSessionId(AAudioStreamBuilder *b, int32_t session_id)
{ if (b) b->session_id = session_id; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, aaudio_data_callback_t cb, void *userData)
{ if (b) { b->data_cb = cb; b->data_user = userData; } }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, aaudio_error_callback_t cb, void *userData)
{ if (b) { b->error_cb = cb; b->error_user = userData; } }
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *b, int32_t frames)
{ if (b) b->frames_per_cb = frames; }

static int env_rate(void)
{
    const char *e = getenv("UA_AUDIO_RATE");
    int r = e && *e ? atoi(e) : 0;
    return r >= 8000 && r <= 192000 ? r : 48000;
}

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **stream)
{
    if (!stream) return AAUDIO_ERROR_NULL;
    *stream = NULL;
    if (!b) return AAUDIO_ERROR_NULL;
    if (b->direction == AAUDIO_DIRECTION_INPUT || b->direction != AAUDIO_DIRECTION_OUTPUT)
        return AAUDIO_ERROR_UNIMPLEMENTED;                        // output only
    aaudio_format_t format = b->format == AAUDIO_FORMAT_UNSPECIFIED ? AAUDIO_FORMAT_PCM_I16 : b->format;
    if (format != AAUDIO_FORMAT_PCM_I16 && format != AAUDIO_FORMAT_PCM_FLOAT) return AAUDIO_ERROR_INVALID_FORMAT;
    int32_t channels = b->channels > 0 ? b->channels : 2;
    if (channels < 1 || channels > 8) return AAUDIO_ERROR_INVALID_FORMAT;
    int32_t rate = b->sample_rate > 0 ? b->sample_rate : env_rate();
    if (rate < 8000 || rate > 192000) return AAUDIO_ERROR_INVALID_RATE;
    int32_t frames = b->frames_per_cb > 0 ? b->frames_per_cb : AAUDIO_DEFAULT_FRAMES;
    if (frames > 32768) return AAUDIO_ERROR_OUT_OF_RANGE;
    int32_t capacity = b->capacity_frames > 0 ? b->capacity_frames : 2 * frames;
    int nbuf = (int)((capacity + frames - 1) / frames);
    if (nbuf < 2) nbuf = 2;
    if (nbuf > 32) nbuf = 32;

    AAudioStream *s = calloc(1, sizeof *s);
    if (!s) return AAUDIO_ERROR_NO_MEMORY;
    if (pthread_mutex_init(&s->lock, NULL) != 0) { free(s); return AAUDIO_ERROR_NO_MEMORY; }
    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) { pthread_mutex_destroy(&s->lock); free(s); return AAUDIO_ERROR_NO_MEMORY; }
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    if (pthread_cond_init(&s->cond, &ca) != 0) {
        pthread_condattr_destroy(&ca); pthread_mutex_destroy(&s->lock); free(s); return AAUDIO_ERROR_NO_MEMORY;
    }
    pthread_condattr_destroy(&ca);

    s->state = AAUDIO_STREAM_STATE_OPEN;
    s->device_id = b->device_id;
    s->sharing = b->sharing;
    s->sample_rate = rate;
    s->channels = channels;
    s->format = format;
    s->perf_mode = b->perf_mode;
    s->session_id = b->session_id < 0 ? AAUDIO_SESSION_ID_NONE : b->session_id;
    s->usage = b->usage;
    s->content_type = b->content_type;
    s->frames_per_cb = frames;
    s->nbuf = nbuf;
    s->capacity_frames = s->buffer_frames = nbuf * frames;
    s->bytes_per_frame = channels * (format == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2);
    s->frame_bytes_total = frames * s->bytes_per_frame;
    s->data_cb = b->data_cb;
    s->data_user = b->data_user;
    s->error_cb = b->error_cb;
    s->error_user = b->error_user;

    aaudio_result_t r = stream_player_open(s);
    if (r != AAUDIO_OK) {
        SHIM_WARN("AAudio: no output stream (%d Hz, %d ch, fmt %d): no SDL/OpenSL output", rate, channels, (int)format);
        stream_player_destroy(s);
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return r;
    }
    SHIM_TRACE("AAudio: stream %p: %d Hz, %d ch, fmt %d, %d frames x %d buffers",
               (void *)s, rate, channels, (int)format, frames, nbuf);
    *stream = s;
    return AAUDIO_OK;
}

// ---- stream ---------------------------------------------------------------------------------
aaudio_result_t AAudioStream_close(AAudioStream *s)
{
    if (!s) return AAUDIO_ERROR_NULL;
    pthread_mutex_lock(&s->lock);
    if (s->closing) { pthread_mutex_unlock(&s->lock); return AAUDIO_ERROR_INVALID_STATE; }
    __atomic_store_n(&s->closing, 1, __ATOMIC_RELEASE);
    s->state = AAUDIO_STREAM_STATE_CLOSING;
    pthread_cond_broadcast(&s->cond);
    while (s->refs > 0) pthread_cond_wait(&s->cond, &s->lock);
    s->state = AAUDIO_STREAM_STATE_CLOSED;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (s->play) (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_STOPPED);
    stream_player_destroy(s);      // joins the output thread: no callback runs after this
    free(s->block);
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStart(AAudioStream *s)
{
    s = s_ref(s);
    if (!s) return AAUDIO_ERROR_NULL;
    aaudio_result_t r = AAUDIO_OK;
    pthread_mutex_lock(&s->lock);
    if (s->closing || !s->play) r = AAUDIO_ERROR_INVALID_STATE;
    else s->state = AAUDIO_STREAM_STATE_STARTING;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    if (r == AAUDIO_OK && (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS)
        r = AAUDIO_ERROR_INVALID_STATE;
    if (r == AAUDIO_OK) set_state(s, AAUDIO_STREAM_STATE_STARTED);
    else { set_state(s, AAUDIO_STREAM_STATE_OPEN); report_error(s, r); }
    s_unref(s);
    return r;
}

aaudio_result_t AAudioStream_requestStop(AAudioStream *s)
{
    s = s_ref(s);
    if (!s) return AAUDIO_ERROR_NULL;
    aaudio_result_t r = AAUDIO_OK;
    set_state(s, AAUDIO_STREAM_STATE_STOPPING);
    if (!s->play || (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PAUSED) != SL_RESULT_SUCCESS)
        r = AAUDIO_ERROR_INVALID_STATE;
    set_state(s, r == AAUDIO_OK ? AAUDIO_STREAM_STATE_STOPPED : AAUDIO_STREAM_STATE_STARTED);
    s_unref(s);
    return r;
}

aaudio_stream_state_t AAudioStream_getState(AAudioStream *s)
{
    if (!s) return AAUDIO_STREAM_STATE_UNKNOWN;
    pthread_mutex_lock(&s->lock);
    aaudio_stream_state_t st = s->state;
    pthread_mutex_unlock(&s->lock);
    return st;
}

aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s, aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t *nextState, int64_t timeoutNanos)
{
    if (nextState) *nextState = AAUDIO_STREAM_STATE_UNKNOWN;
    if (!s) return AAUDIO_ERROR_NULL;
    if (!nextState) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    aaudio_result_t r = AAUDIO_OK;
    pthread_mutex_lock(&s->lock);
    if (s->state == inputState) {
        if (timeoutNanos < 0) {
            while (s->state == inputState) pthread_cond_wait(&s->cond, &s->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += (time_t)(timeoutNanos / 1000000000);
            ts.tv_nsec += (long)(timeoutNanos % 1000000000);
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            while (s->state == inputState) {
                if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) != 0) break;
            }
        }
        if (s->state == inputState) r = AAUDIO_ERROR_TIMEOUT;
    }
    *nextState = s->state;
    pthread_mutex_unlock(&s->lock);
    return r;
}

int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s ? s->sample_rate : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s ? s->channels : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getDeviceId(AAudioStream *s) { return s ? s->device_id : AAUDIO_ERROR_NULL; }
aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream *s) { return s ? s->sharing : AAUDIO_ERROR_NULL; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s) { return s ? s->perf_mode : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return s ? s->frames_per_cb : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getFramesPerDataCallback(AAudioStream *s) { return s ? s->frames_per_cb : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { return s ? s->buffer_frames : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { return s ? s->capacity_frames : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getXRunCount(AAudioStream *s) { return s ? __atomic_load_n(&s->xruns, __ATOMIC_RELAXED) : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getHardwareSampleRate(AAudioStream *s) { return s ? s->sample_rate : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getHardwareChannelCount(AAudioStream *s) { return s ? s->channels : AAUDIO_ERROR_NULL; }
aaudio_format_t AAudioStream_getHardwareFormat(AAudioStream *s) { return s ? s->format : AAUDIO_ERROR_NULL; }
int32_t AAudioStream_getSessionId(AAudioStream *s) { return s ? s->session_id : AAUDIO_ERROR_NULL; }
bool AAudioStream_isMMapUsed(AAudioStream *s) { (void)s; return false; }

int32_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t frames)
{
    s = s_ref(s);
    if (!s) return AAUDIO_ERROR_NULL;
    if (frames < s->frames_per_cb) frames = s->frames_per_cb;
    if (frames > s->capacity_frames) frames = s->capacity_frames;
    s->buffer_frames = frames;      // the queue's buffer count is fixed at open
    s_unref(s);
    return frames;
}

// ---- texts ----------------------------------------------------------------------------------
const char *AAudio_convertResultToText(aaudio_result_t code)
{
    switch (code) {
    case AAUDIO_OK: return "AAUDIO_OK";
    case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case AAUDIO_ERROR_INTERNAL: return "AAUDIO_ERROR_INTERNAL";
    case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
    case AAUDIO_ERROR_INVALID_HANDLE: return "AAUDIO_ERROR_INVALID_HANDLE";
    case AAUDIO_ERROR_UNIMPLEMENTED: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
    case AAUDIO_ERROR_NO_FREE_HANDLES: return "AAUDIO_ERROR_NO_FREE_HANDLES";
    case AAUDIO_ERROR_NO_MEMORY: return "AAUDIO_ERROR_NO_MEMORY";
    case AAUDIO_ERROR_NULL: return "AAUDIO_ERROR_NULL";
    case AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
    case AAUDIO_ERROR_WOULD_BLOCK: return "AAUDIO_ERROR_WOULD_BLOCK";
    case AAUDIO_ERROR_INVALID_FORMAT: return "AAUDIO_ERROR_INVALID_FORMAT";
    case AAUDIO_ERROR_OUT_OF_RANGE: return "AAUDIO_ERROR_OUT_OF_RANGE";
    case AAUDIO_ERROR_NO_SERVICE: return "AAUDIO_ERROR_NO_SERVICE";
    case AAUDIO_ERROR_INVALID_RATE: return "AAUDIO_ERROR_INVALID_RATE";
    default: return "AAUDIO_ERROR_UNKNOWN";
    }
}

const char *AAudio_convertStreamStateToText(aaudio_stream_state_t state)
{
    switch (state) {
    case AAUDIO_STREAM_STATE_UNINITIALIZED: return "AAUDIO_STREAM_STATE_UNINITIALIZED";
    case AAUDIO_STREAM_STATE_UNKNOWN: return "AAUDIO_STREAM_STATE_UNKNOWN";
    case AAUDIO_STREAM_STATE_OPEN: return "AAUDIO_STREAM_STATE_OPEN";
    case AAUDIO_STREAM_STATE_STARTING: return "AAUDIO_STREAM_STATE_STARTING";
    case AAUDIO_STREAM_STATE_STARTED: return "AAUDIO_STREAM_STATE_STARTED";
    case AAUDIO_STREAM_STATE_PAUSING: return "AAUDIO_STREAM_STATE_PAUSING";
    case AAUDIO_STREAM_STATE_PAUSED: return "AAUDIO_STREAM_STATE_PAUSED";
    case AAUDIO_STREAM_STATE_FLUSHING: return "AAUDIO_STREAM_STATE_FLUSHING";
    case AAUDIO_STREAM_STATE_FLUSHED: return "AAUDIO_STREAM_STATE_FLUSHED";
    case AAUDIO_STREAM_STATE_STOPPING: return "AAUDIO_STREAM_STATE_STOPPING";
    case AAUDIO_STREAM_STATE_STOPPED: return "AAUDIO_STREAM_STATE_STOPPED";
    case AAUDIO_STREAM_STATE_CLOSING: return "AAUDIO_STREAM_STATE_CLOSING";
    case AAUDIO_STREAM_STATE_CLOSED: return "AAUDIO_STREAM_STATE_CLOSED";
    case AAUDIO_STREAM_STATE_DISCONNECTED: return "AAUDIO_STREAM_STATE_DISCONNECTED";
    default: return "AAUDIO_STREAM_STATE_UNKNOWN";
    }
}
