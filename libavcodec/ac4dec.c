/*
 * AC4 decoder
 * 2020-09-12
 *
 */

/**
 * @file
 * AC4 decoder
 * @author Jason Justman (jjustman@onemediallc.com)
 *
 */


#include "libavutil/float_dsp.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"
#include "get_bits.h"
#include "fft.h"
#include "mdct15.h"
#include "lpc.h"
#include "kbdwin.h"
#include "sinewin.h"

#include "aac.h"
//#include "aactab.h"
#include "ac4dectab.h"
//#include "adts_header.h"
//#include "cbrt_data.h"
//#include "sbr.h"
//#include "aacsbr.h"
#include "mpeg4audio.h"
#include "profiles.h"
#include "libavutil/intfloat.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if ARCH_ARM
#   include "arm/aac.h"
#elif ARCH_MIPS
#   include "mips/aacdec_mips.h"
#endif


static int read_stream_mux_config(struct LATMContext *latmctx,
                                  GetBitContext *gb)
{
    int ret, audio_mux_version = get_bits(gb, 1);

    latmctx->audio_mux_version_A = 0;
    if (audio_mux_version)
        latmctx->audio_mux_version_A = get_bits(gb, 1);

    if (!latmctx->audio_mux_version_A) {

        if (audio_mux_version)
            latm_get_value(gb);                 // taraFullness

        skip_bits(gb, 1);                       // allStreamSameTimeFraming
        skip_bits(gb, 6);                       // numSubFrames
        // numPrograms
        if (get_bits(gb, 4)) {                  // numPrograms
            avpriv_request_sample(latmctx->aac_ctx.avctx, "Multiple programs");
            return AVERROR_PATCHWELCOME;
        }

        // for each program (which there is only one in DVB)

        // for each layer (which there is only one in DVB)
        if (get_bits(gb, 3)) {                   // numLayer
            avpriv_request_sample(latmctx->aac_ctx.avctx, "Multiple layers");
            return AVERROR_PATCHWELCOME;
        }

        // for all but first stream: use_same_config = get_bits(gb, 1);
        if (!audio_mux_version) {
            if ((ret = latm_decode_audio_specific_config(latmctx, gb, 0)) < 0)
                return ret;
        } else {
            int ascLen = latm_get_value(gb);
            if ((ret = latm_decode_audio_specific_config(latmctx, gb, ascLen)) < 0)
                return ret;
        }

        latmctx->frame_length_type = get_bits(gb, 3);
        switch (latmctx->frame_length_type) {
        case 0:
            skip_bits(gb, 8);       // latmBufferFullness
            break;
        case 1:
            latmctx->frame_length = get_bits(gb, 9);
            break;
        case 3:
        case 4:
        case 5:
            skip_bits(gb, 6);       // CELP frame length table index
            break;
        case 6:
        case 7:
            skip_bits(gb, 1);       // HVXC frame length table index
            break;
        }

        if (get_bits(gb, 1)) {                  // other data
            if (audio_mux_version) {
                latm_get_value(gb);             // other_data_bits
            } else {
                int esc;
                do {
                    if (get_bits_left(gb) < 9)
                        return AVERROR_INVALIDDATA;
                    esc = get_bits(gb, 1);
                    skip_bits(gb, 8);
                } while (esc);
            }
        }

        if (get_bits(gb, 1))                     // crc present
            skip_bits(gb, 8);                    // config_crc
    }

    return 0;
}

static int read_payload_length_info(struct LATMContext *ctx, GetBitContext *gb)
{
    uint8_t tmp;

    if (ctx->frame_length_type == 0) {
        int mux_slot_length = 0;
        do {
            if (get_bits_left(gb) < 8)
                return AVERROR_INVALIDDATA;
            tmp = get_bits(gb, 8);
            mux_slot_length += tmp;
        } while (tmp == 255);
        return mux_slot_length;
    } else if (ctx->frame_length_type == 1) {
        return ctx->frame_length;
    } else if (ctx->frame_length_type == 3 ||
               ctx->frame_length_type == 5 ||
               ctx->frame_length_type == 7) {
        skip_bits(gb, 2);          // mux_slot_length_coded
    }
    return 0;
}

