#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include <pjmedia/opus_port.h>
#include <pjmedia/errno.h>
#include <pj/file_access.h>
#include <pj/pool.h>
#include <pj/log.h>

#include <pjmedia/opus_header.h>
#include <pjmedia/diag_range.h>


#define MINI(_a,_b)      ((_a)<(_b)?(_a):(_b))
#define MAXI(_a,_b)      ((_a)>(_b)?(_a):(_b))
#define CLAMPI(_a,_b,_c) (MAXI(_a,MINI(_b,_c)))

/* 120ms at 48000 */
#define MAX_FRAME_SIZE (960*6)

#if defined WIN32 || defined _WIN32 || defined WIN64 || defined _WIN64
# include <pjmedia/unicode_support.h>
/* We need the following two to set stdout to binary */
# include <io.h>
# include <fcntl.h>
# define I64FORMAT "I64d"
#else
# define I64FORMAT "lld"
# define fopen_utf8(_x,_y) fopen((_x),(_y))
# define argc_utf8 argc
# define argv_utf8 argv
#endif

#include <math.h>
#ifdef HAVE_LRINTF
# define float2int(x) lrintf(x)
#else
# define float2int(flt) ((int)(floor(.5+flt)))
#endif

typedef struct shapestate shapestate;
struct shapestate {
  float * b_buf;
  float * a_buf;
  int fs;
  int mute;
};

static unsigned int rngseed = 22222;
static __inline unsigned int fast_rand(void) {
  rngseed = (rngseed * 96314165) + 907633515;
  return rngseed;
}

#ifndef HAVE_FMINF
# define fminf(_x,_y) ((_x)<(_y)?(_x):(_y))
#endif

#ifndef HAVE_FMAXF
# define fmaxf(_x,_y) ((_x)>(_y)?(_x):(_y))
#endif

#define THIS_FILE "opus_player.c"

#define SIGNATURE PJMEDIA_SIG_PORT_OPUS_PLAYER
#define BITS_PER_SAMPLE 16

struct opus_reader_port {
  pjmedia_port    base;
  pj_bool_t       eof;
  pj_bool_t       rewind;
  pj_off_t	      fsize;
  pjmedia_frame   *frame;
  struct opus_tool_param *p_otp;
  pj_status_t (*cb)(pjmedia_port*, void*);
};

struct opus_tool_param {
  char *inFile;
  FILE *fin, *frange;
  float *output;
  int frame_size;
  OpusMSDecoder *st;
  opus_int64 packet_count;
  int total_links;
  int stream_init;
  int quiet; //delete later
  ogg_int64_t page_granule;
  ogg_int64_t link_out;
  ogg_sync_state    oy;
  ogg_page          og;
  ogg_packet        op;
  ogg_stream_state  os;
  int has_opus_stream;
  int has_tags_packet;
  ogg_int32_t opus_serialno;
  int dither;
  shapestate shapemem;
  int close_in;
  int eos;
  ogg_int64_t audio_size;
  float loss_percent;
  float manual_gain;
  int channels;
  int mapping_family;
  int rate;
  int wav_format; // Delete later
  int preskip;
  int gran_offset;
  float gain;
  int streams;
};

static pj_status_t opus_get_frame(pjmedia_port *this_port, 
				  pjmedia_frame *frame);
static pj_status_t opus_on_destroy(pjmedia_port *this_port);

static struct opus_reader_port *create_opus_port(pj_pool_t *pool)
{
    const pj_str_t name = pj_str("opusfile");
    struct opus_reader_port *orport;

    orport = PJ_POOL_ZALLOC_T(pool, struct opus_reader_port);
    if (!orport)
	return NULL;

    /* Put in default values.
     * These will be overriden once the file is read.
     */
    pjmedia_port_info_init(&orport->base.info, &name, SIGNATURE, 
			   8000, 1, 16, 160);

    orport->base.get_frame = &opus_get_frame;
    orport->base.on_destroy = &opus_on_destroy;


    return orport;
}

static void quit(int _x) {
#ifdef WIN_UNICODE
  uninit_console_utf8();
#endif
  exit(_x);
}

