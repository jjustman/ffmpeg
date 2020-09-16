 /*
 * AC4 decoder wrapper for libDAA
 *
 *	note: WARNING: Disabled dash_demuxer because not all dependencies are satisfied: libxml2
 *	apt-get install pkg-config libxml2-dev
 *


 *   old 2020-09-12 - ./configure --enable-libxml2     --enable-demuxer=dash --enable-muxer=dash --enable-libdaa-ac4 --enable-decoder=libdaa_ac4 --extra-cflags="-I`pwd`/../libdaa/include" --extra-libs="-L`pwd`/../libdaa/lib/android_armv8_float_neon/"

baseline:
./configure --enable-debug=3 --optflags="-O0" --disable-stripping --disable-optimizations  --enable-gpl --enable-nonfree  --enable-libx264   --enable-encoder=libx264  --enable-libxml2     --enable-demuxer=dash --enable-muxer=dash --enable-libdaa-ac4 --enable-decoder=libdaa_ac4 --extra-cflags="-I`pwd`/../libdaa/include" --extra-libs="-L`pwd`/../libdaa/lib/android_armv8_float_neon/"

 */

#include <dlb_decode_api.h>
#include <dlb_buffer.h>

#include <dlb_decode_err.h>
#include <dlb_decode_user.h>


#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"

//jjustman-2020-09-12 - from dlb_dec_fio.h


/**** Module Defines ****/
#define LFE2_IDX_UNDEF      (-1)
#define ROUTING_LFE         5
#define ROUTING_X1          6
#define OUTPUTMAP_TSpr      13
#define OUTPUTMAP_LFE2      14
#define OUTPUTMAP_LFE       15
#define NO_CHAN             16
#define MAXNUMOUTPUTCONFIGS 17
#define MAXNUMSTRINGS       2
#define MAXOUTPUTCHARS      16
#define MAXOUTPUTSTRCHARS   1024
#define FIO_MAXFCHANS       5
#define TEXTBUFLEN          256

/**** Module Macros ****/
#define CHANBIT(A)                      (((unsigned)0x8000) >> (A))
#define CHAN_EXISTS_IN_OUTMAP(A,B)      ((CHANBIT(A)) & (B))
#define IS_OUTMAP_PAIR(A)               ((CHANBIT(A)) & ((unsigned short)0x0674))

#define EVOFF_SYNC_BITS     0x58013802


typedef struct DAAAC4DecContext {
    const AVClass *class;
   // HANDLE_AACDECODER handle;

    uint8_t *decoder_buffer;
    int decoder_buffer_size;

   //from dlb_dec_player.h


      /* Output buffers */
      dlb_buffer outputbuf;                       /* output buffers for PCM data */


      /* Text buffer for error handling */
      char textbuf[TEXTBUFLEN];                   /* text buffer */
      BFD_BUFDESC_TEXT textbfd;                   /* Used for error code conversions to text */

      /* handlers */
      void *p_dechdl;                             /* subroutine memory handle */
      dlb_decode_query_info_op query_info_op;     /* data returned from query_info */
      dlb_decode_query_mem_op  query_mem_op;      /* data returned from query_memory */
      dlb_decode_io_params     io_params;         /* input and output parameter at each frame*/


      int outnchans;                                  /* Number of output channels        */
      int chanrouting[FIO_MAXPCMOUTCHANS];            /* Channel routing array            */
      int channelmask;                                /* Wave output channel mask         */
      int outchanconfig;                              /* Output channel configuration     */
      int quitonerr;                                  /* if 0, continue on process err    */
      unsigned int mdatflags;                         /* Metadata output flags            */

      /* demuxers related parameters */
      void                    *demuxer;

      int                     current_sample;

      dlb_decode_query_ip 	p_queryip;
      auto_config_para_t 	a_auto_conf_para[ MAX_AUTO_CONFIG_PARA_SIZE ];
      int 					auto_mode_num;
      unsigned int   		framecompletecount;
      unsigned int			total_frames_decoded;


      int numofaudioin;                               /* Number of audio stream inputs (1 or 2)  */

      int dap_onoff;                                  /* flag of audio post-processing is ON or OFF */
      int endpoint;                                   /* endpoint index */
      int virtualizer_onoff;                          /* flag of virtualizer is ON or OFF */
      int dialog_enhancement_gain;                    /* Dialog enhancement gain */
      int output_reference_level;                     /* Output reference level */
      int presentation_id;                            /* AC-4 only. Presentation index */
      int main_asso_pref;                             /* AC-4 only. Indicates the preferred mixing ratio between main and associated audio programs */

      int nsamples;                               /* number of samples to write out */
      int samplerate;                             /* local variable in main: sample rate to write out */

      unsigned long timescale;

      int output_delay;

} DAAAC4DecContext;

//#define OFFSET(x) offsetof(FDKAACDecContext, x)
#define AD AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption daa_ac4_dec_options[] = {

