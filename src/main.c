/* 
 * Copyright (C) 2013 nu774
 * For conditions of distribution and use, see copyright notice in COPYING
 */
#if HAVE_CONFIG_H
#  include "config.h"
#endif
#if HAVE_STDINT_H
#  include <stdint.h>
#endif
#if HAVE_INTTYPES_H
#  include <inttypes.h>
#elif defined(_MSC_VER)
#  define SCNd64 "I64d"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SIGACTION
#include <signal.h>
#endif
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "compat.h"
#include "wav_reader.h"
#include "aacenc.h"
#include "m4af.h"
#include "progress.h"
#include "version.h"
#include "metadata.h"

#ifdef USE_LIBSNDFILE
#include "sndfile.h"
#endif
#ifdef USE_LIBSAMPLERATE
#include "samplerate.h"
#endif

#define PROGNAME "fdkaac"

static volatile int g_interrupted = 0;

#if HAVE_SIGACTION
static void signal_handler(int signum)
{
    g_interrupted = 1;
}
static void handle_signals(void)
{
    int i, sigs[] = { SIGINT, SIGHUP, SIGTERM };
    for (i = 0; i < sizeof(sigs)/sizeof(sigs[0]); ++i) {
        struct sigaction sa = { 0 };
        sa.sa_handler = signal_handler;
        sa.sa_flags |= SA_RESTART;
        sigaction(sigs[i], &sa, 0);
    }
}
#elif defined(_WIN32)
static BOOL WINAPI signal_handler(DWORD type)
{
    g_interrupted = 1;
    return TRUE;
}

static void handle_signals(void)
{
    SetConsoleCtrlHandler(signal_handler, TRUE);
}
#else
static void handle_signals(void)
{
}
#endif

static
int read_callback(void *cookie, void *data, uint32_t size)
{
    size_t rc = fread(data, 1, size, (FILE*)cookie);
    return ferror((FILE*)cookie) ? -1 : (int)rc;
}

static
int write_callback(void *cookie, const void *data, uint32_t size)
{
    size_t rc = fwrite(data, 1, size, (FILE*)cookie);
    return ferror((FILE*)cookie) ? -1 : (int)rc;
}

static
int seek_callback(void *cookie, int64_t off, int whence)
{
    return fseeko((FILE*)cookie, off, whence);
}

static
int64_t tell_callback(void *cookie)
{
    return ftello((FILE*)cookie);
}

