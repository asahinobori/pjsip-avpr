#include <pjmedia.h>
#include <pjmedia/webm.h>
#include <pjmedia/errno.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/pool.h>

#define THIS_FILE "webm_writer.c"
#define SIGNATURE	    PJMEDIA_SIG_PORT_VID_WEBM_WRITER

#include <vpx/vpx_config.h>
#include <pjmedia/tools_common.h>
#include <vpx/vpx_timer.h>
#include <pjmedia/scale.h>

#include <pjmedia/webmenc.h>
#include <pjmedia/vpxstats.h>

#include "vpx/vpx_encoder.h"
#if CONFIG_DECODERS
#include "vpx/vpx_decoder.h"
#endif

#if CONFIG_VP8_ENCODER || CONFIG_VP9_ENCODER
#include "vpx/vp8cx.h"
#endif
#if CONFIG_VP8_DECODER || CONFIG_VP9_DECODER
#include "vpx/vp8dx.h"
#endif

struct pjmedia_webm_streams
{
  unsigned     num_streams;
  pjmedia_port **streams;
};


/* vpxenc related data member declare of define
 * Start
 */

static const char *exec_name;

static const struct codec_item {
  char const              *name;
  const vpx_codec_iface_t *(*iface)(void);
  const vpx_codec_iface_t *(*dx_iface)(void);
  unsigned int             fourcc;
} codecs[] = {
#if CONFIG_VP8_ENCODER && CONFIG_VP8_DECODER
  {"vp8", &vpx_codec_vp8_cx, &vpx_codec_vp8_dx, VP8_FOURCC},
#elif CONFIG_VP8_ENCODER && !CONFIG_VP8_DECODER
  {"vp8", &vpx_codec_vp8_cx, NULL, VP8_FOURCC},
#endif
#if CONFIG_VP9_ENCODER && CONFIG_VP9_DECODER
  {"vp9", &vpx_codec_vp9_cx, &vpx_codec_vp9_dx, VP9_FOURCC},
#elif CONFIG_VP9_ENCODER && !CONFIG_VP9_DECODER
  {"vp9", &vpx_codec_vp9_cx, NULL, VP9_FOURCC},
#endif
};

#include <pjmedia/args.h>
static const arg_def_t debugmode = ARG_DEF("D", "debug", 0,
                                           "Debug mode (makes output deterministic)");
static const arg_def_t outputfile = ARG_DEF("o", "output", 1,
                                            "Output filename");
static const arg_def_t use_yv12 = ARG_DEF(NULL, "yv12", 0,
                                          "Input file is YV12 ");
static const arg_def_t use_i420 = ARG_DEF(NULL, "i420", 0,
                                          "Input file is I420 (default)");
static const arg_def_t codecarg = ARG_DEF(NULL, "codec", 1,
                                          "Codec to use");
static const arg_def_t passes           = ARG_DEF("p", "passes", 1,
                                                  "Number of passes (1/2)");
static const arg_def_t pass_arg         = ARG_DEF(NULL, "pass", 1,
                                                  "Pass to execute (1/2)");
static const arg_def_t fpf_name         = ARG_DEF(NULL, "fpf", 1,
                                                  "First pass statistics file name");
static const arg_def_t limit = ARG_DEF(NULL, "limit", 1,
                                       "Stop encoding after n input frames");
static const arg_def_t skip = ARG_DEF(NULL, "skip", 1,
                                      "Skip the first n input frames");
static const arg_def_t deadline         = ARG_DEF("d", "deadline", 1,
                                                  "Deadline per frame (usec)");
static const arg_def_t best_dl          = ARG_DEF(NULL, "best", 0,
                                                  "Use Best Quality Deadline");
static const arg_def_t good_dl          = ARG_DEF(NULL, "good", 0,
                                                  "Use Good Quality Deadline");
static const arg_def_t rt_dl            = ARG_DEF(NULL, "rt", 0,
                                                  "Use Realtime Quality Deadline");
static const arg_def_t quietarg         = ARG_DEF("q", "quiet", 0,
                                                  "Do not print encode progress");
static const arg_def_t verbosearg       = ARG_DEF("v", "verbose", 0,
                                                  "Show encoder parameters");
static const arg_def_t psnrarg          = ARG_DEF(NULL, "psnr", 0,
                                                  "Show PSNR in status line");
enum TestDecodeFatality {
  TEST_DECODE_OFF,
  TEST_DECODE_FATAL,
  TEST_DECODE_WARN,
};
static const struct arg_enum_list test_decode_enum[] = {
  {"off",   TEST_DECODE_OFF},
  {"fatal", TEST_DECODE_FATAL},
  {"warn",  TEST_DECODE_WARN},
  {NULL, 0}
};
static const arg_def_t recontest = ARG_DEF_ENUM(NULL, "test-decode", 1,
                                                "Test encode/decode mismatch",
                                                test_decode_enum);
static const arg_def_t framerate        = ARG_DEF(NULL, "fps", 1,
                                                  "Stream frame rate (rate/scale)");
static const arg_def_t use_ivf          = ARG_DEF(NULL, "ivf", 0,
                                                  "Output IVF (default is WebM)");
static const arg_def_t out_part = ARG_DEF("P", "output-partitions", 0,
                                          "Makes encoder output partitions. Requires IVF output!");
static const arg_def_t q_hist_n         = ARG_DEF(NULL, "q-hist", 1,
                                                  "Show quantizer histogram (n-buckets)");
static const arg_def_t rate_hist_n         = ARG_DEF(NULL, "rate-hist", 1,
                                                     "Show rate histogram (n-buckets)");
static const arg_def_t *main_args[] = {
  &debugmode,
  &outputfile, &codecarg, &passes, &pass_arg, &fpf_name, &limit, &skip,
  &deadline, &best_dl, &good_dl, &rt_dl,
  &quietarg, &verbosearg, &psnrarg, &use_ivf, &out_part, &q_hist_n, &rate_hist_n,
  NULL
};

static const arg_def_t usage            = ARG_DEF("u", "usage", 1,
                                                  "Usage profile number to use");
static const arg_def_t threads          = ARG_DEF("t", "threads", 1,
                                                  "Max number of threads to use");
static const arg_def_t profile          = ARG_DEF(NULL, "profile", 1,
                                                  "Bitstream profile number to use");
static const arg_def_t width            = ARG_DEF("w", "width", 1,
                                                  "Frame width");
static const arg_def_t height           = ARG_DEF("h", "height", 1,
                                                  "Frame height");
static const struct arg_enum_list stereo_mode_enum[] = {
  {"mono", STEREO_FORMAT_MONO},
  {"left-right", STEREO_FORMAT_LEFT_RIGHT},
  {"bottom-top", STEREO_FORMAT_BOTTOM_TOP},
  {"top-bottom", STEREO_FORMAT_TOP_BOTTOM},
  {"right-left", STEREO_FORMAT_RIGHT_LEFT},
  {NULL, 0}
};
static const arg_def_t stereo_mode      = ARG_DEF_ENUM(NULL, "stereo-mode", 1,
                                                       "Stereo 3D video format", stereo_mode_enum);