#ifdef _LIBDAA_OPTIONS_IMPLEMENTED_
    { "conceal", "Error concealment method", OFFSET(conceal_method), AV_OPT_TYPE_INT, { .i64 = CONCEAL_METHOD_NOISE_SUBSTITUTION }, CONCEAL_METHOD_SPECTRAL_MUTING, CONCEAL_METHOD_NB - 1, AD, "conceal" },
    { "spectral", "Spectral muting",      0, AV_OPT_TYPE_CONST, { .i64 = CONCEAL_METHOD_SPECTRAL_MUTING },      INT_MIN, INT_MAX, AD, "conceal" },
    { "noise",    "Noise Substitution",   0, AV_OPT_TYPE_CONST, { .i64 = CONCEAL_METHOD_NOISE_SUBSTITUTION },   INT_MIN, INT_MAX, AD, "conceal" },
    { "energy",   "Energy Interpolation", 0, AV_OPT_TYPE_CONST, { .i64 = CONCEAL_METHOD_ENERGY_INTERPOLATION }, INT_MIN, INT_MAX, AD, "conceal" },
    { "drc_boost", "Dynamic Range Control: boost, where [0] is none and [127] is max boost",
                     OFFSET(drc_boost),      AV_OPT_TYPE_INT,   { .i64 = -1 }, -1, 127, AD, NULL    },
    { "drc_cut",   "Dynamic Range Control: attenuation factor, where [0] is none and [127] is max compression",
                     OFFSET(drc_cut),        AV_OPT_TYPE_INT,   { .i64 = -1 }, -1, 127, AD, NULL    },
    { "drc_level", "Dynamic Range Control: reference level, quantized to 0.25dB steps where [0] is 0dB and [127] is -31.75dB, -1 for auto, and -2 for disabled",
                     OFFSET(drc_level),      AV_OPT_TYPE_INT,   { .i64 = -1},  -2, 127, AD, NULL    },
    { "drc_heavy", "Dynamic Range Control: heavy compression, where [1] is on (RF mode) and [0] is off",
                     OFFSET(drc_heavy),      AV_OPT_TYPE_INT,   { .i64 = -1},  -1, 1,   AD, NULL    },
#if FDKDEC_VER_AT_LEAST(2, 5) // 2.5.10
    { "level_limit", "Signal level limiting",
                     OFFSET(level_limit),    AV_OPT_TYPE_BOOL,  { .i64 = -1 }, -1, 1, AD },
#endif
#if FDKDEC_VER_AT_LEAST(3, 0) // 3.0.0
    { "drc_effect","Dynamic Range Control: effect type, where e.g. [0] is none and [6] is general",
                     OFFSET(drc_effect),     AV_OPT_TYPE_INT,   { .i64 = -1},  -1, 8,   AD, NULL    },
#endif
#endif

    { NULL }
};

/* channel configuration masks */
const int wav_channelmask[] =
{
    0x0,
    (WAV_CH_BIT_CNTR),                                                                                              /* 1 = (1/0) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT),                                                                            /* 2 = (2/0) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR),                                                          /* 3 = (3/0) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_MSUR),                                                          /* 4 = (2/1) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_MSUR),                                        /* 5 = (3/1) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR),                                        /* 6 = (2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR),                      /* 7 = (3/2) */

    /* Since WAVEFORMATEXTENSIBLE and Dolby Digital Plus define a different set of extension channels,
     * revert to the WAVEFORMATEX when user requests output config > 5.1. The channel mask values below (> 5.1)
     * are only used in deriving the channel routing array. They are never written to a WAVEFORMATEXTENSIBLE
     * header because they do not correctly define the location of the extension channels.
     */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_EXT1),                                        /* 8 = (3/0/1) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1),                      /* 9 = (2/2/1) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1),    /* 10 = (3/2/1) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1),    /* 11 = (3/2/1) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),                      /* 12 = (3/0/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 13 = (2/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 14 = (2/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 15 = (2/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 16 = (2/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 17 = (3/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 18 = (3/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 19 = (3/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 20 = (3/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 21 = (3/2/2) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 22 = (3/2/2) */
    0x0,                                                                                                                              /* 23 = Reserved (not yet implemented) */
    0x0,                                                                                                                              /* 24 = Reserved (not yet implemented) */
    0x0,                                                                                                                              /* 25 = Reserved (not yet implemented) */
    0x0,                                                                                                                              /* 26 = Reserved (not yet implemented) */
    0x0,                                                                                                                              /* 27 = Reserved (not yet implemented) */
    (WAV_CH_BIT_LEFT | WAV_CH_BIT_RGHT | WAV_CH_BIT_CNTR | WAV_CH_BIT_LSUR | WAV_CH_BIT_RSUR | WAV_CH_BIT_EXT1 | WAV_CH_BIT_EXT2),    /* 28 = (3/2/2) */
};


//
///**** Module Structures ****/
//typedef struct
//{
//    char p_infilename[FIO_MAXFILENAMELEN];          /* Input file name */
//    char p_cfginfilename[FIO_MAXFILENAMELEN];       /* auto run config file name, it is NULL if in mannual mode */
//    char p_pcmoutfilename[FIO_MAXFILENAMELEN];      /* Output WAV file name */
//    char p_mdatoutfilename[FIO_MAXFILENAMELEN];     /* Metadata output file name */
//
//    int waveoutput;                                 /* Microsoft Wave output files (*.wav) or not (*.pcm) */
//    int pcmwordtype;                                /* Output PCM file word type        */
//    int outnchans;                                  /* Number of output channels        */
//    int chanrouting[FIO_MAXPCMOUTCHANS];            /* Channel routing array            */
//    int channelmask;                                /* Wave output channel mask         */
//    int outchanconfig;                              /* Output channel configuration     */
//    int quitonerr;                                  /* if 0, continue on process err    */
//    unsigned int mdatflags;                         /* Metadata output flags            */
//
//    int numofaudioin;                               /* Number of audio stream inputs (1 or 2)  */
//    IN_FILE_TYPE infiletype;                        /* Input file type */
//    int program_index;                               /* Program index, only used when input file is mp2ts format */
//    int main_index;                                 /* Index of audio main stream in .mp4/.ts file */
//
//    int dap_onoff;                                  /* flag of audio post-processing is ON or OFF */
//    int endpoint;                                   /* endpoint index */
//    int virtualizer_onoff;                          /* flag of virtualizer is ON or OFF */
//    int dialog_enhancement_gain;                    /* Dialog enhancement gain */
//    int output_reference_level;                     /* Output reference level */
//    int presentation_id;                            /* AC-4 only. Presentation index */
//    int main_asso_pref;                             /* AC-4 only. Indicates the preferred mixing ratio between main and associated audio programs */
//
//    unsigned long timescale;
//} EXECPARAMS;