static
void usage(void)
{
    printf(
PROGNAME " %s\n"
"Usage: " PROGNAME " [options] input_file\n"
"Options:\n"
" -h, --help                    Print this help message\n"
" -p, --profile <n>             Profile (audio object type)\n"
"                                 2: MPEG-4 AAC LC (default)\n"
"                                 5: MPEG-4 HE-AAC (SBR)\n"
"                                29: MPEG-4 HE-AAC v2 (SBR+PS)\n"
"                                23: MPEG-4 AAC LD\n"
"                                39: MPEG-4 AAC ELD\n"
"                               129: MPEG-2 AAC LC\n"
"                               132: MPEG-2 HE-AAC (SBR)\n"
"                               156: MPEG-2 HE-AAC v2 (SBR+PS)\n"
" -b, --bitrate <n>             Bitrate in bits per seconds (for CBR)\n"
" -m, --bitrate-mode <n>        Bitrate configuration\n"
"                                 0: CBR (default)\n"
"                                 1-5: VBR\n"
"                               (VBR mode is not officially supported, and\n"
"                                works only on a certain combination of\n"
"                                parameter settings, sample rate, and\n"
"                                channel configuration)\n"
" -w, --bandwidth <n>           Frequency bandwidth in Hz (AAC LC only)\n"
" -a, --afterburner <n>         Afterburner\n"
"                                 0: Off\n"
"                                 1: On(default)\n"
" -L, --lowdelay-sbr            Enable ELD-SBR (AAC ELD only)\n"
" -s, --sbr-signaling <n>       SBR signaling mode\n"
"                                 0: Implicit, backward compatible(default)\n"
"                                 1: Explicit SBR and implicit PS\n"
"                                 2: Explicit hierarchical signaling\n"
" -f, --transport-format <n>    Transport format\n"
"                                 0: RAW (default, muxed into M4A)\n"
"                                 1: ADIF\n"
"                                 2: ADTS\n"
"                                 6: LATM MCP=1\n"
"                                 7: LATM MCP=0\n"
"                                10: LOAS/LATM (LATM within LOAS)\n"
" -C, --adts-crc-check          Add CRC protection on ADTS header\n"
" -h, --header-period <n>       StreamMuxConfig/PCE repetition period in\n"
"                               transport layer\n"
"\n"
" -o <filename>                 Output filename\n"
" -G, --gapless-mode <n>        Encoder delay signaling for gapless playback\n"
"                                 0: iTunSMPB (default)\n"
"                                 1: ISO standard (edts + sgpd)\n"
"                                 2: Both\n"
" --ignorelength                Ignore length of WAV header\n"
" -S, --silent                  Don't print progress messages\n"
" --moov-before-mdat            Place moov box before mdat box on m4a output\n"
#ifdef USE_LIBSAMPLERATE
" --resample <rate>             Resample audio to specified rate before encoding\n"
#endif
"\n"
"Options for raw (headerless) input:\n"
" -R, --raw                     Treat input as raw (by default WAV is\n"
"                               assumed)\n"
" --raw-channels <n>            Number of channels (default: 2)\n"
" --raw-rate     <n>            Sample rate (default: 44100)\n"
" --raw-format   <spec>         Sample format, default is \"S16L\".\n"
"                               Spec is as follows:\n"
"                                1st char: S(igned)|U(nsigned)|F(loat)\n"
"                                2nd part: bits per channel\n"
"                                Last char: L(ittle)|B(ig)\n"
"                               Last char can be omitted, in which case L is\n"
"                               assumed. Spec is case insensitive, therefore\n"
"                               \"u16b\" is same as \"U16B\".\n"
"\n"
"Tagging options:\n"
" --title <string>\n"
" --artist <string>\n"
" --album <string>\n"
" --genre <string>\n"
" --date <string>\n"
" --composer <string>\n"
" --grouping <string>\n"
" --comment <string>\n"
" --album-artist <string>\n"
" --track <number[/total]>\n"
" --disk <number[/total]>\n"
" --tempo <n>\n"
" --tag <fcc>:<value>          Set iTunes predefined tag with four char code.\n"
" --tag-from-file <fcc>:<filename>\n"
"                              Same as above, but value is read from file.\n"
" --long-tag <name>:<value>    Set arbitrary tag as iTunes custom metadata.\n"
" --tag-from-json <filename[?dot_notation]>\n"
"                              Read tags from JSON. By default, tags are\n"
"                              assumed to be direct children of the root\n"
"                              object(dictionary).\n"
"                              Optionally, position of the dictionary\n"
"                              that contains tags can be specified with\n"
"                              dotted notation.\n"
"                              Example:\n"
"                                --tag-from-json /path/to/json?format.tags\n"
    , fdkaac_version);
}

typedef struct aacenc_param_ex_t {
    AACENC_PARAMS

    char *input_filename;
    char *output_filename;
    unsigned gapless_mode;
    unsigned ignore_length;
    int silent;
    int moov_before_mdat;

    unsigned int resample;

    int is_raw;
    unsigned raw_channels;
    unsigned raw_rate;
    const char *raw_format;

    aacenc_tag_param_t tags;

    char *json_filename;
} aacenc_param_ex_t;