static const arg_def_t timebase         = ARG_DEF(NULL, "timebase", 1,
                                                  "Output timestamp precision (fractional seconds)");
static const arg_def_t error_resilient  = ARG_DEF(NULL, "error-resilient", 1,
                                                  "Enable error resiliency features");
static const arg_def_t lag_in_frames    = ARG_DEF(NULL, "lag-in-frames", 1,
                                                  "Max number of frames to lag");

static const arg_def_t *global_args[] = {
  &use_yv12, &use_i420, &usage, &threads, &profile,
  &width, &height, &stereo_mode, &timebase, &framerate,
  &error_resilient,
  &lag_in_frames, NULL
};

static const arg_def_t dropframe_thresh   = ARG_DEF(NULL, "drop-frame", 1,
                                                    "Temporal resampling threshold (buf %)");
static const arg_def_t resize_allowed     = ARG_DEF(NULL, "resize-allowed", 1,
                                                    "Spatial resampling enabled (bool)");
static const arg_def_t resize_up_thresh   = ARG_DEF(NULL, "resize-up", 1,
                                                    "Upscale threshold (buf %)");
static const arg_def_t resize_down_thresh = ARG_DEF(NULL, "resize-down", 1,
                                                    "Downscale threshold (buf %)");
static const struct arg_enum_list end_usage_enum[] = {
  {"vbr", VPX_VBR},
  {"cbr", VPX_CBR},
  {"cq",  VPX_CQ},
  {"q",   VPX_Q},
  {NULL, 0}
};
static const arg_def_t end_usage          = ARG_DEF_ENUM(NULL, "end-usage", 1,
                                                         "Rate control mode", end_usage_enum);
static const arg_def_t target_bitrate     = ARG_DEF(NULL, "target-bitrate", 1,
                                                    "Bitrate (kbps)");
static const arg_def_t min_quantizer      = ARG_DEF(NULL, "min-q", 1,
                                                    "Minimum (best) quantizer");
static const arg_def_t max_quantizer      = ARG_DEF(NULL, "max-q", 1,
                                                    "Maximum (worst) quantizer");
static const arg_def_t undershoot_pct     = ARG_DEF(NULL, "undershoot-pct", 1,
                                                    "Datarate undershoot (min) target (%)");
static const arg_def_t overshoot_pct      = ARG_DEF(NULL, "overshoot-pct", 1,
                                                    "Datarate overshoot (max) target (%)");
static const arg_def_t buf_sz             = ARG_DEF(NULL, "buf-sz", 1,
                                                    "Client buffer size (ms)");
static const arg_def_t buf_initial_sz     = ARG_DEF(NULL, "buf-initial-sz", 1,
                                                    "Client initial buffer size (ms)");
static const arg_def_t buf_optimal_sz     = ARG_DEF(NULL, "buf-optimal-sz", 1,
                                                    "Client optimal buffer size (ms)");
static const arg_def_t *rc_args[] = {
  &dropframe_thresh, &resize_allowed, &resize_up_thresh, &resize_down_thresh,
  &end_usage, &target_bitrate, &min_quantizer, &max_quantizer,
  &undershoot_pct, &overshoot_pct, &buf_sz, &buf_initial_sz, &buf_optimal_sz,
  NULL
};


static const arg_def_t bias_pct = ARG_DEF(NULL, "bias-pct", 1,
                                          "CBR/VBR bias (0=CBR, 100=VBR)");
static const arg_def_t minsection_pct = ARG_DEF(NULL, "minsection-pct", 1,
                                                "GOP min bitrate (% of target)");
static const arg_def_t maxsection_pct = ARG_DEF(NULL, "maxsection-pct", 1,
                                                "GOP max bitrate (% of target)");
static const arg_def_t *rc_twopass_args[] = {
  &bias_pct, &minsection_pct, &maxsection_pct, NULL
};


static const arg_def_t kf_min_dist = ARG_DEF(NULL, "kf-min-dist", 1,
                                             "Minimum keyframe interval (frames)");
static const arg_def_t kf_max_dist = ARG_DEF(NULL, "kf-max-dist", 1,
                                             "Maximum keyframe interval (frames)");
static const arg_def_t kf_disabled = ARG_DEF(NULL, "disable-kf", 0,
                                             "Disable keyframe placement");
static const arg_def_t *kf_args[] = {
  &kf_min_dist, &kf_max_dist, &kf_disabled, NULL
};


static const arg_def_t noise_sens = ARG_DEF(NULL, "noise-sensitivity", 1,
                                            "Noise sensitivity (frames to blur)");
static const arg_def_t sharpness = ARG_DEF(NULL, "sharpness", 1,
                                           "Filter sharpness (0-7)");
static const arg_def_t static_thresh = ARG_DEF(NULL, "static-thresh", 1,
                                               "Motion detection threshold");
static const arg_def_t cpu_used = ARG_DEF(NULL, "cpu-used", 1,
                                          "CPU Used (-16..16)");
static const arg_def_t token_parts = ARG_DEF(NULL, "token-parts", 1,
                                     "Number of token partitions to use, log2");
static const arg_def_t tile_cols = ARG_DEF(NULL, "tile-columns", 1,
                                         "Number of tile columns to use, log2");
static const arg_def_t tile_rows = ARG_DEF(NULL, "tile-rows", 1,
                                           "Number of tile rows to use, log2");
static const arg_def_t auto_altref = ARG_DEF(NULL, "auto-alt-ref", 1,
                                             "Enable automatic alt reference frames");
static const arg_def_t arnr_maxframes = ARG_DEF(NULL, "arnr-maxframes", 1,
                                                "AltRef Max Frames");
static const arg_def_t arnr_strength = ARG_DEF(NULL, "arnr-strength", 1,
                                               "AltRef Strength");
static const arg_def_t arnr_type = ARG_DEF(NULL, "arnr-type", 1,
                                           "AltRef Type");
static const struct arg_enum_list tuning_enum[] = {
  {"psnr", VP8_TUNE_PSNR},
  {"ssim", VP8_TUNE_SSIM},
  {NULL, 0}
};
static const arg_def_t tune_ssim = ARG_DEF_ENUM(NULL, "tune", 1,
                                                "Material to favor", tuning_enum);
static const arg_def_t cq_level = ARG_DEF(NULL, "cq-level", 1,
                                          "Constant/Constrained Quality level");
static const arg_def_t max_intra_rate_pct = ARG_DEF(NULL, "max-intra-rate", 1,
                                                    "Max I-frame bitrate (pct)");
static const arg_def_t lossless = ARG_DEF(NULL, "lossless", 1, "Lossless mode");
#if CONFIG_VP9_ENCODER
static const arg_def_t frame_parallel_decoding  = ARG_DEF(
    NULL, "frame-parallel", 1, "Enable frame parallel decodability features");
#endif