static const AVClass daa_ac4_dec_class = {
    .class_name = "libdaa-ac4 decoder",
    .item_name  = av_default_item_name,
    .option     = daa_ac4_dec_options,
    .version    = LIBAVUTIL_VERSION_INT,
};


static int updateparamsbyoutput(DAAAC4DecContext *s)
{
    int err = ERR_NO_ERROR;
    int nchannel;
    assert( s );


    assert(&s->io_params);
    s->nsamples             = s->io_params.output_samples_num;
    s->samplerate           = s->io_params.output_sample_rate;
    s->outnchans = s->io_params.output_channels_num;

    nchannel = s->outnchans;

    if ( (nchannel != 2) && (nchannel != 6) && (nchannel != 8) )
    {
        err = ERR_PROCESS_OUTPUT;

        goto error;
    }
    if ( s->outnchans >= 2 )   /* stereo */
    {
        s->chanrouting[0] = WAV_CH_LEFT;
        s->chanrouting[1] = WAV_CH_RGHT;
        s->outchanconfig  = 2;
    }
    if ( s->outnchans >= 6 )   /* 5.1 channel */
    {
        s->chanrouting[2] = WAV_CH_CNTR;
        s->chanrouting[3] = WAV_CH_LFE;
        s->chanrouting[4] = WAV_CH_LSUR;
        s->chanrouting[5] = WAV_CH_RSUR;
        s->outchanconfig  = 7;
    }
    if ( s->outnchans >= 8 )   /* 7.1 channel */
    {
        s->chanrouting[6] = WAV_CH_EXT1;
        s->chanrouting[7] = WAV_CH_EXT2;
        s->outchanconfig  = 21;
    }

    s->channelmask = wav_channelmask[s->outchanconfig];

error:
    return err;
}


/*****************************************************************
* set_auto_config_param:
*****************************************************************/
static int set_auto_config_param(DAAAC4DecContext* s, auto_config_para_t* p_config_data, int* auto_mode_num, int framecompletecount )
{
    int err, num;


    if ( p_config_data == NULL )
    {
        printf("ERROR: auto configuration data is invalid.\n");
        return ERR_INVALID_PARAM;
    }

    if ( auto_mode_num == NULL )
    {
        printf("ERROR: auto_mode_num is NULL.\n");
        return ERR_INVALID_PARAM;
    }

    if ( *auto_mode_num < 0 )
    {
        printf("ERROR: auto mode num is a negative number.\n");
        return ERR_INVALID_PARAM;
    }

    num = *auto_mode_num;

    /* check if there is any parameter to change at the frame N */
    if ( p_config_data[num].framecomplete != framecompletecount )
    {
        return ERR_NO_ERROR;
    }

    if (p_config_data[num].dap_onoff != -1)   /* Turn on/off audio processing */
    {
        printf("Change audio_processing to %s at frame %d\n",
               p_config_data[num].dap_onoff == 1 ? "ON" : "OFF",
               p_config_data[num].framecomplete);

        s->dap_onoff = p_config_data[num].dap_onoff;

        err = dlb_decode_setparam(
                  s->p_dechdl,
                  DLB_DECODE_CTL_DAP_ONOFF_ID,
                  &(s->dap_onoff),
                  sizeof(s->dap_onoff) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change audio_processing to %s at frame %d, error code is %d\n",
                   p_config_data[num].dap_onoff == 1 ? "ON" : "OFF",
                   p_config_data[num].framecomplete,
                   err);
        }
    }

    if ( p_config_data[num].endpoint != 0 )   /* Switch the endpoint*/
    {
        printf("Change endpoint to %d at frame %d\n",
               p_config_data[num].endpoint,
               p_config_data[num].framecomplete);

        s->endpoint = p_config_data[num].endpoint;

        err = dlb_decode_setparam(
                  s->p_dechdl,
                  DLB_DECODE_CTL_ENDPOINT_ID,
                  &(s->endpoint),
                  sizeof(s->endpoint) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change endpoint to %d at frame %d, error code is %d\n",
                   p_config_data[num].endpoint,
                   p_config_data[num].framecomplete,
                   err);
        }
    }

    if ( p_config_data[num].virtualizer_onoff != -1 )   /* Turn on/off virtualizer */
    {
        printf("Change virtualizer to %s at frame %d\n",
               p_config_data[num].virtualizer_onoff == 1 ? "ON" : "OFF",
               p_config_data[num].framecomplete);

        s->virtualizer_onoff = p_config_data[num].virtualizer_onoff;

        err = dlb_decode_setparam(
                  s->p_dechdl,
                  DLB_DECODE_CTL_VIRTUALIZER_ID,
                  &(s->virtualizer_onoff),
                  sizeof(s->virtualizer_onoff) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change virtualizer_onoff to %d at frame %d, error code is %d\n",
                   p_config_data[num].virtualizer_onoff,
                   p_config_data[num].framecomplete,
                   err);
        }
    }

    if (p_config_data[num].dialog_enhancement_gain != -1)   /* Set dialogue enhancement level */
    {
        printf("Change dialogue enhancement level to %d at frame %d\n",
               p_config_data[num].dialog_enhancement_gain,
               p_config_data[num].framecomplete);

        s->dialog_enhancement_gain = p_config_data[num].dialog_enhancement_gain;

        err = dlb_decode_setparam(
                  s->p_dechdl,
                  DLB_DECODE_CTL_DIALOG_ENHANCEMENT_ID,
                  &(s->dialog_enhancement_gain),
                  sizeof(s->dialog_enhancement_gain) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change dialogue enhancement level to %d at frame %d, error code is %d\n",
                   p_config_data[num].dialog_enhancement_gain,
                   p_config_data[num].framecomplete,
                   err);
        }
    }

    if (p_config_data[num].output_reference_level!= -24) /* Set output reference level */
    {
        /* Set output reference level  */
        printf("Change output reference level to %d at frame %d\n",
               p_config_data[num].output_reference_level,
               p_config_data[num].framecomplete);

        s->output_reference_level = p_config_data[num].output_reference_level;

        err = dlb_decode_setparam(
                                   s->p_dechdl,
                                   DLB_DECODE_CTL_OUTPUT_REFERENCE_LEVEL_ID,
                                   &(s->output_reference_level),
                                   sizeof(s->output_reference_level) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change output_reference_level to %d at frame %d, error code is %d\n",
                   p_config_data[num].output_reference_level,
                   p_config_data[num].framecomplete,
                   err);
        }
    }


    if (p_config_data[num].presentation_id != -1)   /* Set presentation id */
    {
        printf("Change presentation id to %d at frame %d\n",
               p_config_data[num].presentation_id,
               p_config_data[num].framecomplete);

        s->presentation_id = p_config_data[num].presentation_id;

        err = dlb_decode_setparam(
                                   s->p_dechdl,
                                   DLB_DECODE_CTL_PRESENTATION_ID,
                                   &(s->presentation_id),
                                   sizeof(s->presentation_id) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change presentation id to %d at frame %d, error code is %d\n",
                   p_config_data[num].presentation_id,
                   p_config_data[num].framecomplete,
                   err);
        }
    }

    if (p_config_data[num].main_asso_pref != -33) /* Set mixing preference */
    {
        /* Set main/associated stream preference */
        printf("Change main_asso_pref to %d at frame %d\n",
               p_config_data[num].main_asso_pref,
               p_config_data[num].framecomplete);

        s->main_asso_pref = p_config_data[num].main_asso_pref;

        err = dlb_decode_setparam(
                                   s->p_dechdl,
                                   DLB_DECODE_CTL_MAIN_ASSO_PREF_ID,
                                   &(s->main_asso_pref),
                                   sizeof(s->main_asso_pref) );

        if ( err != ERR_NO_ERROR )
        {
            printf("Failed to change main_asso_pref to %d at frame %d, error code is %d\n",
                   p_config_data[num].main_asso_pref,
                   p_config_data[num].framecomplete,
                   err);
        }
    }

    *auto_mode_num += 1;

    return ERR_NO_ERROR;
}