/* This implements a 16 bit quantization with full triangular dither
   and IIR noise shaping. The noise shaping filters were designed by
   Sebastian Gesemann based on the LAME ATH curves with flattening
   to limit their peak gain to 20 dB.
   (Everyone elses' noise shaping filters are mildly crazy)
   The 48kHz version of this filter is just a warped version of the
   44.1kHz filter and probably could be improved by shifting the
   HF shelf up in frequency a little bit since 48k has a bit more
   room and being more conservative against bat-ears is probably
   more important than more noise suppression.
   This process can increase the peak level of the signal (in theory
   by the peak error of 1.5 +20 dB though this much is unobservable rare)
   so to avoid clipping the signal is attenuated by a couple thousandths
   of a dB. Initially the approach taken here was to only attenuate by
   the 99.9th percentile, making clipping rare but not impossible (like
   SoX) but the limited gain of the filter means that the worst case was
   only two thousandths of a dB more, so this just uses the worst case.
   The attenuation is probably also helpful to prevent clipping in the DAC
   reconstruction filters or downstream resampling in any case.*/
static __inline void shape_dither_toshort(shapestate *_ss, short *_o, float *_i, int _n, int _CC)
{
  const float gains[3]={32768.f-15.f,32768.f-15.f,32768.f-3.f};
  const float fcoef[3][8] =
  {
    {2.2374f, -.7339f, -.1251f, -.6033f, 0.9030f, .0116f, -.5853f, -.2571f}, /* 48.0kHz noise shaping filter sd=2.34*/
    {2.2061f, -.4706f, -.2534f, -.6214f, 1.0587f, .0676f, -.6054f, -.2738f}, /* 44.1kHz noise shaping filter sd=2.51*/
    {1.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,0.0000f, 0.0000f, 0.0000f}, /* lowpass noise shaping filter sd=0.65*/
  };
  int i;
  int rate=_ss->fs==44100?1:(_ss->fs==48000?0:2);
  float gain=gains[rate];
  float *b_buf;
  float *a_buf;
  int mute=_ss->mute;
  b_buf=_ss->b_buf;
  a_buf=_ss->a_buf;
  /*In order to avoid replacing digital silence with quiet dither noise
    we mute if the output has been silent for a while*/
  if(mute>64)
    memset(a_buf,0,sizeof(float)*_CC*4);
  for(i=0;i<_n;i++)
  {
    int c;
    int pos = i*_CC;
    int silent=1;
    for(c=0;c<_CC;c++)
    {
      int j, si;
      float r,s,err=0;
      silent&=_i[pos+c]==0;
      s=_i[pos+c]*gain;
      for(j=0;j<4;j++)
        err += fcoef[rate][j]*b_buf[c*4+j] - fcoef[rate][j+4]*a_buf[c*4+j];
      memmove(&a_buf[c*4+1],&a_buf[c*4],sizeof(float)*3);
      memmove(&b_buf[c*4+1],&b_buf[c*4],sizeof(float)*3);
      a_buf[c*4]=err;
      s = s - err;
      r=(float)fast_rand()*(1/(float)UINT_MAX) - (float)fast_rand()*(1/(float)UINT_MAX);
      if (mute>16)r=0;
      /*Clamp in float out of paranoia that the input will be >96 dBFS and wrap if the
        integer is clamped.*/
      _o[pos+c] = si = float2int(fmaxf(-32768,fminf(s + r,32767)));
      /*Including clipping in the noise shaping is generally disastrous:
        the futile effort to restore the clipped energy results in more clipping.
        However, small amounts-- at the level which could normally be created by
        dither and rounding-- are harmless and can even reduce clipping somewhat
        due to the clipping sometimes reducing the dither+rounding error.*/
      b_buf[c*4] = (mute>16)?0:fmaxf(-1.5f,fminf(si - s,1.5f));
    }
    mute++;
    if(!silent)mute=0;
  }
  _ss->mute=MINI(mute,960);
}

/*Process an Opus header and setup the opus decoder based on it.
  It takes several pointers for header values which are needed
  elsewhere in the code.*/
