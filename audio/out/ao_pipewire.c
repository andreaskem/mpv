#include <limits.h>

#include <pipewire-0.3/pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include "ao.h"
#include "audio/format.h"
#include "config.h"
#include "internal.h"
#include "osdep/timer.h"

static void on_process(void *userdata);

struct priv {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
};

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

enum spa_audio_channel mp_pw_channel[] = {
    [MP_SPEAKER_ID_FL  ] = SPA_AUDIO_CHANNEL_FL  ,
    [MP_SPEAKER_ID_FR  ] = SPA_AUDIO_CHANNEL_FR  ,
    [MP_SPEAKER_ID_FC  ] = SPA_AUDIO_CHANNEL_FC  ,
    [MP_SPEAKER_ID_LFE ] = SPA_AUDIO_CHANNEL_LFE ,
    [MP_SPEAKER_ID_BL  ] = SPA_AUDIO_CHANNEL_RL  ,
    [MP_SPEAKER_ID_BR  ] = SPA_AUDIO_CHANNEL_RR  ,
    [MP_SPEAKER_ID_FLC ] = SPA_AUDIO_CHANNEL_FLC ,
    [MP_SPEAKER_ID_FRC ] = SPA_AUDIO_CHANNEL_FRC ,
    [MP_SPEAKER_ID_BC  ] = SPA_AUDIO_CHANNEL_RC  ,
    [MP_SPEAKER_ID_SL  ] = SPA_AUDIO_CHANNEL_SL  ,
    [MP_SPEAKER_ID_SR  ] = SPA_AUDIO_CHANNEL_SR  ,
    [MP_SPEAKER_ID_TC  ] = SPA_AUDIO_CHANNEL_TC  ,
    [MP_SPEAKER_ID_TFL ] = SPA_AUDIO_CHANNEL_TFL ,
    [MP_SPEAKER_ID_TFC ] = SPA_AUDIO_CHANNEL_TFC ,
    [MP_SPEAKER_ID_TFR ] = SPA_AUDIO_CHANNEL_TFR ,
    [MP_SPEAKER_ID_TBL ] = SPA_AUDIO_CHANNEL_TRL ,
    [MP_SPEAKER_ID_TBC ] = SPA_AUDIO_CHANNEL_TRC ,
    [MP_SPEAKER_ID_TBR ] = SPA_AUDIO_CHANNEL_TRR ,
    [MP_SPEAKER_ID_LFE2] = SPA_AUDIO_CHANNEL_LFE2,
    [MP_SPEAKER_ID_NA  ] = SPA_AUDIO_CHANNEL_NA  ,
};

struct pipewire_format {
    enum af_format mp_format;
    enum spa_audio_format pw_format;
    size_t stride;
};

static const struct pipewire_format mp_pw_formats[] = {
    {AF_FORMAT_U8     , SPA_AUDIO_FORMAT_U8  , 1},
    {AF_FORMAT_S16    , SPA_AUDIO_FORMAT_S16 , 2},
    {AF_FORMAT_S32    , SPA_AUDIO_FORMAT_S32 , 4},

    {AF_FORMAT_FLOAT  , SPA_AUDIO_FORMAT_F32 , 4},
    {AF_FORMAT_DOUBLE , SPA_AUDIO_FORMAT_F64 , 8},

    {AF_FORMAT_U8P    , SPA_AUDIO_FORMAT_U8P , 1},
    {AF_FORMAT_S16P   , SPA_AUDIO_FORMAT_S16P, 2},
    {AF_FORMAT_S32P   , SPA_AUDIO_FORMAT_S32P, 4},

    {AF_FORMAT_FLOATP , SPA_AUDIO_FORMAT_F32P, 4},
    {AF_FORMAT_DOUBLEP, SPA_AUDIO_FORMAT_F64P, 8},

    {0},
};