#if CONFIG_VP8_ENCODER
static const arg_def_t *vp8_args[] = {
  &cpu_used, &auto_altref, &noise_sens, &sharpness, &static_thresh,
  &token_parts, &arnr_maxframes, &arnr_strength, &arnr_type,
  &tune_ssim, &cq_level, &max_intra_rate_pct,
  NULL
};
static const int vp8_arg_ctrl_map[] = {
  VP8E_SET_CPUUSED, VP8E_SET_ENABLEAUTOALTREF,
  VP8E_SET_NOISE_SENSITIVITY, VP8E_SET_SHARPNESS, VP8E_SET_STATIC_THRESHOLD,
  VP8E_SET_TOKEN_PARTITIONS,
  VP8E_SET_ARNR_MAXFRAMES, VP8E_SET_ARNR_STRENGTH, VP8E_SET_ARNR_TYPE,
  VP8E_SET_TUNING, VP8E_SET_CQ_LEVEL, VP8E_SET_MAX_INTRA_BITRATE_PCT,
  0
};
#endif

#if CONFIG_VP9_ENCODER
static const arg_def_t *vp9_args[] = {
  &cpu_used, &auto_altref, &noise_sens, &sharpness, &static_thresh,
  &tile_cols, &tile_rows, &arnr_maxframes, &arnr_strength, &arnr_type,
  &tune_ssim, &cq_level, &max_intra_rate_pct, &lossless,
  &frame_parallel_decoding,
  NULL
};
static const int vp9_arg_ctrl_map[] = {
  VP8E_SET_CPUUSED, VP8E_SET_ENABLEAUTOALTREF,
  VP8E_SET_NOISE_SENSITIVITY, VP8E_SET_SHARPNESS, VP8E_SET_STATIC_THRESHOLD,
  VP9E_SET_TILE_COLUMNS, VP9E_SET_TILE_ROWS,
  VP8E_SET_ARNR_MAXFRAMES, VP8E_SET_ARNR_STRENGTH, VP8E_SET_ARNR_TYPE,
  VP8E_SET_TUNING, VP8E_SET_CQ_LEVEL, VP8E_SET_MAX_INTRA_BITRATE_PCT,
  VP9E_SET_LOSSLESS, VP9E_SET_FRAME_PARALLEL_DECODING,
  0
};
#endif

static const arg_def_t *no_args[] = { NULL };

#define NELEMENTS(x) (sizeof(x)/sizeof(x[0]))
#define MAX(x,y) ((x)>(y)?(x):(y))
#if CONFIG_VP8_ENCODER && !CONFIG_VP9_ENCODER
#define ARG_CTRL_CNT_MAX NELEMENTS(vp8_arg_ctrl_map)
#elif !CONFIG_VP8_ENCODER && CONFIG_VP9_ENCODER
#define ARG_CTRL_CNT_MAX NELEMENTS(vp9_arg_ctrl_map)
#else
#define ARG_CTRL_CNT_MAX MAX(NELEMENTS(vp8_arg_ctrl_map), \
                             NELEMENTS(vp9_arg_ctrl_map))
#endif

#define HIST_BAR_MAX 40
struct hist_bucket {
  int low, high, count;
};

#define RATE_BINS (100)
struct rate_hist {
  int64_t            *pts;
  int                *sz;
  int                 samples;
  int                 frames;
  struct hist_bucket  bucket[RATE_BINS];
  int                 total;
};

enum video_file_type {
  FILE_TYPE_RAW,
  FILE_TYPE_IVF,
  FILE_TYPE_Y4M
};

struct detect_buffer {
  char buf[4];
  size_t buf_read;
  size_t position;
};

struct input_state {  //change name later
  char                 *fn;
  FILE                 *file;
  off_t                 length;
  struct detect_buffer  detect;
  enum video_file_type  file_type;
  unsigned int          w;
  unsigned int          h;
  struct vpx_rational   framerate;
  int                   use_i420;
  int                   only_i420;
}; 



/* Configuration elements common to all streams */
struct global_config {
  const struct codec_item  *codec;
  int                       passes;
  int                       pass;
  int                       usage;
  int                       deadline;
  int                       use_i420;
  int                       quiet;
  int                       verbose;
  int                       limit;
  int                       skip_frames;
  int                       show_psnr;
  int                       have_framerate;
  struct vpx_rational       framerate;
  int                       out_part;
  int                       debug;
  int                       show_q_hist_buckets;
  int                       show_rate_hist_buckets;
};

/* Per-stream configuration */
struct stream_config {
  struct vpx_codec_enc_cfg  cfg;
  const char               *out_fn;
  const char               *stats_fn;
  stereo_format_t           stereo_fmt;
  int                       arg_ctrls[ARG_CTRL_CNT_MAX][2];
  int                       arg_ctrl_cnt;
  int                       write_webm;
  int                       have_kf_max_dist;
};

struct stream_state {
  int                       index;
  struct stream_state      *next;
  struct stream_config      config;
  FILE                     *file;
  struct rate_hist          rate_hist;
  struct EbmlGlobal         ebml;
  uint32_t                  hash;
  uint64_t                  psnr_sse_total;
  uint64_t                  psnr_samples_total;
  double                    psnr_totals[4];
  int                       psnr_count;
  int                       counts[64];
  vpx_codec_ctx_t           encoder;
  unsigned int              frames_out;
  uint64_t                  cx_time;
  size_t                    nbytes;
  stats_io_t                stats;
  struct vpx_image         *img;
  vpx_codec_ctx_t           decoder;
  int                       mismatch_seen;
};

/* vpxenc related data member declare or define
 * End
 */

struct webm_writer_port
{
  pjmedia_port            base;
  unsigned                stream_id;
  unsigned	              options;
  pjmedia_format_id       fmt_id;
  unsigned                usec_per_frame;
  pj_uint16_t	          bits_per_sample;
  pj_ssize_t              size_left;
  pj_uint8_t              *p_last;
  pj_timestamp            next_ts;
  //New, come form vpxenc, start here
  struct input_state      input;
  struct global_config    global;
  struct stream_state     *streams;
  int                     stream_cnt;
  vpx_image_t             raw;
  int                     frame_avail, got_data;
  int64_t                 estimated_time_left;
  int64_t                 average_rate;
  off_t                   lagged_count;
  uint64_t                cx_time;
  int                     pass;
  int                     frames_in, seen_frames;
  //New, come form vpxenc, end here
  pj_status_t	          (*cb)(pjmedia_port*, void*);
};

static pj_status_t webm_put_frame(pjmedia_port *this_port, 
			         pjmedia_frame *frame);
static pj_status_t webm_on_destroy(pjmedia_port *this_port);


/* vpxenc related function declare or define
 * Start
 */