static OpusMSDecoder *process_header(ogg_packet *op, opus_int32 *rate,
       int *mapping_family, int *channels, int *preskip, float *gain,
       float manual_gain, int *streams, int wav_format, int quiet)
{
   int err;
   OpusMSDecoder *st;
   OpusHeader header;

   if (opus_header_parse(op->packet, op->bytes, &header)==0)
   {
      fprintf(stderr, "Cannot parse header\n");
      return NULL;
   }

   *mapping_family = header.channel_mapping;
   *channels = header.channels;

   if(!*rate)*rate=header.input_sample_rate;
   /*If the rate is unspecified we decode to 48000*/
   if(*rate==0)*rate=48000;
   if(*rate<8000||*rate>192000){
     fprintf(stderr,"Warning: Crazy input_rate %d, decoding to 48000 instead.\n",*rate);
     *rate=48000;
   }

   *preskip = header.preskip;
   st = opus_multistream_decoder_create(48000, header.channels, header.nb_streams, header.nb_coupled, header.stream_map, &err);
   if(err != OPUS_OK){
     fprintf(stderr, "Cannot create encoder: %s\n", opus_strerror(err));
     return NULL;
   }
   if (!st)
   {
      fprintf (stderr, "Decoder initialization failed: %s\n", opus_strerror(err));
      return NULL;
   }

   *streams=header.nb_streams;

   if(header.gain!=0 || manual_gain!=0)
   {
      /*Gain API added in a newer libopus version, if we don't have it
        we apply the gain ourselves. We also add in a user provided
        manual gain at the same time.*/
      int gainadj = (int)(manual_gain*256.)+header.gain;
#ifdef OPUS_SET_GAIN
      err=opus_multistream_decoder_ctl(st,OPUS_SET_GAIN(gainadj));
      if(err==OPUS_UNIMPLEMENTED)
      {
#endif
         *gain = pow(10., gainadj/5120.);
#ifdef OPUS_SET_GAIN
      } else if (err!=OPUS_OK)
      {
         fprintf (stderr, "Error setting gain: %s\n", opus_strerror(err));
         return NULL;
      }
#endif
   }

   if (!quiet)
   {
      fprintf(stderr, "Decoding to %d Hz (%d channel%s)", *rate,
        *channels, *channels>1?"s":"");
      if(header.version!=1)fprintf(stderr, ", Header v%d",header.version);
      fprintf(stderr, "\n");
      if (header.gain!=0)fprintf(stderr,"Playback gain: %f dB\n", header.gain/256.);
      if (manual_gain!=0)fprintf(stderr,"Manual gain: %f dB\n", manual_gain);
   }

   return st;
}

opus_int64 audio_write(float *pcm, int channels, int frame_size, int *skip, 
                       shapestate *shapemem, opus_int64 maxout, pjmedia_port *this_port)
{
  struct opus_reader_port *orport=(struct opus_reader_port *)this_port;
  pjmedia_frame *frame = orport->frame;
   opus_int64 sampout=0;
   int i,ret,tmp_skip;
   unsigned out_len;
   short *out;
   float *buf;
   float *output;
   out=(short *)_alloca(sizeof(short)*MAX_FRAME_SIZE*channels);
   buf=(float *)_alloca(sizeof(float)*MAX_FRAME_SIZE*channels);
   maxout=maxout<0?0:maxout;
   do {
     if (skip){
       tmp_skip = (*skip>frame_size) ? (int)frame_size : *skip;
       *skip -= tmp_skip;
     } else {
       tmp_skip = 0;
     }
       output=pcm+channels*tmp_skip;
       out_len=frame_size-tmp_skip;
       frame_size=0;

     /*Convert to short and save to output file*/
     if (shapemem){
       shape_dither_toshort(shapemem,out,output,out_len,channels);
     }else{
       for (i=0;i<(int)out_len*channels;i++)
         out[i]=(short)float2int(fmaxf(-32768,fminf(output[i]*32768.f,32767)));
     }

     if(maxout>0)
     {
       pj_memcpy(frame->buf, (char *)out, out_len * 2 *channels);
       ret = frame->size = orport->p_otp->frame_size;
       sampout+=ret;
       maxout-=ret;
     }
   } while (frame_size>0 && maxout>0);
   return sampout;
}