static int get_stream_info(AVCodecContext *avctx)
{
	DAAAC4DecContext *s   = avctx->priv_data;
    //CStreamInfo *info     = aacDecoder_GetStreamInfo(s->handle);

    set_auto_config_param(s, s->a_auto_conf_para, &s->auto_mode_num, (int)s->framecompletecount);

    updateparamsbyoutput(s);

    int channel_counts[0x24] = { 0 };
    int i, ch_error       = 0;
    uint64_t ch_layout    = 0;


    if (s->samplerate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Stream info not initialized\n");
        return AVERROR_UNKNOWN;
    }
    avctx->sample_rate = s->samplerate;

    //todo - check data_type - p_pcmchbfds->data_type;
    int sample_depth = 0;
    switch (s->outputbuf.data_type) {
    	case DLB_BUFFER_SHORT_16:
    		sample_depth = 2;
    		break;
    	case DLB_BUFFER_LONG_32:
			sample_depth = sizeof(long); //is this right?
			break;
    	default:
            av_log(avctx, AV_LOG_ERROR, "Unknown outputbuf.datatype of: %d\n", s->outputbuf.data_type);

    }

    avctx->frame_size  = s->nsamples; //s->outnchans * s->nsamples * sample_depth;
    av_log(avctx, AV_LOG_TRACE, "get_stream_info: avctx->frame_size: %d, sample_rate: %d, from s->outnchans: %d (s->outputbuf.nchannel: %d), s->nsamples: %d, sample_depth: %d\n",
    		avctx->frame_size,
			avctx->sample_rate,
			s->outnchans,
			s->outputbuf.nchannel,
			s->nsamples,
			sample_depth
    		);

//
//    for (i = 0; i < info->numChannels; i++) {
//        AUDIO_CHANNEL_TYPE ctype = info->pChannelType[i];
//        if (ctype <= ACT_NONE || ctype >= FF_ARRAY_ELEMS(channel_counts)) {
//            av_log(avctx, AV_LOG_WARNING, "unknown channel type\n");
//            break;
//        }
//        channel_counts[ctype]++;
//    }
//    av_log(avctx, AV_LOG_DEBUG,
//           "%d channels - front:%d side:%d back:%d lfe:%d top:%d\n",
//           info->numChannels,
//           channel_counts[ACT_FRONT], channel_counts[ACT_SIDE],
//           channel_counts[ACT_BACK],  channel_counts[ACT_LFE],
//           channel_counts[ACT_FRONT_TOP] + channel_counts[ACT_SIDE_TOP] +
//           channel_counts[ACT_BACK_TOP]  + channel_counts[ACT_TOP]);
//
//    switch (channel_counts[ACT_FRONT]) {
//    case 4:
//        ch_layout |= AV_CH_LAYOUT_STEREO | AV_CH_FRONT_LEFT_OF_CENTER |
//                     AV_CH_FRONT_RIGHT_OF_CENTER;
//        break;
//    case 3:
//        ch_layout |= AV_CH_LAYOUT_STEREO | AV_CH_FRONT_CENTER;
//        break;
//    case 2:
//        ch_layout |= AV_CH_LAYOUT_STEREO;
//        break;
//    case 1:
//        ch_layout |= AV_CH_FRONT_CENTER;
//        break;
//    default:
//        av_log(avctx, AV_LOG_WARNING,
//               "unsupported number of front channels: %d\n",
//               channel_counts[ACT_FRONT]);
//        ch_error = 1;
//        break;
//    }
//    if (channel_counts[ACT_SIDE] > 0) {
//        if (channel_counts[ACT_SIDE] == 2) {
//            ch_layout |= AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT;
//        } else {
//            av_log(avctx, AV_LOG_WARNING,
//                   "unsupported number of side channels: %d\n",
//                   channel_counts[ACT_SIDE]);
//            ch_error = 1;
//        }
//    }
//    if (channel_counts[ACT_BACK] > 0) {
//        switch (channel_counts[ACT_BACK]) {
//        case 3:
//            ch_layout |= AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT | AV_CH_BACK_CENTER;
//            break;
//        case 2:
//            ch_layout |= AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT;
//            break;
//        case 1:
//            ch_layout |= AV_CH_BACK_CENTER;
//            break;
//        default:
//            av_log(avctx, AV_LOG_WARNING,
//                   "unsupported number of back channels: %d\n",
//                   channel_counts[ACT_BACK]);
//            ch_error = 1;
//            break;
//        }
//    }
//    if (channel_counts[ACT_LFE] > 0) {
//        if (channel_counts[ACT_LFE] == 1) {
//            ch_layout |= AV_CH_LOW_FREQUENCY;
//        } else {
//            av_log(avctx, AV_LOG_WARNING,
//                   "unsupported number of LFE channels: %d\n",
//                   channel_counts[ACT_LFE]);
//            ch_error = 1;
//        }
//    }
//    if (!ch_error &&
//        av_get_channel_layout_nb_channels(ch_layout) != info->numChannels) {
//        av_log(avctx, AV_LOG_WARNING, "unsupported channel configuration\n");
//        ch_error = 1;
//    }
//    if (ch_error)
//        avctx->channel_layout = 0;
//    else
    ch_layout = AV_CH_LAYOUT_STEREO;

    int channel_layout_nb = av_get_channel_layout_nb_channels(ch_layout);
	av_log(avctx, AV_LOG_TRACE, "get_stream_info: channel layout: %d, channel_layout_nb: %d\n", ch_layout, channel_layout_nb);

    avctx->channel_layout = ch_layout;

    avctx->channels = s->outputbuf.nchannel;

    return 0;
}