void usage_exit() {
  int i;

  fprintf(stderr, "Usage: %s <options> -o dst_filename src_filename \n",
          exec_name);

  fprintf(stderr, "\nOptions:\n");
  arg_show_usage(stderr, main_args);
  fprintf(stderr, "\nEncoder Global Options:\n");
  arg_show_usage(stderr, global_args);
  fprintf(stderr, "\nRate Control Options:\n");
  arg_show_usage(stderr, rc_args);
  fprintf(stderr, "\nTwopass Rate Control Options:\n");
  arg_show_usage(stderr, rc_twopass_args);
  fprintf(stderr, "\nKeyframe Placement Options:\n");
  arg_show_usage(stderr, kf_args);
#if CONFIG_VP8_ENCODER
  fprintf(stderr, "\nVP8 Specific Options:\n");
  arg_show_usage(stderr, vp8_args);
#endif
#if CONFIG_VP9_ENCODER
  fprintf(stderr, "\nVP9 Specific Options:\n");
  arg_show_usage(stderr, vp9_args);
#endif
  fprintf(stderr, "\nStream timebase (--timebase):\n"
          "  The desired precision of timestamps in the output, expressed\n"
          "  in fractional seconds. Default is 1/1000.\n");
  fprintf(stderr, "\n"
          "Included encoders:\n"
          "\n");

  for (i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
    fprintf(stderr, "    %-6s - %s\n",
            codecs[i].name,
            vpx_codec_iface_name(codecs[i].iface()));

  exit(EXIT_FAILURE);
}

#define FOREACH_STREAM(func)\
  do\
  {\
    struct stream_state  *stream;\
    \
    for(stream = streams; stream; stream = stream->next)\
      func;\
  }while(0)

static void warn_or_exit_on_errorv(vpx_codec_ctx_t *ctx, int fatal,
                                   const char *s, va_list ap) {
  if (ctx->err) {
    const char *detail = vpx_codec_error_detail(ctx);

    vfprintf(stderr, s, ap);
    fprintf(stderr, ": %s\n", vpx_codec_error(ctx));

    if (detail)
      fprintf(stderr, "    %s\n", detail);

    if (fatal)
      exit(EXIT_FAILURE);
  }
}

static void ctx_exit_on_error(vpx_codec_ctx_t *ctx, const char *s, ...) {
  va_list ap;

  va_start(ap, s);
  warn_or_exit_on_errorv(ctx, 1, s, ap);
  va_end(ap);
}

static void warn_or_exit_on_error(vpx_codec_ctx_t *ctx, int fatal,
                                  const char *s, ...) {
  va_list ap;

  va_start(ap, s);
  warn_or_exit_on_errorv(ctx, fatal, s, ap);
  va_end(ap);
}

static struct stream_state *new_stream(struct global_config *global,
                                       struct stream_state *prev) {
  struct stream_state *stream;

  stream = calloc(1, sizeof(*stream));
  if (!stream)
    fatal("Failed to allocate new stream.");
  if (prev) {
    memcpy(stream, prev, sizeof(*stream));
    stream->index++;
    prev->next = stream;
  } else {
    vpx_codec_err_t  res;

    /* Populate encoder configuration */
    res = vpx_codec_enc_config_default(global->codec->iface(),
                                       &stream->config.cfg,
                                       global->usage);
    if (res)
      fatal("Failed to get config: %s\n", vpx_codec_err_to_string(res));

    /* Change the default timebase to a high enough value so that the
     * encoder will always create strictly increasing timestamps.
     */
    stream->config.cfg.g_timebase.den = 1000;

    /* Never use the library's default resolution, require it be parsed
     * from the file or set on the command line.
     */
    stream->config.cfg.g_w = 0;
    stream->config.cfg.g_h = 0;

    /* Initialize remaining stream parameters */
    stream->config.stereo_fmt = STEREO_FORMAT_MONO;
    stream->config.write_webm = 1;
    stream->ebml.last_pts_ms = -1;

    /* Allows removal of the application version from the EBML tags */
    stream->ebml.debug = global->debug;
  }

  /* Output files must be specified for each stream */
  stream->config.out_fn = NULL;

  stream->next = NULL;
  return stream;
}

static int parse_stream_params(struct global_config *global,
                               struct stream_state  *stream) {
  static const arg_def_t **ctrl_args = no_args;
  static const int        *ctrl_args_map = NULL;
  struct stream_config    *config = &stream->config;

  /* Handle codec specific options */
  if (0) {
#if CONFIG_VP8_ENCODER
  } else if (global->codec->iface == vpx_codec_vp8_cx) {
    ctrl_args = vp8_args;
    ctrl_args_map = vp8_arg_ctrl_map;
#endif
#if CONFIG_VP9_ENCODER
  } else if (global->codec->iface == vpx_codec_vp9_cx) {
    ctrl_args = vp9_args;
    ctrl_args_map = vp9_arg_ctrl_map;
#endif
  }

  /* Set cpu-used=4 here */
  config->arg_ctrls[0][0] = ctrl_args_map[0];
  config->arg_ctrls[0][1] = 4;

  /* Set end-usage=cbr here */
  config->cfg.rc_end_usage = VPX_CBR;

  return 0;
}

static void set_stream_dimensions(struct stream_state *stream,
                                  unsigned int w,
                                  unsigned int h) {
  if (!stream->config.cfg.g_w) {
    if (!stream->config.cfg.g_h)
      stream->config.cfg.g_w = w;
    else
      stream->config.cfg.g_w = w * stream->config.cfg.g_h / h;
  }
  if (!stream->config.cfg.g_h) {
    stream->config.cfg.g_h = h * stream->config.cfg.g_w / w;
  }
}

static void validate_stream_config(struct stream_state *stream) {
  struct stream_state *streami;

  if (!stream->config.cfg.g_w || !stream->config.cfg.g_h)
    fatal("Stream %d: Specify stream dimensions with --width (-w) "
          " and --height (-h)", stream->index);

  for (streami = stream; streami; streami = streami->next) {
    /* All streams require output files */
    if (!streami->config.out_fn)
      fatal("Stream %d: Output file is required (specify with -o)",
            streami->index);

    /* Check for two streams outputting to the same file */
    if (streami != stream) {
      const char *a = stream->config.out_fn;
      const char *b = streami->config.out_fn;
      if (!strcmp(a, b) && strcmp(a, "/dev/null") && strcmp(a, ":nul"))
        fatal("Stream %d: duplicate output file (from stream %d)",
              streami->index, stream->index);
    }

    /* Check for two streams sharing a stats file. */
    if (streami != stream) {
      const char *a = stream->config.stats_fn;
      const char *b = streami->config.stats_fn;
      if (a && b && !strcmp(a, b))
        fatal("Stream %d: duplicate stats file (from stream %d)",
              streami->index, stream->index);
    }
  }
}

static void set_default_kf_interval(struct stream_state  *stream,
                                    struct global_config *global) {
  /* Use a max keyframe interval of 5 seconds, if none was
   * specified on the command line.
   */
  if (!stream->config.have_kf_max_dist) {
    double framerate = (double)global->framerate.num / global->framerate.den;
    if (framerate > 0.0)
      stream->config.cfg.kf_max_dist = (unsigned int)(5.0 * framerate);
  }
}

static void init_rate_histogram(struct rate_hist          *hist,
                                const vpx_codec_enc_cfg_t *cfg,
                                const vpx_rational_t      *fps) {
  int i;

  /* Determine the number of samples in the buffer. Use the file's framerate
   * to determine the number of frames in rc_buf_sz milliseconds, with an
   * adjustment (5/4) to account for alt-refs
   */
  hist->samples = cfg->rc_buf_sz * 5 / 4 * fps->num / fps->den / 1000;

  /* prevent division by zero */
  if (hist->samples == 0)
    hist->samples = 1;

  hist->pts = calloc(hist->samples, sizeof(*hist->pts));
  hist->sz = calloc(hist->samples, sizeof(*hist->sz));
  for (i = 0; i < RATE_BINS; i++) {
    hist->bucket[i].low = INT_MAX;
    hist->bucket[i].high = 0;
    hist->bucket[i].count = 0;
  }
}

static void setup_pass(struct stream_state  *stream,
                       struct global_config *global,
                       int                   pass) {
  if (stream->config.stats_fn) {
    if (!stats_open_file(&stream->stats, stream->config.stats_fn,
                         pass))
      fatal("Failed to open statistics store");
  } else {
    if (!stats_open_mem(&stream->stats, pass))
      fatal("Failed to open statistics store");
  }

  stream->config.cfg.g_pass = global->passes == 2
                              ? pass ? VPX_RC_LAST_PASS : VPX_RC_FIRST_PASS
                            : VPX_RC_ONE_PASS;
  if (pass)
    stream->config.cfg.rc_twopass_stats_in = stats_get(&stream->stats);

  stream->cx_time = 0;
  stream->nbytes = 0;
  stream->frames_out = 0;
}

static void open_output_file(struct stream_state *stream,
                             struct global_config *global) {
  const char *fn = stream->config.out_fn;

  stream->file = strcmp(fn, "-") ? fopen(fn, "wb") : set_binary_mode(stdout);

  if (!stream->file)
    fatal("Failed to open output file");

  if (stream->config.write_webm && fseek(stream->file, 0, SEEK_CUR))
    fatal("WebM output to pipes not supported.");

  if (stream->config.write_webm) {
    stream->ebml.stream = stream->file;
    write_webm_file_header(&stream->ebml, &stream->config.cfg,
                           &global->framerate,
                           stream->config.stereo_fmt,
                           global->codec->fourcc);
  }
}

static void initialize_encoder(struct stream_state  *stream,
                               struct global_config *global) {
  int i;
  int flags = 0;

  flags |= global->show_psnr ? VPX_CODEC_USE_PSNR : 0;
  flags |= global->out_part ? VPX_CODEC_USE_OUTPUT_PARTITION : 0;

  /* Construct Encoder Context */
  vpx_codec_enc_init(&stream->encoder, global->codec->iface(),
                     &stream->config.cfg, flags);
  ctx_exit_on_error(&stream->encoder, "Failed to initialize encoder");

  /* Note that we bypass the vpx_codec_control wrapper macro because
   * we're being clever to store the control IDs in an array. Real
   * applications will want to make use of the enumerations directly
   */
  for (i = 0; i < stream->config.arg_ctrl_cnt; i++) {
    int ctrl = stream->config.arg_ctrls[i][0];
    int value = stream->config.arg_ctrls[i][1];
    if (vpx_codec_control_(&stream->encoder, ctrl, value))
      fprintf(stderr, "Error: Tried to set control %d = %d\n",
              ctrl, value);

    ctx_exit_on_error(&stream->encoder, "Failed to control codec");
  }

}

static int read_frame(struct input_state *input, vpx_image_t *img, const pjmedia_frame *frame) {
  enum video_file_type file_type = input->file_type;
  struct detect_buffer *detect = &input->detect;
  char *buf = frame->buf;
  int plane = 0;
  int shortread = 0;

  for (plane = 0; plane < 3; plane++) {
    unsigned char *ptr;
    int w = (plane ? (1 + img->d_w) / 2 : img->d_w);
    int h = (plane ? (1 + img->d_h) / 2 : img->d_h);
    int r;

    /* Determine the correct plane based on the image format. The for-loop
      * always counts in Y,U,V order, but this may not match the order of
      * the data on disk.
      */
    switch (plane) {
      case 1:
        ptr = img->planes[img->fmt == VPX_IMG_FMT_YV12 ? VPX_PLANE_V : VPX_PLANE_U];
        break;
      case 2:
        ptr = img->planes[img->fmt == VPX_IMG_FMT_YV12 ? VPX_PLANE_U : VPX_PLANE_V];
        break;
      default:
        ptr = img->planes[plane];
    }

    for (r = 0; r < h; r++) {
      size_t needed = w;
      size_t buf_position = 0;
      const size_t left = detect->buf_read - detect->position;
      if (left > 0) {
        const size_t more = (left < needed) ? left : needed;
        memcpy(ptr, detect->buf + detect->position, more);
        buf_position = more;
        needed -= more;
        detect->position += more;
      }
      if (needed > 0) {
        //shortread |= (fread(ptr + buf_position, 1, needed, f) < needed);
        memcpy(ptr + buf_position, buf, needed);
      }
      buf += needed;
      ptr += img->stride[plane];
    }
  }


  return !shortread;
}

static float usec_to_fps(uint64_t usec, unsigned int frames) {
  return (float)(usec > 0 ? frames * 1000000.0 / (float)usec : 0);
}

static void print_time(const char *label, int64_t etl) {
  int hours, mins, secs;

  if (etl >= 0) {
    hours = etl / 3600;
    etl -= hours * 3600;
    mins = etl / 60;
    etl -= mins * 60;
    secs = etl;

    fprintf(stderr, "[%3s %2d:%02d:%02d] ",
            label, hours, mins, secs);
  } else {
    fprintf(stderr, "[%3s  unknown] ", label);
  }
}

static void encode_frame(struct stream_state  *stream,
                         struct global_config *global,
                         struct vpx_image     *img,
                         unsigned int          frames_in) {
  vpx_codec_pts_t frame_start, next_frame_start;
  struct vpx_codec_enc_cfg *cfg = &stream->config.cfg;
  struct vpx_usec_timer timer;

  frame_start = (cfg->g_timebase.den * (int64_t)(frames_in - 1)
                 * global->framerate.den)
                / cfg->g_timebase.num / global->framerate.num;
  next_frame_start = (cfg->g_timebase.den * (int64_t)(frames_in)
                      * global->framerate.den)
                     / cfg->g_timebase.num / global->framerate.num;

  /* Scale if necessary */
  if (img && (img->d_w != cfg->g_w || img->d_h != cfg->g_h)) {
    if (!stream->img)
      stream->img = vpx_img_alloc(NULL, VPX_IMG_FMT_I420,
                                  cfg->g_w, cfg->g_h, 16);
    I420Scale(img->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y],
              img->planes[VPX_PLANE_U], img->stride[VPX_PLANE_U],
              img->planes[VPX_PLANE_V], img->stride[VPX_PLANE_V],
              img->d_w, img->d_h,
              stream->img->planes[VPX_PLANE_Y],
              stream->img->stride[VPX_PLANE_Y],
              stream->img->planes[VPX_PLANE_U],
              stream->img->stride[VPX_PLANE_U],
              stream->img->planes[VPX_PLANE_V],
              stream->img->stride[VPX_PLANE_V],
              stream->img->d_w, stream->img->d_h,
              kFilterBox);

    img = stream->img;
  }

  vpx_usec_timer_start(&timer);
  vpx_codec_encode(&stream->encoder, img, frame_start,
                   (unsigned long)(next_frame_start - frame_start),
                   0, global->deadline);
  vpx_usec_timer_mark(&timer);
  stream->cx_time += vpx_usec_timer_elapsed(&timer);
  ctx_exit_on_error(&stream->encoder, "Stream %d: Failed to encode frame",
                    stream->index);
}