static pj_status_t filedata_to_packet(pjmedia_port *this_port) {
  struct opus_reader_port *orport=(struct opus_reader_port *)this_port;
  struct opus_tool_param *otp=orport->p_otp;
  while (1) {
    char *data;
    int i, nb_read;
    /* Get the ogg buffer for writing */
    data = ogg_sync_buffer(&otp->oy, 200);
    /* Read bitstream from input file */
    nb_read = fread(data, sizeof(char), 200, otp->fin);
    ogg_sync_wrote(&otp->oy, nb_read);

    /* Loop for all complete pages we got (most likely only one) */
    while (ogg_sync_pageout(&otp->oy, &otp->og)==1) {
      if (otp->stream_init == 0) {
        ogg_stream_init(&otp->os, ogg_page_serialno(&otp->og));
        otp->stream_init = 1;
      }
      if (ogg_page_serialno(&otp->og) != otp->os.serialno) {
        /* so all streams are read */
        ogg_stream_reset_serialno(&otp->os, ogg_page_serialno(&otp->og));
      }
      /* Add page to the bistream */
      ogg_stream_pagein(&otp->os, &otp->og);
      otp->page_granule = ogg_page_granulepos(&otp->og);
      /* Return to process packet */
      return PJ_SUCCESS;
    }
    if (feof(otp->fin)) {
      if (otp->stream_init)
        ogg_stream_clear(&otp->os);
      ogg_sync_clear(&otp->oy);

      if(otp->shapemem.a_buf) {
        free(otp->shapemem.a_buf);
        otp->shapemem.a_buf = NULL;
      }
      if(otp->shapemem.b_buf) {
        free(otp->shapemem.b_buf);
        otp->shapemem.b_buf = NULL;
      }

      if(otp->output) {
        free(otp->output);
        otp->output = NULL;
      }

      if(otp->frange)fclose(otp->frange);

      if (otp->close_in)
        fclose(otp->fin);
      break;
    }
  }
  return 1;
}