static
int parse_options(int argc, char **argv, aacenc_param_ex_t *params)
{
    int ch;
    unsigned n;

#define OPT_MOOV_BEFORE_MDAT     M4AF_FOURCC('m','o','o','v')
#define OPT_RAW_CHANNELS         M4AF_FOURCC('r','c','h','n')
#define OPT_RAW_RATE             M4AF_FOURCC('r','r','a','t')
#define OPT_RAW_FORMAT           M4AF_FOURCC('r','f','m','t')
#define OPT_SHORT_TAG            M4AF_FOURCC('s','t','a','g')
#define OPT_SHORT_TAG_FILE       M4AF_FOURCC('s','t','g','f')
#define OPT_LONG_TAG             M4AF_FOURCC('l','t','a','g')
#define OPT_TAG_FROM_JSON        M4AF_FOURCC('t','f','j','s')

    static struct option long_options[] = {
        { "help",             no_argument,       0, 'h' },
        { "profile",          required_argument, 0, 'p' },
        { "bitrate",          required_argument, 0, 'b' },
        { "bitrate-mode",     required_argument, 0, 'm' },
        { "bandwidth",        required_argument, 0, 'w' },
        { "afterburner",      required_argument, 0, 'a' },
        { "lowdelay-sbr",     no_argument,       0, 'L' },
        { "sbr-signaling",    required_argument, 0, 's' },
        { "transport-format", required_argument, 0, 'f' },
        { "adts-crc-check",   no_argument,       0, 'C' },
        { "header-period",    required_argument, 0, 'P' },

        { "gapless-mode",     required_argument, 0, 'G' },
        { "ignorelength",     no_argument,       0, 'I' },
        { "silent",           no_argument,       0, 'S' },
        { "moov-before-mdat", no_argument,       0, OPT_MOOV_BEFORE_MDAT   },

#ifdef USE_LIBSAMPLERATE
        { "resample",         required_argument, 0, 'X' },
#endif

        { "raw",              no_argument,       0, 'R' },
        { "raw-channels",     required_argument, 0, OPT_RAW_CHANNELS       },
        { "raw-rate",         required_argument, 0, OPT_RAW_RATE           },
        { "raw-format",       required_argument, 0, OPT_RAW_FORMAT         },

        { "title",            required_argument, 0, M4AF_TAG_TITLE         },
        { "artist",           required_argument, 0, M4AF_TAG_ARTIST        },
        { "album",            required_argument, 0, M4AF_TAG_ALBUM         },
        { "genre",            required_argument, 0, M4AF_TAG_GENRE         },
        { "date",             required_argument, 0, M4AF_TAG_DATE          },
        { "composer",         required_argument, 0, M4AF_TAG_COMPOSER      },
        { "grouping",         required_argument, 0, M4AF_TAG_GROUPING      },
        { "comment",          required_argument, 0, M4AF_TAG_COMMENT       },
        { "album-artist",     required_argument, 0, M4AF_TAG_ALBUM_ARTIST  },
        { "track",            required_argument, 0, M4AF_TAG_TRACK         },
        { "disk",             required_argument, 0, M4AF_TAG_DISK          },
        { "tempo",            required_argument, 0, M4AF_TAG_TEMPO         },
        { "tag",              required_argument, 0, OPT_SHORT_TAG          },
        { "tag-from-file",    required_argument, 0, OPT_SHORT_TAG_FILE     },
        { "long-tag",         required_argument, 0, OPT_LONG_TAG           },
        { "tag-from-json",    required_argument, 0, OPT_TAG_FROM_JSON      },
        { 0,                  0,                 0, 0                      },
    };
    params->afterburner = 1;

    aacenc_getmainargs(&argc, &argv);
    while ((ch = getopt_long(argc, argv, "hp:b:m:w:a:Ls:f:CP:G:Io:SRX:",
                             long_options, 0)) != EOF) {
        switch (ch) {
        case 'h':
            return usage(), -1;
        case 'p':
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for profile\n");
                return -1;
            }
            params->profile = n;
            break;
        case 'b':
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for bitrate\n");
                return -1;
            }
            params->bitrate = n;
            break;
        case 'm':
            if (sscanf(optarg, "%u", &n) != 1 || n > 5) {
                fprintf(stderr, "invalid arg for bitrate-mode\n");
                return -1;
            }
            params->bitrate_mode = n;
            break;
        case 'w':
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for bandwidth\n");
                return -1;
            }
            params->bandwidth = n;
            break;
        case 'a':
            if (sscanf(optarg, "%u", &n) != 1 || n > 1) {
                fprintf(stderr, "invalid arg for afterburner\n");
                return -1;
            }
            params->afterburner = n;
            break;
        case 'L':
            params->lowdelay_sbr = 1;
            break;
        case 's':
            if (sscanf(optarg, "%u", &n) != 1 || n > 2) {
                fprintf(stderr, "invalid arg for sbr-signaling\n");
                return -1;
            }
            params->sbr_signaling = n;
            break;
        case 'f':
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for transport-format\n");
                return -1;
            }
            params->transport_format = n;
            break;
        case 'C':
            params->adts_crc_check = 1;
            break;
        case 'P':
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for header-period\n");
                return -1;
            }
            params->header_period = n;
            break;
        case 'o':
            params->output_filename = optarg;
            break;
        case 'G':
            if (sscanf(optarg, "%u", &n) != 1 || n > 2) {
                fprintf(stderr, "invalid arg for gapless-mode\n");
                return -1;
            }
            params->gapless_mode = n;
            break;
        case 'I':
            params->ignore_length = 1;
            break;
        case 'S':
            params->silent = 1;
            break;
        case OPT_MOOV_BEFORE_MDAT:
            params->moov_before_mdat = 1;
            break;
        case 'R':
            params->is_raw = 1;
            break;
        case 'X':
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for resample\n");
                return -1;
            }
            params->resample = n;
            break;
        case OPT_RAW_CHANNELS:
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for raw-channels\n");
                return -1;
            }
            params->raw_channels = n;
            break;
        case OPT_RAW_RATE:
            if (sscanf(optarg, "%u", &n) != 1) {
                fprintf(stderr, "invalid arg for raw-rate\n");
                return -1;
            }
            params->raw_rate = n;
            break;
        case OPT_RAW_FORMAT:
            params->raw_format = optarg;
            break;
        case M4AF_TAG_TITLE:
        case M4AF_TAG_ARTIST:
        case M4AF_TAG_ALBUM:
        case M4AF_TAG_GENRE:
        case M4AF_TAG_DATE:
        case M4AF_TAG_COMPOSER:
        case M4AF_TAG_GROUPING:
        case M4AF_TAG_COMMENT:
        case M4AF_TAG_ALBUM_ARTIST:
        case M4AF_TAG_TRACK:
        case M4AF_TAG_DISK:
        case M4AF_TAG_TEMPO:
            aacenc_param_add_itmf_entry(&params->tags, ch, 0, optarg,
                                        strlen(optarg), 0);
            break;
        case OPT_SHORT_TAG:
        case OPT_SHORT_TAG_FILE:
        case OPT_LONG_TAG:
            {
                char *val;
                size_t klen;
                unsigned fcc = M4AF_FOURCC('-','-','-','-');

                if ((val = strchr(optarg, ':')) == 0) {
                    fprintf(stderr, "invalid arg for tag\n");
                    return -1;
                }
                *val++ = '\0';
                if (ch == OPT_SHORT_TAG || ch == OPT_SHORT_TAG_FILE) {
                    /*
                     * take care of U+00A9(COPYRIGHT SIGN).
                     * 1) if length of fcc is 3, we prepend '\xa9'.
                     * 2) U+00A9 becomes "\xc2\xa9" in UTF-8. Therefore
                     *    we remove first '\xc2'.
                     */
                    if (optarg[0] == '\xc2')
                        ++optarg;
                    if ((klen = strlen(optarg))== 3)
                        fcc = 0xa9;
                    else if (klen != 4) {
                        fprintf(stderr, "invalid arg for tag\n");
                        return -1;
                    }
                    for (; *optarg; ++optarg)
                        fcc = ((fcc << 8) | (*optarg & 0xff));
                }
                aacenc_param_add_itmf_entry(&params->tags, fcc, optarg,
                                            val, strlen(val),
                                            ch == OPT_SHORT_TAG_FILE);
            }
            break;
        case OPT_TAG_FROM_JSON:
            params->json_filename = optarg;
            break;
        default:
            return usage(), -1;
        }
    }
    if (argc == optind)
        return usage(), -1;

    if (!params->bitrate && !params->bitrate_mode) {
        fprintf(stderr, "bitrate or bitrate-mode is mandatory\n");
        return -1;
    }
    if (params->output_filename && !strcmp(params->output_filename, "-") &&
        !params->transport_format) {
        fprintf(stderr, "stdout streaming is not available on M4A output\n");
        return -1;
    }
    if (params->bitrate && params->bitrate < 10000)
        params->bitrate *= 1000;

    if (params->is_raw) {
        if (!params->raw_channels)
            params->raw_channels = 2;
        if (!params->raw_rate)
            params->raw_rate = 44100;
        if (!params->raw_format)
            params->raw_format = "S16L";
    }
    params->input_filename = argv[optind];
    return 0;
};