static void update_quantizer_histogram(struct stream_state *stream) {
  if (stream->config.cfg.g_pass != VPX_RC_FIRST_PASS) {
    int q;

    vpx_codec_control(&stream->encoder, VP8E_GET_LAST_QUANTIZER_64, &q);
    ctx_exit_on_error(&stream->encoder, "Failed to read quantizer");
    stream->counts[q]++;
  }
}

static void update_rate_histogram(struct rate_hist          *hist,
                                  const vpx_codec_enc_cfg_t *cfg,
                                  const vpx_codec_cx_pkt_t  *pkt) {
  int i, idx;
  int64_t now, then, sum_sz = 0, avg_bitrate;

  now = pkt->data.frame.pts * 1000
        * (uint64_t)cfg->g_timebase.num / (uint64_t)cfg->g_timebase.den;

  idx = hist->frames++ % hist->samples;
  hist->pts[idx] = now;
  hist->sz[idx] = (int)pkt->data.frame.sz;

  if (now < cfg->rc_buf_initial_sz)
    return;

  then = now;

  /* Sum the size over the past rc_buf_sz ms */
  for (i = hist->frames; i > 0 && hist->frames - i < hist->samples; i--) {
    int i_idx = (i - 1) % hist->samples;

    then = hist->pts[i_idx];
    if (now - then > cfg->rc_buf_sz)
      break;
    sum_sz += hist->sz[i_idx];
  }

  if (now == then)
    return;

  avg_bitrate = sum_sz * 8 * 1000 / (now - then);
  idx = (int)(avg_bitrate * (RATE_BINS / 2) / (cfg->rc_target_bitrate * 1000));
  if (idx < 0)
    idx = 0;
  if (idx > RATE_BINS - 1)
    idx = RATE_BINS - 1;
  if (hist->bucket[idx].low > avg_bitrate)
    hist->bucket[idx].low = (int)avg_bitrate;
  if (hist->bucket[idx].high < avg_bitrate)
    hist->bucket[idx].high = (int)avg_bitrate;
  hist->bucket[idx].count++;
  hist->total++;
}