static pj_status_t packet_to_frame(pjmedia_port *this_port) {
  struct opus_reader_port *orport=(struct opus_reader_port *)this_port;
  struct opus_tool_param *otp=orport->p_otp;
  pjmedia_frame *frame = orport->frame;
  pj_status_t status;
  int i;

  while (ogg_stream_packetout(&otp->os, &otp->op) != 1) {
    if(otp->eos) {
      otp->has_opus_stream=0;
      if(otp->st)opus_multistream_decoder_destroy(otp->st);
      otp->st=NULL;
    }
    if (ogg_sync_pageout(&otp->oy, &otp->og) == 1) {
        if (ogg_page_serialno(&otp->og) != otp->os.serialno) {
          /* so all streams are read. */
          ogg_stream_reset_serialno(&otp->os, ogg_page_serialno(&otp->og));
        }
        /*Add page to the bitstream*/
        ogg_stream_pagein(&otp->os, &otp->og);
        otp->page_granule = ogg_page_granulepos(&otp->og);
    } else {
      status = filedata_to_packet(this_port);
      if (status == 1)
        return 1;
    }
  }

  /* Extract all available packets */
  /* OggOpus streams are identified by a magic string in the initial
      stream header. */
  if (otp->op.b_o_s && otp->op.bytes>=8 && !memcmp(otp->op.packet, "OpusHeader", 8)) {
    if (otp->has_opus_stream && otp->has_tags_packet) {
      /*If we're seeing another BOS OpusHead now it means
        the stream is chained without an EOS.*/
      otp->has_opus_stream=0;
      if(otp->st)opus_multistream_decoder_destroy(otp->st);
      otp->st=NULL;
      fprintf(stderr,"\nWarning: stream %" I64FORMAT " ended without EOS and a new stream began.\n",(long long)otp->os.serialno);
    }
    if(!otp->has_opus_stream) {
      if(otp->packet_count>0 && otp->opus_serialno==otp->os.serialno) {
        fprintf(stderr,"\nError: Apparent chaining without changing serial number (%" I64FORMAT "==%" I64FORMAT ").\n",
          (long long)otp->opus_serialno,(long long)otp->os.serialno);
        quit(1);
      }
      otp->opus_serialno = otp->os.serialno;
      otp->has_opus_stream = 1;
      otp->has_tags_packet = 0;
      otp->link_out = 0;
      otp->packet_count = 0;
      otp->eos = 0;
      otp->total_links++;
    } else {
      fprintf(stderr,"\nWarning: ignoring opus stream %" I64FORMAT "\n",(long long)otp->os.serialno);
    }
  }
  if (!otp->has_opus_stream || otp->os.serialno != otp->opus_serialno)
    return PJ_SUCCESS;
  /* If first packet in a logical stream, process the Opus header */
  if (otp->packet_count == 0) {
    otp->st = process_header(&otp->op, &otp->rate, &otp->mapping_family, &otp->channels, &otp->preskip, &otp->gain, otp->manual_gain, &otp->streams, otp->wav_format, otp->quiet);
    if (!otp->st)
      quit(1);
    if (ogg_stream_packetout(&otp->os, &otp->op) != 0 || otp->og.header[otp->og.header_len - 1] ==255) {
      /*The format specifies that the initial header and tags packets are on their
        own pages. To aid implementors in discovering that their files are wrong
        we reject them explicitly here. In some player designs files like this would
        fail even without an explicit test.*/
      fprintf(stderr, "Extra packets on initial header page. Invalid stream.\n");
      quit(1);
    }
    /*Remember how many samples at the front we were told to skip
      so that we can adjust the timestamp counting.*/
    otp->gran_offset=otp->preskip;

    /*Setup the memory for the dithered output*/
    if(!otp->shapemem.a_buf) {
      otp->shapemem.a_buf=(float *)calloc(otp->channels,sizeof(float)*4);
      otp->shapemem.b_buf=(float *)calloc(otp->channels,sizeof(float)*4);
      otp->shapemem.fs=otp->rate;
    }
    if(!otp->output)otp->output=(float *)malloc(sizeof(float)*MAX_FRAME_SIZE*otp->channels);

    /*Normal players should just play at 48000 or their maximum rate,
      as described in the OggOpus spec.  But for commandline tools
      like opusdec it can be desirable to exactly preserve the original
      sampling rate and duration, so we have a resampler here.*/
    //if (otp->rate != 48000 && otp->resampler==NULL) {
    //  int err;
    //  otp->resampler = speex_resampler_init(otp->channels, 48000, otp->rate, 5, &err);
    //  if (err!=0)
    //      fprintf(stderr, "resampler error: %s\n", speex_resampler_strerror(err));
    //  speex_resampler_skip_zeros(otp->resampler);
    //}
  } else if (otp->packet_count == 1) {
    otp->has_tags_packet = 1;
    if(ogg_stream_packetout(&otp->os, &otp->op)!=0 || otp->og.header[otp->og.header_len-1]==255) {
        fprintf(stderr, "Extra packets on initial tags page. Invalid stream.\n");
        quit(1);
    }
  } else {
    int ret;
    opus_int64 maxout;
    opus_int64 outsamp;
    int lost = 0;
    if (otp->loss_percent>0 && 100*((float)rand())/RAND_MAX<otp->loss_percent)
      lost=1;

    /*End of stream condition*/
    if (otp->op.e_o_s && otp->os.serialno == otp->opus_serialno)otp->eos=1; /* don't care for anything except opus eos */

    /*Are we simulating loss for this packet?*/
    if (!lost){
      /*Decode Opus packet*/
      ret = opus_multistream_decode_float(otp->st, (unsigned char*)otp->op.packet, otp->op.bytes, otp->output, MAX_FRAME_SIZE, 0);
    } else {
      /*Extract the original duration.
        Normally you wouldn't have it for a lost packet, but normally the
        transports used on lossy channels will effectively tell you.
        This avoids opusdec squaking when the decoded samples and
        granpos mismatches.*/
      opus_int32 lost_size;
      lost_size = MAX_FRAME_SIZE;
      if(otp->op.bytes>0){
        opus_int32 spp;
        spp=opus_packet_get_nb_frames(otp->op.packet,otp->op.bytes);
        if(spp>0){
          spp*=opus_packet_get_samples_per_frame(otp->op.packet,48000/*decoding_rate*/);
          if(spp>0)lost_size=spp;
        }
      }
      /*Invoke packet loss concealment.*/
      ret = opus_multistream_decode_float(otp->st, NULL, 0, otp->output, lost_size, 0);
    }

    /*If the decoder returned less than zero, we have an error.*/
    if (ret<0)
    {
      fprintf (stderr, "Decoding error: %s\n", opus_strerror(ret));
      return 1;
    }
    otp->frame_size = ret;

    /*If we're collecting --save-range debugging data, collect it now.*/
    if(otp->frange!=NULL) {
      OpusDecoder *od;
      opus_uint32 rngs[256];
      for(i=0;i<otp->streams;i++) {
        ret=opus_multistream_decoder_ctl(otp->st,OPUS_MULTISTREAM_GET_DECODER_STATE(i,&od));
        ret=opus_decoder_ctl(od,OPUS_GET_FINAL_RANGE(&rngs[i]));
      }
      save_range(otp->frange,otp->frame_size*(48000/48000/*decoding_rate*/),otp->op.packet,otp->op.bytes,
                rngs,otp->streams);
    }

    /*Apply header gain, if we're not using an opus library new
      enough to do this internally.*/
    if (otp->gain!=0){
      for (i=0;i<otp->frame_size*otp->channels;i++)
        otp->output[i] *= otp->gain;
    }

    /*This handles making sure that our output duration respects
      the final end-trim by not letting the output sample count
      get ahead of the granpos indicated value.*/
    maxout=((otp->page_granule-otp->gran_offset)*otp->rate/48000)-otp->link_out;
    outsamp=audio_write(otp->output, otp->channels, otp->frame_size, &otp->preskip, otp->dither?&otp->shapemem:0, 0>maxout?0:maxout, this_port);
    otp->link_out+=outsamp;
    otp->audio_size+=sizeof(short)*outsamp*otp->channels;
  }
  otp->packet_count++;
  return PJ_SUCCESS;
}