static
int write_sample(FILE *ofp, m4af_ctx_t *m4af,
                 const void *data, uint32_t size, uint32_t duration)
{
    if (!m4af) {
        fwrite(data, 1, size, ofp);
        if (ferror(ofp)) {
            fprintf(stderr, "ERROR: fwrite(): %s\n", strerror(errno));
            return -1;
        }
    } else if (m4af_write_sample(m4af, 0, data, size, duration) < 0) {
        fprintf(stderr, "ERROR: failed to write m4a sample\n");
        return -1;
    }
    return 0;
}

#ifdef USE_LIBSNDFILE
static
int encode_sndfile(SNDFILE* snd, SF_INFO* info, pcm_sample_description_t* format,
           HANDLE_AACENCODER encoder,
           uint32_t frame_length, FILE *ofp, m4af_ctx_t *m4af,
           int show_progress)
{
#ifdef USE_LIBSAMPLERATE
    //short *resamplebuf = 0;
    SRC_STATE* srcstate = NULL;
    SRC_DATA srcdata;
    int srcerror = 0;
    float *ibuf = 0;
#else
    short *ibuf = 0;
#endif

    int16_t *pcmbuf = 0;
    uint32_t pcmsize = 0;
    uint8_t *obuf = 0;
    uint32_t olen;
    uint32_t osize = 0;
    int nread = 1;
    int consumed, written;
    int rc = -1;
    int frames_written = 0;
    aacenc_progress_t progress = { 0 };

    ibuf = malloc(frame_length * format->bytes_per_frame);
#ifdef USE_LIBSAMPLERATE
    if(format->sample_rate != info->samplerate) {
        /* set up resampler */
        srcstate = src_new(SRC_SINC_MEDIUM_QUALITY, info->channels, &srcerror);
        srcdata.src_ratio = format->sample_rate; srcdata.src_ratio /= info->samplerate;
        srcdata.data_in = ibuf;
        srcdata.data_out = malloc(frame_length * format->bytes_per_frame * srcdata.src_ratio);
        srcdata.end_of_input = 0;
    }

#endif
    aacenc_progress_init(&progress, info->frames, info->samplerate);
    do {
        if (g_interrupted)
            nread = 0;
        else if (nread) {
#ifdef USE_LIBSAMPLERATE
            if ((nread = sf_readf_float(snd, ibuf, frame_length)) < 0) {
#else
            if ((nread = sf_readf_short(snd, ibuf, frame_length)) < 0) {
#endif
                fprintf(stderr, "ERROR: read failed\n");
                goto END;
            } else if (nread > 0) {
                float* in_buf = ibuf;
#ifdef USE_LIBSAMPLERATE
                if(srcstate) {
                    /* run converstion */
                    srcdata.input_frames = nread;
                    srcdata.output_frames = frame_length;
                    if(srcerror = src_process(srcstate, &srcdata)) {
                        fprintf(stderr, "resample error: %s\n", src_strerror(srcerror));
                        goto END;
                    }
                    //printf("resampled %d samples to %d samples\n", srcdata.input_frames_used, srcdata.output_frames_gen);
                    in_buf = srcdata.data_out;
                    nread = srcdata.output_frames_gen;
                }
#endif
                if (pcm_convert_to_native_sint16(format, in_buf, nread,
                                                 &pcmbuf, &pcmsize) < 0) {
                    fprintf(stderr, "ERROR: unsupported sample format\n");
                    goto END;
                }
            }
            if (show_progress)
                aacenc_progress_update(&progress, sf_seek(snd, 0, SEEK_CUR),
                                       info->samplerate * 2);
        }
        written = nread;
        do {
            if ((consumed = aac_encode_frame(encoder, format, pcmbuf + (nread-written)*info->channels, written,
                                             &obuf, &olen, &osize)) < 0)
                goto END;
            //printf("nread: %d consumed: %d olen: %d\n", nread, consumed, olen);
            written -= (consumed / info->channels);
            if (olen > 0) {
                if (write_sample(ofp, m4af, obuf, olen, frame_length) < 0)
                    goto END;
                ++frames_written;
                //printf("wrote frame %d\n", frames_written);
            }
        } while (written > 0);
    } while (nread > 0 || olen > 0);

    if (show_progress)
        aacenc_progress_finish(&progress, sf_seek(snd, 0, SEEK_CUR));
    rc = frames_written;
END:
    if (ibuf) free(ibuf);
    if (pcmbuf) free(pcmbuf);
    if (obuf) free(obuf);
#ifdef USE_SAMPLERATE
    if (srcstate) {
        free(srcdata.data_out);
        src_delete(srcstate);
    }
#endif
    return rc;
}
#endif


static
int encode(wav_reader_t *wavf, HANDLE_AACENCODER encoder,
           uint32_t frame_length, FILE *ofp, m4af_ctx_t *m4af,
           int show_progress)
{
    uint8_t *ibuf = 0;
    int16_t *pcmbuf = 0;
    uint32_t pcmsize = 0;
    uint8_t *obuf = 0;
    uint32_t olen;
    uint32_t osize = 0;
    int nread = 1;
    int consumed;
    int rc = -1;
    int frames_written = 0;
    aacenc_progress_t progress = { 0 };
    const pcm_sample_description_t *format = wav_get_format(wavf);

    ibuf = malloc(frame_length * format->bytes_per_frame);
    aacenc_progress_init(&progress, wav_get_length(wavf), format->sample_rate);
    do {
        if (g_interrupted)
            nread = 0;
        else if (nread) {
            if ((nread = wav_read_frames(wavf, ibuf, frame_length)) < 0) {
                fprintf(stderr, "ERROR: read failed\n");
                goto END;
            } else if (nread > 0) {
                if (pcm_convert_to_native_sint16(format, ibuf, nread,
                                                 &pcmbuf, &pcmsize) < 0) {
                    fprintf(stderr, "ERROR: unsupported sample format\n");
                    goto END;
                }
            }
            if (show_progress)
                aacenc_progress_update(&progress, wav_get_position(wavf),
                                       format->sample_rate * 2);
        }
        if ((consumed = aac_encode_frame(encoder, format, pcmbuf, nread,
                                         &obuf, &olen, &osize)) < 0)
            goto END;
        if (olen > 0) {
            if (write_sample(ofp, m4af, obuf, olen, frame_length) < 0)
                goto END;
            ++frames_written;
        }
    } while (nread > 0 || olen > 0);

    if (show_progress)
        aacenc_progress_finish(&progress, wav_get_position(wavf));
    rc = frames_written;
END:
    if (ibuf) free(ibuf);
    if (pcmbuf) free(pcmbuf);
    if (obuf) free(obuf);
    return rc;
}

static
void put_tool_tag(m4af_ctx_t *m4af, const aacenc_param_ex_t *params,
                  HANDLE_AACENCODER encoder)
{
    char tool_info[256];
    char *p = tool_info;
    LIB_INFO *lib_info = 0;

    p += sprintf(p, PROGNAME " %s, ", fdkaac_version);

    lib_info = calloc(FDK_MODULE_LAST, sizeof(LIB_INFO));
    if (aacEncGetLibInfo(lib_info) == AACENC_OK) {
        int i;
        for (i = 0; i < FDK_MODULE_LAST; ++i)
            if (lib_info[i].module_id == FDK_AACENC)
                break;
        p += sprintf(p, "libfdk-aac %s, ", lib_info[i].versionStr);
    }
    free(lib_info);
    if (params->bitrate_mode)
        sprintf(p, "VBR mode %d", params->bitrate_mode);
    else
        sprintf(p, "CBR %dkbps",
                aacEncoder_GetParam(encoder, AACENC_BITRATE) / 1000);

    m4af_add_itmf_string_tag(m4af, M4AF_TAG_TOOL, tool_info);
}

static
int finalize_m4a(m4af_ctx_t *m4af, const aacenc_param_ex_t *params,
                 HANDLE_AACENCODER encoder)
{
    unsigned i;
    aacenc_tag_entry_t *tag = params->tags.tag_table;

    if (params->json_filename)
        aacenc_put_tags_from_json(m4af, params->json_filename);

    for (i = 0; i < params->tags.tag_count; ++i, ++tag)
        aacenc_put_tag_entry(m4af, tag);

    put_tool_tag(m4af, params, encoder);

    if (m4af_finalize(m4af, params->moov_before_mdat) < 0) {
        fprintf(stderr, "ERROR: failed to finalize m4a\n");
        return -1;
    }
    return 0;
}

static
char *generate_output_filename(const char *filename, const char *ext)
{
    char *p = 0;
    size_t ext_len = strlen(ext);

    if (strcmp(filename, "-") == 0) {
        p = malloc(ext_len + 6);
        sprintf(p, "stdin%s", ext);
    } else {
        const char *base = aacenc_basename(filename);
        size_t ilen = strlen(base);
        const char *ext_org = strrchr(base, '.');
        if (ext_org) ilen = ext_org - base;
        p = malloc(ilen + ext_len + 1);
        sprintf(p, "%.*s%s", ilen, base, ext);
    }
    return p;
}

static
int parse_raw_spec(const char *spec, pcm_sample_description_t *desc)
{
    unsigned bits;
    unsigned char c_type, c_endian = 'L';
    int type;

    if (sscanf(spec, "%c%u%c", &c_type, &bits, &c_endian) < 2)
        return -1;
    c_type = toupper(c_type);
    c_endian = toupper(c_endian);

    if (c_type == 'S')
        type = 1;
    else if (c_type == 'U')
        type = 2;
    else if (c_type == 'F')
        type = 4;
    else
        return -1;

    if (c_endian == 'B')
        type |= 8;
    else if (c_endian != 'L')
        return -1;

    if (c_type == 'F' && bits != 32 && bits != 64)
        return -1;
    if (c_type != 'F' && (bits < 8 || bits > 32))
        return -1;

    desc->sample_type = type;
    desc->bits_per_channel = bits;
    return 0;
}

#ifdef USE_LIBSNDFILE
static
int parse_sndfile_spec_blah(SF_INFO* info, pcm_sample_description_t *desc)
{
    int type,bits;
    
    if(info->format & SF_FORMAT_FLOAT) {
        type = PCM_TYPE_FLOAT;
    } 
    desc->sample_type = type;
    desc->bits_per_channel = bits;
    return 0;
}
#endif

int main(int argc, char **argv)
{
    wav_io_context_t wav_io = { read_callback, seek_callback, tell_callback };
    m4af_io_callbacks_t
        m4af_io = { read_callback, write_callback, seek_callback, tell_callback };
    aacenc_param_ex_t params = { 0 };

    int result = 2;
    FILE *ifp = 0;
    FILE *ofp = 0;
    char *output_filename = 0;
#ifdef USE_LIBSNDFILE
    SNDFILE* snd = NULL;
    SF_INFO snd_info;
    pcm_sample_description_t snd_desc = { 0 };
#else
    wav_reader_t *wavf = 0;
#endif
    HANDLE_AACENCODER encoder = 0;
    AACENC_InfoStruct aacinfo = { 0 };
    m4af_ctx_t *m4af = 0;
    const pcm_sample_description_t *sample_format;
    int downsampled_timescale = 0;
    int frame_count = 0;
    struct stat stb = { 0 };

    setlocale(LC_CTYPE, "");
    setbuf(stderr, 0);

    if (parse_options(argc, argv, &params) < 0)
        return 1;

#ifdef USE_LIBSNDFILE
    if ((snd = sf_open (params.input_filename, SFM_READ, &snd_info)) == NULL) {
        fprintf(stderr, "ERROR: broken / unsupported input file\n");
        goto END;
    }
#ifdef USE_LIBSAMPLERATE
    if(params.resample) {
        snd_desc.sample_rate = params.resample;
        printf("resampling to %dhz\n", snd_desc.sample_rate);
    } else {
        snd_desc.sample_rate = snd_info.samplerate;
    }
    snd_desc.sample_type = PCM_TYPE_FLOAT; // always -- libsndfile does the conversion for us
    snd_desc.bits_per_channel = sizeof(float)*8;
#else
    snd_desc.sample_rate = snd_info.samplerate;
    snd_desc.sample_type = PCM_TYPE_SINT; // always -- libsndfile does the conversion for us
    snd_desc.bits_per_channel = sizeof(short)*8;
#endif
    snd_desc.channels_per_frame = snd_info.channels;
    snd_desc.bytes_per_frame = snd_info.channels * (snd_desc.bits_per_channel / 8);
    snd_desc.channel_mask = 0;

    sample_format = &snd_desc;
#else
    if ((ifp = aacenc_fopen(params.input_filename, "rb")) == 0) {
        aacenc_fprintf(stderr, "ERROR: %s: %s\n", params.input_filename,
                       strerror(errno));
        goto END;
    }
    
    if (fstat(fileno(ifp), &stb) == 0 && (stb.st_mode & S_IFMT) != S_IFREG) {
        wav_io.seek = 0;
        wav_io.tell = 0;
    }
    
    if (!params.is_raw) {
        if ((wavf = wav_open(&wav_io, ifp, params.ignore_length)) == 0) {
            fprintf(stderr, "ERROR: broken / unsupported input file\n");
            goto END;
        }
    } else {
        int bytes_per_channel;
        pcm_sample_description_t desc = { 0 };
        if (parse_raw_spec(params.raw_format, &desc) < 0) {
            fprintf(stderr, "ERROR: invalid raw-format spec\n");
            goto END;
        }
        desc.sample_rate = params.raw_rate;
        desc.channels_per_frame = params.raw_channels;
        bytes_per_channel = (desc.bits_per_channel + 7) / 8;
        desc.bytes_per_frame = params.raw_channels * bytes_per_channel;
        if ((wavf = raw_open(&wav_io, ifp, &desc)) == 0) {
            fprintf(stderr, "ERROR: failed to open raw input\n");
            goto END;
        }
    }

    sample_format = wav_get_format(wavf);
#endif

    if (aacenc_init(&encoder, (aacenc_param_t*)&params, sample_format,
                    &aacinfo) < 0)
        goto END;

    if (!params.output_filename) {
        const char *ext = params.transport_format ? ".aac" : ".m4a";
        output_filename = generate_output_filename(params.input_filename, ext);
        params.output_filename = output_filename;
    }

    if ((ofp = aacenc_fopen(params.output_filename, "wb+")) == 0) {
        aacenc_fprintf(stderr, "ERROR: %s: %s\n", params.output_filename,
                       strerror(errno));
        goto END;
    }
    handle_signals();
    if (!params.transport_format) {
        uint32_t scale;
        unsigned framelen = aacinfo.frameLength;
        int sbr_mode = aacenc_is_sbr_active((aacenc_param_t*)&params);
        int sig_mode = aacEncoder_GetParam(encoder, AACENC_SIGNALING_MODE);
        if (sbr_mode && !sig_mode)
            downsampled_timescale = 1;
        scale = sample_format->sample_rate >> downsampled_timescale;
        if ((m4af = m4af_create(M4AF_CODEC_MP4A, scale, &m4af_io, ofp)) < 0)
            goto END;
        m4af_set_decoder_specific_info(m4af, 0, aacinfo.confBuf,
                                       aacinfo.confSize);
        m4af_set_fixed_frame_duration(m4af, 0,
                                      framelen >> downsampled_timescale);
        m4af_set_priming_mode(m4af, params.gapless_mode + 1);
        m4af_begin_write(m4af);
    }

#ifdef USE_LIBSNDFILE
    frame_count = encode_sndfile(snd, &snd_info, &snd_desc, encoder, aacinfo.frameLength, ofp, m4af,
                         !params.silent);
#else
    frame_count = encode(wavf, encoder, aacinfo.frameLength, ofp, m4af,
                         !params.silent);
#endif

    if (frame_count < 0)
        goto END;
    if (m4af) {
        uint32_t delay = aacinfo.nDelay;
#ifdef USE_LIBSNDFILE
        int64_t frames_read = sf_seek(snd, 0, SEEK_CUR);
#else
        int64_t frames_read = wav_get_position(wavf);
#endif
        uint32_t padding = frame_count * aacinfo.frameLength
                            - frames_read - delay;
        m4af_set_priming(m4af, 0, delay >> downsampled_timescale,
                         padding >> downsampled_timescale);
        if (finalize_m4a(m4af, &params, encoder) < 0)
            goto END;
    }
    result = 0;
END:
#ifdef USE_LIBSNDFILE
    if (snd) sf_close(snd);
#else
    if (wavf) wav_teardown(&wavf);
    if (ifp) fclose(ifp);
#endif
    if (m4af) m4af_teardown(&m4af);
    if (ofp) fclose(ofp);
    if (encoder) aacEncClose(&encoder);
    if (output_filename) free(output_filename);
    if (params.tags.tag_table) free(params.tags.tag_table);

    return result;
}