/* Murmur hash derived from public domain reference implementation at
 *   http:// sites.google.com/site/murmurhash/
 */
static unsigned int murmur(const void *key, int len, unsigned int seed) {
  const unsigned int m = 0x5bd1e995;
  const int r = 24;

  unsigned int h = seed ^ len;

  const unsigned char *data = (const unsigned char *)key;

  while (len >= 4) {
    unsigned int k;

    k  = (unsigned int)data[0];
    k |= (unsigned int)data[1] << 8;
    k |= (unsigned int)data[2] << 16;
    k |= (unsigned int)data[3] << 24;

    k *= m;
    k ^= k >> r;
    k *= m;

    h *= m;
    h ^= k;

    data += 4;
    len -= 4;
  }

  switch (len) {
    case 3:
      h ^= data[2] << 16;
    case 2:
      h ^= data[1] << 8;
    case 1:
      h ^= data[0];
      h *= m;
  };

  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;

  return h;
}

static void get_cx_data(struct stream_state  *stream,
                        struct global_config *global,
                        int                  *got_data) {
  const vpx_codec_cx_pkt_t *pkt;
  const struct vpx_codec_enc_cfg *cfg = &stream->config.cfg;
  vpx_codec_iter_t iter = NULL;

  *got_data = 0;
  while ((pkt = vpx_codec_get_cx_data(&stream->encoder, &iter))) {
    static size_t fsize = 0;
    static off_t ivf_header_pos = 0;

    switch (pkt->kind) {
      case VPX_CODEC_CX_FRAME_PKT:
        if (!(pkt->data.frame.flags & VPX_FRAME_IS_FRAGMENT)) {
          stream->frames_out++;
        }
        if (!global->quiet)
          fprintf(stderr, " %6luF", (unsigned long)pkt->data.frame.sz);

        update_rate_histogram(&stream->rate_hist, cfg, pkt);
        if (stream->config.write_webm) {  //make sure be 1 here now since i delete some else codes
          /* Update the hash */
          if (!stream->ebml.debug)
            stream->hash = murmur(pkt->data.frame.buf,
                                  (int)pkt->data.frame.sz,
                                  stream->hash);

          write_webm_block(&stream->ebml, cfg, pkt);
        }
        stream->nbytes += pkt->data.raw.sz;

        *got_data = 1;

        break;
      case VPX_CODEC_STATS_PKT:
        stream->frames_out++;
        stats_write(&stream->stats,
                    pkt->data.twopass_stats.buf,
                    pkt->data.twopass_stats.sz);
        stream->nbytes += pkt->data.raw.sz;
        break;
      case VPX_CODEC_PSNR_PKT:

        if (global->show_psnr) {
          int i;

          stream->psnr_sse_total += pkt->data.psnr.sse[0];
          stream->psnr_samples_total += pkt->data.psnr.samples[0];
          for (i = 0; i < 4; i++) {
            if (!global->quiet)
              fprintf(stderr, "%.3f ", pkt->data.psnr.psnr[i]);
            stream->psnr_totals[i] += pkt->data.psnr.psnr[i];
          }
          stream->psnr_count++;
        }

        break;
      default:
        break;
    }
  }
}

static void close_output_file(struct stream_state *stream,
                              unsigned int         fourcc) {
  if (stream->config.write_webm) {
    write_webm_file_footer(&stream->ebml, stream->hash);
    free(stream->ebml.cue_list);
    stream->ebml.cue_list = NULL;
  }

  fclose(stream->file);
}

static void destroy_rate_histogram(struct rate_hist *hist) {
  free(hist->pts);
  free(hist->sz);
}

/* vpxenc related function declare or define
 * End
 */

static struct webm_writer_port *create_webm_port(pj_pool_t *pool)
{
  const pj_str_t name = pj_str("webmrec");
  struct webm_writer_port *port;

  port = PJ_POOL_ZALLOC_T(pool, struct webm_writer_port);
  if (!port)
  return NULL;

  /* Put in default values.
   * These will be overriden later in the setting for writer port.
   */
  pjmedia_port_info_init(&port->base.info, &name, SIGNATURE, 
			  8000, 1, 16, 80);

  port->base.put_frame = &webm_put_frame;
  port->base.on_destroy = &webm_on_destroy;

  return port;
}

/*
 * Create WEBM writer port
 */