/*
 * Create OPUS player port.
 */
PJ_DEF(pj_status_t) pjmedia_opus_player_port_create(pj_pool_t *pool,
                                                    const char *filename,
                                                    unsigned ptime,
                                                    unsigned options,
                                                    pj_ssize_t buff_size,
                                                    pjmedia_port **p_port)
{
  struct opus_reader_port *orport;
  struct opus_tool_param *otp;
  pjmedia_audio_format_detail *ad;
  unsigned samples_per_frame;
  pj_str_t name;
  pj_status_t status = PJ_SUCCESS;

  /* Check arguments. */
  PJ_ASSERT_RETURN(pool && filename && p_port, PJ_EINVAL);

  /* Check the file really exists. */
  if (!pj_file_exists(filename)) {
  return PJ_ENOTFOUND;
  }

  /* Normalize ptime */
  if (ptime == 0)
  ptime = 20;

  /* Normalize buff_size */
  if (buff_size < 1) buff_size = PJMEDIA_FILE_PORT_BUFSIZE;

  /* Create fport instance. */
  orport = create_opus_port(pool);
  if (!orport) {
  return PJ_ENOMEM;
  }

  otp = PJ_POOL_ZALLOC_T(pool, struct opus_tool_param);
  PJ_ASSERT_RETURN(otp != NULL, PJ_ENOMEM);

  /* Get the file size. */
  orport->fsize = pj_file_size(filename);

  orport->rewind = 0;

  /**********************
   * opus-tool Start
   *********************/

  /* Set up some default value */
  otp->frange = NULL;
  otp->frame_size = 0;
  otp->st = NULL;
  otp->packet_count = 0;
  otp->total_links = 0;
  otp->stream_init = 0;
  otp->quiet = 0;
  otp->page_granule = 0;
  otp->link_out = 0;
  otp->close_in = 0;
  otp->eos = 0;
  otp->audio_size = 0;
  otp->loss_percent=-1;
  otp->manual_gain = 0;
  otp->channels = -1;
  otp->rate = 0;
  otp->wav_format = 0;
  otp->preskip = 0;
  otp->gran_offset = 0;
  otp->has_opus_stream = 0;
  otp->has_tags_packet = 0;
  otp->dither = 1;
  otp->gain = 1;
  otp->streams = 0;

  otp->inFile=(char *)filename;
  if(otp->rate == 0) otp->rate=48000;

  /* Open file */
  otp->fin = fopen_utf8(otp->inFile, "rb");
  if (!otp->fin)
  {
      perror(otp->inFile);
      quit(1);
  }
  otp->close_in=1;

  /* .opus files use the Ogg container to provide framing and timekeeping.
  * http://tools.ietf.org/html/draft-terriberry-oggopus
  * The easiest way to decode the Ogg container is to use libogg, so
  *  thats what we do here.
  * Using libogg is fairly straight forward-- you take your stream of bytes
  *  and feed them to ogg_sync_ and it periodically returns Ogg pages, you
  *  check if the pages belong to the stream you're decoding then you give
  *  them to libogg and it gives you packets. You decode the packets. The
  *  pages also provide timing information.*/
  ogg_sync_init(&otp->oy);

  orport->p_otp=otp;

  /* Normal opus file has two pages(exactly two packets) to contain header plus tag*/
  status = filedata_to_packet((pjmedia_port *)orport); //read header packet
  status = packet_to_frame((pjmedia_port *)orport);
  status = filedata_to_packet((pjmedia_port *)orport); //read tag packet
  status = packet_to_frame((pjmedia_port *)orport);

  /* Update port info */
  ad = pjmedia_format_get_audio_format_detail(&orport->base.info.fmt, 1);
  pj_strdup2(pool, &name, filename);
  samples_per_frame = ptime * otp->rate * otp->channels / 1000;
  pjmedia_port_info_init(&orport->base.info, &name, SIGNATURE,  otp->rate,
      otp->channels, BITS_PER_SAMPLE, samples_per_frame);

  /* Save the filename */
  otp->inFile = (char *)malloc(orport->base.info.name.slen + 1);
  memcpy(otp->inFile, orport->base.info.name.ptr, orport->base.info.name.slen);
  otp->inFile[orport->base.info.name.slen] = '\0';

  /* Done */
  *p_port = &orport->base;

  PJ_LOG(4,(THIS_FILE, 
	    "File player '%.*s' created: samp.rate=%d, ch=%d, "
	    "filesize=%luKB",
	    (int)orport->base.info.name.slen,
	    orport->base.info.name.ptr,
	    ad->clock_rate,
	    ad->channel_count,
	    (unsigned long)(orport->fsize / 1000)));

  return PJ_SUCCESS;
}