static int read_audio_mux_element(struct LATMContext *latmctx,
                                  GetBitContext *gb)
{
    int err;
    uint8_t use_same_mux = get_bits(gb, 1);
    if (!use_same_mux) {
        if ((err = read_stream_mux_config(latmctx, gb)) < 0)
            return err;
    } else if (!latmctx->aac_ctx.avctx->extradata) {
        av_log(latmctx->aac_ctx.avctx, AV_LOG_DEBUG,
               "no decoder config found\n");
        return 1;
    }
    if (latmctx->audio_mux_version_A == 0) {
        int mux_slot_length_bytes = read_payload_length_info(latmctx, gb);
        if (mux_slot_length_bytes < 0 || mux_slot_length_bytes * 8LL > get_bits_left(gb)) {
            av_log(latmctx->aac_ctx.avctx, AV_LOG_ERROR, "incomplete frame\n");
            return AVERROR_INVALIDDATA;
        } else if (mux_slot_length_bytes * 8 + 256 < get_bits_left(gb)) {
            av_log(latmctx->aac_ctx.avctx, AV_LOG_ERROR,
                   "frame length mismatch %d << %d\n",
                   mux_slot_length_bytes * 8, get_bits_left(gb));
            return AVERROR_INVALIDDATA;
        }
    }
    return 0;
}


static int latm_decode_frame(AVCodecContext *avctx, void *out,
                             int *got_frame_ptr, AVPacket *avpkt)
{
    struct LATMContext *latmctx = avctx->priv_data;
    int                 muxlength, err;
    GetBitContext       gb;

    if ((err = init_get_bits8(&gb, avpkt->data, avpkt->size)) < 0)
        return err;

    // check for LOAS sync word
    if (get_bits(&gb, 11) != LOAS_SYNC_WORD)
        return AVERROR_INVALIDDATA;

    muxlength = get_bits(&gb, 13) + 3;
    // not enough data, the parser should have sorted this out
    if (muxlength > avpkt->size)
        return AVERROR_INVALIDDATA;

    if ((err = read_audio_mux_element(latmctx, &gb)))
        return (err < 0) ? err : avpkt->size;

    if (!latmctx->initialized) {
        if (!avctx->extradata) {
            *got_frame_ptr = 0;
            return avpkt->size;
        } else {
            push_output_configuration(&latmctx->aac_ctx);
            if ((err = decode_audio_specific_config(
                    &latmctx->aac_ctx, avctx, &latmctx->aac_ctx.oc[1].m4ac,
                    avctx->extradata, avctx->extradata_size*8LL, 1)) < 0) {
                pop_output_configuration(&latmctx->aac_ctx);
                return err;
            }
            latmctx->initialized = 1;
        }
    }

    if (show_bits(&gb, 12) == 0xfff) {
        av_log(latmctx->aac_ctx.avctx, AV_LOG_ERROR,
               "ADTS header detected, probably as result of configuration "
               "misparsing\n");
        return AVERROR_INVALIDDATA;
    }

    switch (latmctx->aac_ctx.oc[1].m4ac.object_type) {
    case AOT_ER_AAC_LC:
    case AOT_ER_AAC_LTP:
    case AOT_ER_AAC_LD:
    case AOT_ER_AAC_ELD:
        err = aac_decode_er_frame(avctx, out, got_frame_ptr, &gb);
        break;
    default:
        err = aac_decode_frame_int(avctx, out, got_frame_ptr, &gb, avpkt);
    }
    if (err < 0)
        return err;

    return muxlength;
}

//    .priv_data_size  = sizeof(AACContext),


AVCodec ff_ac4_decoder = {
    .name            = "ac4",
    .long_name       = NULL_IF_CONFIG_SMALL("AC-4 (Dolby AC-4 Decoder static libDAA)"),
    .type            = AVMEDIA_TYPE_AUDIO,
    .id              = AV_CODEC_ID_AC4,
    .init            = ac4_decode_init,
    .close           = ac4_decode_close,
    .decode          = ac4_decode_frame,
    .sample_fmts     = (const enum AVSampleFormat[]) {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE
    },
    .capabilities    = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .caps_internal   = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .channel_layouts = aac_channel_layout,
    .flush = flush,
    .priv_class      = &aac_decoder_class,
    .profiles        = NULL_IF_CONFIG_SMALL(ff_aac_profiles),
};