PJ_DEF(pj_status_t)
pjmedia_webm_writer_create_streams(pj_pool_t *pool, const char *filename,
                                   unsigned options, pjmedia_webm_streams **p_streams)
{
  unsigned i, nstr = 1;
  int pass;
  struct stream_state     *streams = NULL;
  struct webm_writer_port *fport[PJMEDIA_WEBM_MAX_NUM_STREAMS];

  /* Check arguments. */
  PJ_ASSERT_RETURN(pool && filename && p_streams, PJ_EINVAL);

  /* Create fport instance. */
  fport[0] = create_webm_port(pool);
    if (!fport[0]) {
    return PJ_ENOMEM;
  }

  fport[0]->fmt_id = PJMEDIA_FORMAT_I420;
  /*
   * If we want to encode frame using this file's code which
   * use the libvpx's vpxenc, just comment the code of the next line,
   * if we want to encode using the ffmpeg encasulated into pjmedia,
   * do not comment the code of the next line
   */
  //fport[0]->fmt_id = PJMEDIA_FORMAT_VP8;  

  /* vpxenc prepare
   * start here
   */
  
  /* Setup default input stream settings */
  fport[0]->input.framerate.num = 30;
  fport[0]->input.framerate.den = 1;
  fport[0]->input.use_i420 = 1;
  fport[0]->input.only_i420 = 1;
  fport[0]->input.w = 640;
  fport[0]->input.h = 480;

  /* Initialize default parameters */
  memset(&(fport[0]->global), 0, sizeof(fport[0]->global));
  fport[0]->global.codec = codecs;
  fport[0]->global.passes = 0;
  fport[0]->global.use_i420 = 1;
  /* Assign default deadline to good quality */
  fport[0]->global.deadline = VPX_DL_GOOD_QUALITY;

  /* Change it to Real-time CBR Encoding and Streaming */
  fport[0]->global.deadline = VPX_DL_REALTIME;

  /* Validate global config */
  if (fport[0]->global.passes == 0) {
#if CONFIG_VP9_ENCODER
    // Make default VP9 passes = 2 until there is a better quality 1-pass
    // encoder
    fport[0]->global.passes = (fport[0]->global.codec->iface == vpx_codec_vp9_cx ? 2 : 1);
#else
    global->passes = 1;
#endif
  }

  if (fport[0]->global.pass) {
    /* DWIM: Assume the user meant passes=2 if pass=2 is specified */
    if (fport[0]->global.pass > fport[0]->global.passes) {
      warn("Assuming --pass=%d implies --passes=%d\n",
           fport[0]->global.pass, fport[0]->global.pass);
      fport[0]->global.passes = fport[0]->global.pass;
    }
  }

  {
    /* Now parse each stream's parameters. Using a local scope here
     * due to the use of 'stream' as loop variable in FOREACH_STREAM
     * loops
     */
    struct stream_state *stream = NULL;
    static const arg_def_t **ctrl_args = no_args;
    static const int        *ctrl_args_map = NULL;

    do {
      stream = new_stream(&(fport[0]->global), stream);
      fport[0]->stream_cnt++;
      if (!(fport[0]->streams))
        fport[0]->streams = stream;
      stream->config.out_fn = filename;
      //stream->config.cfg.g_timebase.num = 1;
      //stream->config.cfg.g_timebase.den = 30;
    } while (parse_stream_params(&(fport[0]->global), stream));
  }

  pass = fport[0]->global.pass;
  fport[0]->input.file_type = FILE_TYPE_RAW;
  streams = fport[0]->streams;

  FOREACH_STREAM(set_stream_dimensions(stream, fport[0]->input.w, fport[0]->input.h));
  FOREACH_STREAM(validate_stream_config(stream));

  /* Use the frame rate from the file only if none was specified
   * on the command-line.
   */
  if (!fport[0]->global.have_framerate)
    fport[0]->global.framerate = fport[0]->input.framerate;

  FOREACH_STREAM(set_default_kf_interval(stream, &fport[0]->global));

  if (pass == (fport[0]->global.pass ? fport[0]->global.pass - 1 : 0)) {
    if (fport[0]->input.file_type == FILE_TYPE_Y4M)
      /*The Y4M reader does its own allocation.
        Just initialize this here to avoid problems if we never read any
          frames.*/
      memset(&fport[0]->raw, 0, sizeof(fport[0]->raw));
    else
      vpx_img_alloc(&fport[0]->raw,
                    fport[0]->input.use_i420 ? VPX_IMG_FMT_I420
                    : VPX_IMG_FMT_YV12,
                    fport[0]->input.w, fport[0]->input.h, 32);

    FOREACH_STREAM(init_rate_histogram(&stream->rate_hist,
                                        &stream->config.cfg,
                                        &fport[0]->global.framerate));
  }

  FOREACH_STREAM(setup_pass(stream, &fport[0]->global, pass));
  FOREACH_STREAM(open_output_file(stream, &fport[0]->global));
  FOREACH_STREAM(initialize_encoder(stream, &fport[0]->global));

  fport[0]->frame_avail = 1;
  fport[0]->got_data = 0;
  fport[0]->estimated_time_left = -1;
  fport[0]->average_rate = -1;
  fport[0]->lagged_count = 0;
  fport[0]->cx_time = 0;
  fport[0]->frames_in = 0;
  fport[0]->seen_frames = 0;
  fport[0]->global.quiet = 1;   //output some messege,set quiet to 0

  for (i = 0; i < nstr; i++) {
    /* Initialize. */
    fport[i]->options = options;

    if (fport[i]->fmt_id == PJMEDIA_FORMAT_I420) {
    //if (fport[i]->fmt_id == PJMEDIA_FORMAT_VP8) {
      fport[i]->usec_per_frame = (fport[i]->input.framerate.den * 1000000)/(fport[i]->input.framerate.num);

      pjmedia_format_init_video(&fport[i]->base.info.fmt,
                                fport[i]->fmt_id,
                                fport[i]->input.w,
                                fport[i]->input.h,
                                fport[i]->input.framerate.num,
                                fport[i]->input.framerate.den);
    }
    pj_strdup2_with_null(pool, &fport[i]->base.info.name, filename);
  }

  /* Done. */
  *p_streams = pj_pool_alloc(pool, sizeof(pjmedia_webm_streams));
  (*p_streams)->num_streams = nstr;
  (*p_streams)->streams = pj_pool_calloc(pool, (*p_streams)->num_streams,
                                        sizeof(pjmedia_port *));
  for (i = 0; i < nstr; i++)
    (*p_streams)->streams[i] = &fport[i]->base;

  PJ_LOG(4,(THIS_FILE, 
	    "WEBM file writer '%.*s' created with "
	    "%d media ports",
	    (int)fport[0]->base.info.name.slen,
	    fport[0]->base.info.name.ptr,
        (*p_streams)->num_streams));

  return PJ_SUCCESS;
}