static av_cold int daa_ac4_decode_close(AVCodecContext *avctx)
{
	DAAAC4DecContext *s = avctx->priv_data;

	/* Free DEC_EXEC Memory */
	if (s->p_dechdl)
	{
		/*************************************/
		/* Step 8: Call dlb_decode_close() */
		/*************************************/
		/* close subroutine memory           */
		dlb_decode_close(s->p_dechdl);
		/* free subroutine memory */
		av_free(s->p_dechdl);
		s->p_dechdl = NULL;
	}

	/* Free PCM buffers */
	if (s->decoder_buffer)
	{
		av_free(s->decoder_buffer);
		s->decoder_buffer = NULL;
	}

	/* Free data buffers */
	if (s->outputbuf.ppdata)
	{
		av_free(s->outputbuf.ppdata);
	}


	av_free(s->decoder_buffer);

    return 0;
}


/*****************************************************************\
* daa_ac4_displaybanner: display version, copyright, and configuration info
\*****************************************************************/
static int daa_ac4_displaybanner(
    const dlb_decode_query_ip *p_inparams,              /* input  */
    const dlb_decode_query_info_op  *p_decparams)       /* input  */
{
    /* check input arguments */
    assert(p_inparams);
    assert(p_decparams);

    /* display version info */
    printf("\n*** Dolby Decoder Player Version %s ***", p_decparams->daa_version);

    if (p_inparams->input_bitstream_format == DLB_DECODE_INPUT_FORMAT_DDP)
    {
       /* display DDP decoder configuration info */
        printf("\nAtmos decoder & audio processing.\n");
        printf("Dolby Digital Plus decoder version %s \n", p_decparams->core_decoder_version);
        printf("Dolby audio processing version %s\n\n", p_decparams->dap_version);
    }
    else if (p_inparams->input_bitstream_format == DLB_DECODE_INPUT_FORMAT_AC4)
    {
       /* display AC-4 decoder configuration info */
        printf("\nDolby AC-4 decoder version %s\n\n", p_decparams->core_decoder_version);
    }
    else
    {
        printf("\nUnsupported input stream format.\n");
        return ERR_UNSUPPORTED_PARAM;
    }


    return (ERR_NO_ERROR);
}