/*
 * Get frame from file.
 */
static pj_status_t opus_get_frame(pjmedia_port *this_port,
                                  pjmedia_frame *frame) {
  struct opus_reader_port *orport = (struct opus_reader_port*)this_port;
  struct opus_tool_param *otp=orport->p_otp;
  unsigned frame_size;
  char * fname;
  pj_status_t status;

  if(orport->eof && orport->rewind) {
    /* Set up some default value */
    orport->rewind = 0;
    orport->eof = 0;
    otp->frange = NULL;
    otp->frame_size = 0;
    otp->st = NULL;
    otp->packet_count = 0;
    otp->total_links = 0;
    otp->stream_init = 0;
    otp->quiet = 0;
    otp->page_granule = 0;
    otp->link_out = 0;
    otp->close_in = 0;
    otp->eos = 0;
    otp->audio_size = 0;
    otp->loss_percent=-1;
    otp->manual_gain = 0;
    otp->channels = -1;
    otp->wav_format = 0;
    otp->preskip = 0;
    otp->gran_offset = 0;
    otp->has_opus_stream = 0;
    otp->has_tags_packet = 0;
    otp->dither = 1;
    otp->gain = 1;
    otp->streams = 0;
    otp->rate=48000;
    otp->og.body = otp->og.header = NULL;
    otp->og.body_len = otp->og.header_len = (long) 0;
    otp->op.packet = NULL;
    otp->op.bytes = otp->op.b_o_s = otp->op.e_o_s = (long) 0;
    otp->op.granulepos = otp->op.packetno = (ogg_int64_t) 0;

    /* Check the file really exists. */
    if (!pj_file_exists(otp->inFile)) {
      return PJ_ENOTFOUND;
    }
    /* Open file */
    otp->fin = fopen_utf8(otp->inFile, "rb");
    if (!otp->fin)
    {
        perror(otp->inFile);
        quit(1);
    }
    otp->close_in=1;
    ogg_sync_init(&otp->oy);

    status = filedata_to_packet((pjmedia_port *)orport); //read header packet
    status = packet_to_frame((pjmedia_port *)orport);
    status = filedata_to_packet((pjmedia_port *)orport); //read tag packet
    status = packet_to_frame((pjmedia_port *)orport);

    return PJ_SUCCESS;
  }

  /* EOF is set */
  if (orport->eof) {
    frame->type = PJMEDIA_FRAME_TYPE_NONE;
    frame->size = 0;
    orport->rewind = 1;
    return PJ_EEOF;
  }

  frame_size = frame->size;

  /* Copy frame from buffer. */
  frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
  frame->timestamp.u64 = 0;
  orport->frame = frame;

  status = packet_to_frame(this_port);
  if (status == 1)
    orport->eof = 1;
  return PJ_SUCCESS;                                 
}



/*
 * Destroy port.
 */
static pj_status_t opus_on_destroy(pjmedia_port *this_port)
{
  pj_assert(this_port->info.signature == SIGNATURE);
    return PJ_SUCCESS;
}