static void on_process(void *userdata)
{
    struct ao *ao = userdata;
    struct priv *p = ao->priv;

    struct pw_buffer *b;
    struct spa_buffer *buf;
    void * data_pointer[16];
    int nframes = 0;

    pw_thread_loop_lock(p->loop);
    if ((b = pw_stream_dequeue_buffer(p->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }
    buf = b->buffer;

    unsigned int maxbuf = UINT_MAX;
    size_t buffer_cnt = af_fmt_is_planar(ao->format) ? ao->channels.num : 1;
    for (int i = 0; i < buffer_cnt; ++i) {
        if ((data_pointer[i] = buf->datas[i].data) == NULL)
            return;
        maxbuf = buf->datas[i].maxsize < maxbuf ? buf->datas[i].maxsize : maxbuf;
    }

    nframes = maxbuf / ao->sstride / 2;
    int64_t end_time = mp_time_us();

    struct pw_time time = {0};
    pw_stream_get_time(p->stream, &time);
    if (time.rate.denom == 0)
        time.rate.denom = ao->samplerate;

    nframes = ao_read_data(ao, data_pointer, nframes,
                           end_time + (nframes + time.queued / ao->sstride / ao->channels.num + time.delay)
                           * 1e6 / time.rate.denom);
    b->size = 0;
    for (int i = 0; i < buffer_cnt; ++i) {
        buf->datas[i].chunk->offset = 0;
        buf->datas[i].chunk->stride = ao->sstride;
        buf->datas[i].chunk->size = nframes * ao->sstride;
        b->size += buf->datas[i].chunk->size;
    }

    pw_stream_queue_buffer(p->stream, b);
    pw_thread_loop_unlock(p->loop);
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];

    pw_init(NULL, NULL);

    p->loop = pw_thread_loop_new("ao-pipewire", NULL);
    p->stream = pw_stream_new_simple(
                    pw_thread_loop_get_loop(p->loop),
                    "audio-src",
                    pw_properties_new(
                        PW_KEY_MEDIA_TYPE, "Audio",
                        PW_KEY_MEDIA_CATEGORY, "Playback",
                        PW_KEY_MEDIA_ROLE, "Music",
                        NULL),
                    &stream_events,
                    ao);

    const struct pipewire_format * desired_format = NULL;
    for (int n = 0; mp_pw_formats[n].mp_format; n++) {
        if (mp_pw_formats[n].mp_format == ao->format) {
            desired_format = &mp_pw_formats[n];
            break;
        }
    }

    ao->sstride = desired_format->stride;
    if (!af_fmt_is_planar(ao->format))
        ao->sstride *= ao->channels.num;

    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
                                         .format = desired_format->pw_format,
                                         .channels = ao->channels.num,
                                         .rate = ao->samplerate);

    struct mp_chmap_sel sel = {0};
    mp_chmap_sel_add_waveext_def(&sel);
    ao_chmap_sel_adjust(ao, &sel, &ao->channels);
    ao_chmap_sel_get_def(ao, &sel, &ao->channels, ao->channels.num);

    if (mp_chmap_equals(&ao->channels,
                        &(const struct mp_chmap)MP_CHMAP_INIT_MONO)) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        for (int n = 0; n < ao->channels.num; n++) {
            info.position[n] = mp_pw_channel[ao->channels.speaker[n]];
        }
    }

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(p->stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_stream_set_active(p->stream, true);

    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (p->loop)
        pw_thread_loop_stop(p->loop);
    if (p->stream)
        pw_stream_destroy(p->stream);
    p->stream = NULL;
    if (p->loop)
        pw_thread_loop_destroy(p->loop);
    p->loop = NULL;
    pw_deinit();
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;
    pw_thread_loop_stop(p->loop);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;
    pw_thread_loop_start(p->loop);
}

const struct ao_driver audio_out_pipewire = {
    .description = "PipeWire audio output",
    .name      = "pipewire",

    .init      = init,
    .uninit    = uninit,
    .reset     = reset,
    .start     = start,

    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv)
    {
        .loop = NULL,
        .stream = NULL,
    },
};