// #define JJ_AC4_USE_RAW_FRAME
static av_cold int daa_ac4_decode_init(AVCodecContext *avctx)
{
    DAAAC4DecContext *s = avctx->priv_data;
    int err;

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;


    /* initialize queryip depending on the input file*/
     memset( &s->p_queryip, 0, sizeof(dlb_decode_query_ip) );
     s->p_queryip.output_datatype              =  DLB_BUFFER_SHORT_16;
     s->p_queryip.input_bitstream_format       =  DLB_DECODE_INPUT_FORMAT_AC4;
#ifdef JJ_AC4_USE_RAW_FRAME
     s->p_queryip.input_bitstream_type         =  DLB_DECODE_INPUT_TYPE_AC4_RAW_FRAME;
#else
     s->p_queryip.input_bitstream_type         =  DLB_DECODE_INPUT_TYPE_AC4_SIMPLE_TRANSPORT;

#endif
     s->p_queryip.timescale                    =  48000; //by default..?

     /****************************************/
     /* Step 1: Call dlb_decode_query_info()*/
     /* Call to this API is optional as this */
     /* provides version information         */
     /****************************************/

     /* query the library to find out the version */
     err = dlb_decode_query_info(&s->p_queryip, &(s->query_info_op) );
     if (err) { return AVERROR_UNKNOWN; }

     /* display banner info (regardless of verbose mode) */
     err = daa_ac4_displaybanner(&s->p_queryip, &(s->query_info_op));
     if (err) { return AVERROR_UNKNOWN; }

     /*********************************************/
     /* Step 2: Call dlb_decode_query_memory() */
     /********************************************/
     /* initialize query input parameters to zero */
     err = dlb_decode_query_memory( &s->p_queryip, &(s->query_mem_op) );
     if (err) { return AVERROR_UNKNOWN; }

     /* Allocate output buffers */
//     p_outbuf = (char *) calloc(1, s->query_mem_op.output_buffer_size);
//     if (!p_outbuf)
//     {
//         err = ERR_MEMORY;
//         goto cleanup;
//     }

     //allocate pcm buffer
     s->decoder_buffer_size = s->query_mem_op.output_buffer_size;
     s->decoder_buffer = av_malloc(s->decoder_buffer_size);
     if (!s->decoder_buffer)
         return AVERROR(ENOMEM);

     /* allocate memory for subroutine and take care of alignment */
     s->p_dechdl =(char *)calloc(1, s->query_mem_op.decoder_size);
     if (!s->p_dechdl) {
         return AVERROR(ENOMEM);
     }

     /***************************************/
     /* Step 3: Call dlb_decode_open()    */
     /***************************************/
     err = dlb_decode_open( &s->p_queryip, s->p_dechdl );
     if (err)
     {
    	 av_log(avctx, AV_LOG_ERROR, "dlb_decode_open failed with query_ip: %p, and dechdl: %p\n", s->p_queryip, s->p_dechdl);

         return AVERROR_UNKNOWN;
     }

     /******************************************/
     /* Step 4: Call dlb_decode_setparam()   */
     /******************************************/
     /* If dlb_decode_setparam() is NOT called, the default value will be used in DAA library, which are  */
     /*     AUDIO PROCESSING:        ON */
     /*     ENDPOINT:                Headphone */
     /*     VIRTUALIZER:             ON  */
     /*     DIALOG ENHANCEMENT:      6 */
     /*     OUTPUT REFERENCE LEVEL:  -14 */
     /*     PRESENTATION:            0xffff */
     /*     MAIN/ASSOCIATED LEVEL:   -32 */

     /* dlb_decode_setparam() is not called in this function */
//     err = set_decode_parameters( s->p_dechdl, &(s->execparams));
//     if ( err != ERR_NO_ERROR )
//     {
//         goto cleanup;
//     }

     /* initialize textbfd, which is used for error code conversions to text */

     /* initialize the text buffer descriptor */
         s->textbfd.p_buf = s->textbuf;
         s->textbfd.nbufchars = TEXTBUFLEN;


     /* Initialize buffers */
     s->io_params.pcm_output_buf = &(s->outputbuf);

     s->outputbuf.ppdata = (void **) malloc(sizeof(void *) * DLB_DECODE_MAX_PCMOUT_CHANNELS);
     if (!(s->outputbuf.ppdata))
     {
         return AVERROR(ENOMEM);
     }
     memset(s->outputbuf.ppdata, 0, sizeof(void *) * DLB_DECODE_MAX_PCMOUT_CHANNELS);

     /******************************************/
     /* Step 5: Set output buffer pointer   */
     /******************************************/
     /* !!!!!! This can be done before each decode() calling */

     s->outputbuf.ppdata[0] = s->decoder_buffer;

     if (avctx->request_channel_layout > 0 &&
             avctx->request_channel_layout != AV_CH_LAYOUT_NATIVE) {
    	 int downmix_channels = -1;

    	 switch (avctx->request_channel_layout) {
             case AV_CH_LAYOUT_STEREO:
             case AV_CH_LAYOUT_STEREO_DOWNMIX:
                 downmix_channels = 2;
                 break;
             case AV_CH_LAYOUT_MONO:
                 downmix_channels = 1;
                 break;
             default:
                 av_log(avctx, AV_LOG_WARNING, "Invalid request_channel_layout\n");
                 break;
             }

    		av_log(avctx, AV_LOG_INFO, "daa_ac4_decode_init: downmix_channels is: %d", downmix_channels);

//             if (downmix_channels != -1) {
//                 if (aacDecoder_SetParam(s->handle, AAC_PCM_MAX_OUTPUT_CHANNELS,
//                                         downmix_channels) != AAC_DEC_OK) {
//                    av_log(avctx, AV_LOG_WARNING, "Unable to set output channels in the decoder\n");
//                 } else {
//                    s->anc_buffer = av_malloc(DMX_ANC_BUFFSIZE);
//                    if (!s->anc_buffer) {
//                        av_log(avctx, AV_LOG_ERROR, "Unable to allocate ancillary buffer for the decoder\n");
//                        return AVERROR(ENOMEM);
//                    }
//                    if (aacDecoder_AncDataInit(s->handle, s->anc_buffer, DMX_ANC_BUFFSIZE)) {
//                        av_log(avctx, AV_LOG_ERROR, "Unable to register downmix ancillary buffer in the decoder\n");
//                        return AVERROR_UNKNOWN;
//                    }
//                 }
//             }
         }

//
//    s->handle = aacDecoder_Open(avctx->extradata_size ? TT_MP4_RAW : TT_MP4_ADTS, 1);
//    if (!s->handle) {
//        av_log(avctx, AV_LOG_ERROR, "Error opening decoder\n");
//        return AVERROR_UNKNOWN;
//    }
//
//    if (avctx->extradata_size) {
//        if ((err = aacDecoder_ConfigRaw(s->handle, &avctx->extradata,
//                                        &avctx->extradata_size)) != AAC_DEC_OK) {
//            av_log(avctx, AV_LOG_ERROR, "Unable to set extradata\n");
//            return AVERROR_INVALIDDATA;
//        }
//    }
//
//    if ((err = aacDecoder_SetParam(s->handle, AAC_CONCEAL_METHOD,
//                                   s->conceal_method)) != AAC_DEC_OK) {
//        av_log(avctx, AV_LOG_ERROR, "Unable to set error concealment method\n");
//        return AVERROR_UNKNOWN;
//    }
//
//    if (avctx->request_channel_layout > 0 &&
//        avctx->request_channel_layout != AV_CH_LAYOUT_NATIVE) {
//        int downmix_channels = -1;
//
//        switch (avctx->request_channel_layout) {
//        case AV_CH_LAYOUT_STEREO:
//        case AV_CH_LAYOUT_STEREO_DOWNMIX:
//            downmix_channels = 2;
//            break;
//        case AV_CH_LAYOUT_MONO:
//            downmix_channels = 1;
//            break;
//        default:
//            av_log(avctx, AV_LOG_WARNING, "Invalid request_channel_layout\n");
//            break;
//        }
//
//        if (downmix_channels != -1) {
//            if (aacDecoder_SetParam(s->handle, AAC_PCM_MAX_OUTPUT_CHANNELS,
//                                    downmix_channels) != AAC_DEC_OK) {
//               av_log(avctx, AV_LOG_WARNING, "Unable to set output channels in the decoder\n");
//            } else {
//               s->anc_buffer = av_malloc(DMX_ANC_BUFFSIZE);
//               if (!s->anc_buffer) {
//                   av_log(avctx, AV_LOG_ERROR, "Unable to allocate ancillary buffer for the decoder\n");
//                   return AVERROR(ENOMEM);
//               }
//               if (aacDecoder_AncDataInit(s->handle, s->anc_buffer, DMX_ANC_BUFFSIZE)) {
//                   av_log(avctx, AV_LOG_ERROR, "Unable to register downmix ancillary buffer in the decoder\n");
//                   return AVERROR_UNKNOWN;
//               }
//            }
//        }
//    }
//
//    if (s->drc_boost != -1) {
//        if (aacDecoder_SetParam(s->handle, AAC_DRC_BOOST_FACTOR, s->drc_boost) != AAC_DEC_OK) {
//            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC boost factor in the decoder\n");
//            return AVERROR_UNKNOWN;
//        }
//    }
//
//    if (s->drc_cut != -1) {
//        if (aacDecoder_SetParam(s->handle, AAC_DRC_ATTENUATION_FACTOR, s->drc_cut) != AAC_DEC_OK) {
//            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC attenuation factor in the decoder\n");
//            return AVERROR_UNKNOWN;
//        }
//    }
//
//    if (s->drc_level != -1) {
//        // This option defaults to -1, i.e. not calling
//        // aacDecoder_SetParam(AAC_DRC_REFERENCE_LEVEL) at all, which defaults
//        // to the level from DRC metadata, if available. The user can set
//        // -drc_level -2, which calls aacDecoder_SetParam(
//        // AAC_DRC_REFERENCE_LEVEL) with a negative value, which then
//        // explicitly disables the feature.
//        if (aacDecoder_SetParam(s->handle, AAC_DRC_REFERENCE_LEVEL, s->drc_level) != AAC_DEC_OK) {
//            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC reference level in the decoder\n");
//            return AVERROR_UNKNOWN;
//        }
//    }
//
//    if (s->drc_heavy != -1) {
//        if (aacDecoder_SetParam(s->handle, AAC_DRC_HEAVY_COMPRESSION, s->drc_heavy) != AAC_DEC_OK) {
//            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC heavy compression in the decoder\n");
//            return AVERROR_UNKNOWN;
//        }
//    }
//
//#if FDKDEC_VER_AT_LEAST(2, 5) // 2.5.10
//    // Setting this parameter to -1 enables the auto behaviour in the library.
//    if (aacDecoder_SetParam(s->handle, AAC_PCM_LIMITER_ENABLE, s->level_limit) != AAC_DEC_OK) {
//        av_log(avctx, AV_LOG_ERROR, "Unable to set in signal level limiting in the decoder\n");
//        return AVERROR_UNKNOWN;
//    }
//#endif
//
//#if FDKDEC_VER_AT_LEAST(3, 0) // 3.0.0
//    if (s->drc_effect != -1) {
//        if (aacDecoder_SetParam(s->handle, AAC_UNIDRC_SET_EFFECT, s->drc_effect) != AAC_DEC_OK) {
//            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC effect type in the decoder\n");
//            return AVERROR_UNKNOWN;
//        }
//    }
//#endif
//


    return 0;
}