static pj_status_t webm_put_frame(pjmedia_port *this_port, 
			         const pjmedia_frame *frame)
{
  struct vpx_usec_timer timer;
  struct stream_state     *streams = NULL;

  struct webm_writer_port *fport = (struct webm_writer_port*)this_port;
  unsigned int w, h, real_w, real_h;
  streams = fport->streams;
  w = fport->input.w;
  h = fport->input.h;
  real_w = fport->base.info.fmt.det.vid.size.w;
  real_h = fport->base.info.fmt.det.vid.size.h;
  if (w != real_w || h != real_h) {
    int pass;
    webm_on_destroy(this_port);
    fport->streams = NULL;

  fport->fmt_id = PJMEDIA_FORMAT_I420;
  /*
   * If we want to encode frame using this file's code which
   * use the libvpx's vpxenc, just comment the code of the next line,
   * if we want to encode using the ffmpeg encasulated into pjmedia,
   * do not comment the code of the next line
   */
  //fport[0]->fmt_id = PJMEDIA_FORMAT_VP8;  

  /* vpxenc prepare
   * start here
   */
  
  /* Setup default input stream settings */
  fport->input.framerate.num = 30;
  fport->input.framerate.den = 1;
  fport->input.use_i420 = 1;
  fport->input.only_i420 = 1;
  fport->input.w = real_w;
  fport->input.h = real_h;

  /* Initialize default parameters */
  memset(&(fport->global), 0, sizeof(fport->global));
  fport->global.codec = codecs;
  fport->global.passes = 0;
  fport->global.use_i420 = 1;
  /* Assign default deadline to good quality */
  fport->global.deadline = VPX_DL_GOOD_QUALITY;

  /* Change it to Real-time CBR Encoding and Streaming */
  fport->global.deadline = VPX_DL_REALTIME;

  /* Validate global config */
  if (fport->global.passes == 0) {
#if CONFIG_VP9_ENCODER
    // Make default VP9 passes = 2 until there is a better quality 1-pass
    // encoder
    fport->global.passes = (fport->global.codec->iface == vpx_codec_vp9_cx ? 2 : 1);
#else
    global->passes = 1;
#endif
  }

  if (fport->global.pass) {
    /* DWIM: Assume the user meant passes=2 if pass=2 is specified */
    if (fport->global.pass > fport->global.passes) {
      warn("Assuming --pass=%d implies --passes=%d\n",
           fport->global.pass, fport->global.pass);
      fport->global.passes = fport->global.pass;
    }
  }

  {
    /* Now parse each stream's parameters. Using a local scope here
     * due to the use of 'stream' as loop variable in FOREACH_STREAM
     * loops
     */
    struct stream_state *stream = NULL;
    static const arg_def_t **ctrl_args = no_args;
    static const int        *ctrl_args_map = NULL;

    do {
      stream = new_stream(&(fport->global), stream);
      fport->stream_cnt++;
      if (!(fport->streams))
        fport->streams = stream;
      stream->config.out_fn = fport->base.info.name.ptr;
      //stream->config.cfg.g_timebase.num = 1;
      //stream->config.cfg.g_timebase.den = 30;
    } while (parse_stream_params(&(fport->global), stream));
  }

  pass = fport->global.pass;
  fport->input.file_type = FILE_TYPE_RAW;
  streams = fport->streams;

  FOREACH_STREAM(set_stream_dimensions(stream, fport->input.w, fport->input.h));
  FOREACH_STREAM(validate_stream_config(stream));

  /* Use the frame rate from the file only if none was specified
   * on the command-line.
   */
  if (!fport->global.have_framerate)
    fport->global.framerate = fport->input.framerate;

  FOREACH_STREAM(set_default_kf_interval(stream, &fport->global));

  if (pass == (fport->global.pass ? fport->global.pass - 1 : 0)) {
    if (fport->input.file_type == FILE_TYPE_Y4M)
      /*The Y4M reader does its own allocation.
        Just initialize this here to avoid problems if we never read any
          frames.*/
      memset(&fport->raw, 0, sizeof(fport->raw));
    else
      vpx_img_alloc(&fport->raw,
                    fport->input.use_i420 ? VPX_IMG_FMT_I420
                    : VPX_IMG_FMT_YV12,
                    fport->input.w, fport->input.h, 32);

    FOREACH_STREAM(init_rate_histogram(&stream->rate_hist,
                                        &stream->config.cfg,
                                        &fport->global.framerate));
  }

  FOREACH_STREAM(setup_pass(stream, &fport->global, pass));
  FOREACH_STREAM(open_output_file(stream, &fport->global));
  FOREACH_STREAM(initialize_encoder(stream, &fport->global));

  fport->frame_avail = 1;
  fport->got_data = 0;
  fport->estimated_time_left = -1;
  fport->average_rate = -1;
  fport->lagged_count = 0;
  fport->cx_time = 0;
  fport->frames_in = 0;
  fport->seen_frames = 0;
  fport->global.quiet = 1;   //output some messege,set quiet to 0
  }

  if (!fport->global.limit || fport->frames_in < fport->global.limit) {
    fport->frame_avail = read_frame(&fport->input, &fport->raw, frame);
    
    if (fport->frame_avail)
      fport->frames_in++;
    fport->seen_frames = fport->frames_in > fport->global.skip_frames ?
                      fport->frames_in - fport->global.skip_frames : 0;

    if (!fport->global.quiet) {
      float fps = usec_to_fps(fport->cx_time, fport->seen_frames);
      fprintf(stderr, "\rPass %d/%d ", fport->pass + 1, fport->global.passes);

      if (fport->stream_cnt == 1)
        fprintf(stderr,
                "frame %4d/%-4d %7"PRId64"B ",
                fport->frames_in, fport->streams->frames_out, (int64_t)fport->streams->nbytes);
      else
        fprintf(stderr, "frame %4d ", fport->frames_in);

      fprintf(stderr, "%7"PRId64" %s %.2f %s ",
              fport->cx_time > 9999999 ? fport->cx_time / 1000 : fport->cx_time,
              fport->cx_time > 9999999 ? "ms" : "us",
              fps >= 1.0 ? fps : fps * 60,
              fps >= 1.0 ? "fps" : "fpm");
      print_time("ETA", fport->estimated_time_left);
      fprintf(stderr, "\033[K");
    }

  } else
    fport->frame_avail = 0;

  if (fport->frames_in > fport->global.skip_frames) {
    vpx_usec_timer_start(&timer);
    FOREACH_STREAM(encode_frame(stream, &fport->global,
                                fport->frame_avail ? &fport->raw : NULL,
                                fport->frames_in));
    vpx_usec_timer_mark(&timer);
    fport->cx_time += vpx_usec_timer_elapsed(&timer);

    FOREACH_STREAM(update_quantizer_histogram(stream));

    fport->got_data = 0;
    FOREACH_STREAM(get_cx_data(stream, &fport->global, &fport->got_data));

    if (!fport->got_data && fport->input.length && !streams->frames_out) {
      fport->lagged_count = fport->seen_frames;
    }
  }

  fflush(stdout);

  return PJ_SUCCESS;

}

/*
 * Destroy port.
 */
static pj_status_t webm_on_destroy(pjmedia_port *this_port)
{
  struct webm_writer_port *fport = (struct webm_writer_port*) this_port;
  struct stream_state     *streams = NULL;
  pj_assert(this_port->info.signature == SIGNATURE);
  
  streams = fport->streams;

  FOREACH_STREAM(vpx_codec_destroy(&stream->encoder));

  FOREACH_STREAM(close_output_file(stream, fport->global.codec->fourcc));

  FOREACH_STREAM(stats_close(&stream->stats, fport->global.passes - 1));

  FOREACH_STREAM(destroy_rate_histogram(&stream->rate_hist));

  vpx_img_free(&fport->raw);
  free(streams);

  return PJ_SUCCESS;
}