static int daa_ac4_decode_frame(AVCodecContext *avctx, void *data,
                                int *got_frame_ptr, AVPacket *avpkt)
{
	DAAAC4DecContext *s = avctx->priv_data;
    AVFrame *frame = data;
    int ret;
    int valid = avpkt->size;

    unsigned int bytesconsumed;
    int framecomplete, err;
    signed long long input_timestamp = avpkt->pts;

#ifdef JJ_AC4_USE_RAW_FRAME
    /* add buffer bytes to subroutine */
    	err = dlb_decode_addbytes
    		  (s->p_dechdl
    		   , avpkt->data
    		   , avpkt->size
    		   , input_timestamp //0
    		   , &bytesconsumed
    		   , &framecomplete);

    	/* check error */
    	if (err)
    	{
    		printf("ERROR: dlb_decode_addbytes returned %d.\n", err);
    		ret = err;
    		goto end;
    	}
#else
	uint8_t* ac4_sync_word_and_data;
	uint32_t ac4_sync_word_and_data_size;

    unsigned char header[7];
	/* AC-4 sync word, no CRC*/
	header[0]  = 0xAC;
	header[1]  = 0x40;
	/* AC-4 sync size is 3 bytes */
	header[2]  = 0xFF;
	header[3]  = 0xFF;
	/* AC-4 frame size */
	header[4]  = (unsigned char)((avpkt->size >> 16) & 0xFF);
	header[5]  = (unsigned char)((avpkt->size >> 8) & 0xFF);
	header[6]  = (unsigned char)((avpkt->size >> 0) & 0xFF);

	ac4_sync_word_and_data_size = avpkt->size + 7;

	ac4_sync_word_and_data = av_malloc(ac4_sync_word_and_data_size);

	memcpy(ac4_sync_word_and_data, header, 7);
	memcpy(ac4_sync_word_and_data + 7, avpkt->data, avpkt->size);

	/* add buffer bytes to subroutine */
	err = dlb_decode_addbytes
		  (s->p_dechdl
		   , ac4_sync_word_and_data
		   , ac4_sync_word_and_data_size
		   , input_timestamp //0
		   , &bytesconsumed
		   , &framecomplete);


	/* check error */
	if (err)
	{
		printf("ERROR: dlb_decode_addbytes returned %d.\n", err);
		ret = err;
		goto end;
	}

	bytesconsumed -= 7; //remove sync frame header
#endif

	err = dlb_decode_process(
			  s->p_dechdl,
			  &(s->io_params));
		/* in case of error, just exit */
		if (err)
		{
			fprintf(stderr, "dlb_decode_process returned %d\n", err);
			ret = err;
			goto end;
		}

	s->framecompletecount++;


//
//    err = aacDecoder_Fill(s->handle, &avpkt->data, &avpkt->size, &valid);
//    if (err != AAC_DEC_OK) {
//        av_log(avctx, AV_LOG_ERROR, "aacDecoder_Fill() failed: %x\n", err);
//        return AVERROR_INVALIDDATA;
//    }
//
//    err = aacDecoder_DecodeFrame(s->handle, (INT_PCM *) s->decoder_buffer, s->decoder_buffer_size / sizeof(INT_PCM), 0);
//    if (err == AAC_DEC_NOT_ENOUGH_BITS) {
//        ret = avpkt->size - valid;
//        goto end;
//    }
//    if (err != AAC_DEC_OK) {
//        av_log(avctx, AV_LOG_ERROR,
//               "aacDecoder_DecodeFrame() failed: %x\n", err);
//        ret = AVERROR_UNKNOWN;
//        goto end;
//    }

    if ((ret = get_stream_info(avctx)) < 0)
        goto end;
    frame->nb_samples = avctx->frame_size;
    s->total_frames_decoded += frame->nb_samples;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        goto end;

    if (frame->pts != AV_NOPTS_VALUE)
        frame->pts -= av_rescale_q(s->output_delay,
                                   (AVRational){1, avctx->sample_rate},
                                   avctx->time_base);

	av_log(avctx, AV_LOG_TRACE, "daa_ac4_decode_frame: pts: %d, output_delay: %d, sample_rate: %d, time_base: %d",
			frame->pts,
			s->output_delay,
			avctx->sample_rate,
			avctx->time_base);


    int avctx_written_frame_size = avctx->channels * avctx->frame_size *
            av_get_bytes_per_sample(avctx->sample_fmt);
    memcpy(frame->extended_data[0], s->decoder_buffer, avctx_written_frame_size);

    *got_frame_ptr = 1;

    //ret = avpkt->size - bytesconsumed;
    ret = bytesconsumed;

    frame->pkt_duration = av_rescale_q(frame->nb_samples, (AVRational){1, avctx->sample_rate},
            avctx->time_base);

    av_log(avctx, AV_LOG_DEBUG, "daa_ac4_decode_frame: ret: %d, frame->nb_samples: %d, avctx->channels: %d, avctx->frame_size: %d, writing to avframe size: %d",
    			ret,
    			frame->nb_samples,
    			avctx->channels,
    			avctx->frame_size,
    			avctx_written_frame_size);


end:
    return ret;
}

static av_cold void daa_ac4_decode_flush(AVCodecContext *avctx)
{
//    FDKAACDecContext *s = avctx->priv_data;
//    AAC_DECODER_ERROR err;
//
//    if (!s->handle)
//        return;
//
//    if ((err = aacDecoder_SetParam(s->handle,
//                                   AAC_TPDEC_CLEAR_BUFFER, 1)) != AAC_DEC_OK)
	av_log(avctx, AV_LOG_WARNING, "failed to clear buffer when flushing - no impl!\n");
}

AVCodec ff_libdaa_ac4_decoder = {
    .name           = "libdaa_ac4",
    .long_name      = NULL_IF_CONFIG_SMALL("DAA AC4 Decoder Static"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_AC4,
    .priv_data_size = sizeof(DAAAC4DecContext),
    .init           = daa_ac4_decode_init,
    .decode         = daa_ac4_decode_frame,
    .close          = daa_ac4_decode_close,
    .flush          = daa_ac4_decode_flush,
    .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
    .priv_class     = &daa_ac4_dec_class,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .wrapper_name   = "libdaa",
};
