#include <pjmedia/webm_stream.h>
#include <pjmedia/webm.h>
#include <pjmedia/errno.h>
#include <pj/assert.h>
#include <pj/file_access.h>
#include <pj/file_io.h>
#include <pj/log.h>
#include <pj/pool.h>
#include <pj/string.h>

#include <vpx/tools_common.h>
#include <pjmedia/nestegg.h>
#include <vorbis/codec.h>
#include <math.h>

#if defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

#define THIS_FILE   "webm_player.c"

#define SIGNATURE	    PJMEDIA_SIG_PORT_VID_WEBM_PLAYER  

#define VIDEO_CLOCK_RATE	90000

#define VORBIS_FOURCC_MASK (0x42524F56)

ogg_int16_t      convbuffer[4096]; /* take 8k out of the data segment, not the stack */

struct pjmedia_webm_streams
{
  unsigned     num_streams;
  pjmedia_port **streams;
};

typedef struct input_ctx {
  FILE           *infile;
  nestegg        *nestegg_ctx;
  nestegg_packet *pkt;
  unsigned int    chunk;
  unsigned int    chunks;
  unsigned int    video_track;
  unsigned int    audio_track;
}input_ctx;

typedef struct ogg_ctx {
  ogg_sync_state   oy; /* sync and verify incoming physical bitstream */
  ogg_stream_state os; /* take physical pages, weld into a logical
                          stream of packets */
  ogg_page         og; /* one Ogg bitstream page. Vorbis packets are inside */
  ogg_packet       op; /* one raw packet of data for decode */

  vorbis_info      vi; /* struct that stores all the static vorbis bitstream
                          settings */
  vorbis_comment   vc; /* struct that stores all the bitstream user comments */
  vorbis_dsp_state vd; /* central working state for the packet->PCM decoder */
  vorbis_block     vb; /* local working space for packet->PCM decode */
}ogg_ctx;

struct webm_reader_port
{
  pjmedia_port     base;
  unsigned         stream_id;
  unsigned	     options;
  pjmedia_format_id fmt_id;
  unsigned         usec_per_frame;
  pj_uint16_t	     bits_per_sample;
  pj_off_t	     fsize;
  pj_ssize_t       size_left;
  pj_uint8_t        *p_last;
  pj_timestamp     next_ts;
  input_ctx        input_ctx;
  ogg_ctx          ogg_ctx;
  pj_status_t	   (*cb)(pjmedia_port*, void*);
};

static pj_status_t webm_get_frame(pjmedia_port *this_port, 
			         pjmedia_frame *frame);
static pj_status_t webm_on_destroy(pjmedia_port *this_port);

/* 
 * Nestegg, MKV parser
 * Start
 */

static int
nestegg_read_cb(void *buffer, size_t length, void *userdata) {
  FILE *f = userdata;

  if (fread(buffer, 1, length, f) < length) {
    if (ferror(f))
      return -1;
    if (feof(f))
      return 0;
  }
  return 1;
}


static int
nestegg_seek_cb(int64_t offset, int whence, void *userdata) {
  switch (whence) {
    case NESTEGG_SEEK_SET:
      whence = SEEK_SET;
      break;
    case NESTEGG_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case NESTEGG_SEEK_END:
      whence = SEEK_END;
      break;
  };
  return fseek(userdata, (long)offset, whence) ? -1 : 0;
}


static int64_t
nestegg_tell_cb(void *userdata) {
  return ftell(userdata);
}

static int
webm_guess_framerate(struct input_ctx *input,
                     unsigned int     *fps_den,
                     unsigned int     *fps_num) {
  unsigned int i;
  uint64_t     tstamp = 0;
  uint64_t     tstamp_temp[2] = {0, 0};
  long long    tstamp_sub = 0;

  /* Check to see if we can seek before we parse any data. */
  if (nestegg_track_seek(input->nestegg_ctx, input->video_track, 0)) {
    fprintf(stderr,
            "WARNING: Failed to guess framerate (no Cues), set to 30fps.\n");
    *fps_num = 30;
    *fps_den = 1;
    return 0;
  }

  /* Guess the framerate. Read up to 1 second, or 50 video packets,
   * whichever comes first.
   * Here I found that some video miss a few frames in the begining of using nestegg_read_packet
   * and that leads to a wrong fps calculation,
   * so I modify for a little to due with such condition.
   */
  for (i = 0; tstamp < 1000000000 && i < 50;) {
    nestegg_packet *pkt;
    unsigned int track;

    if (nestegg_read_packet(input->nestegg_ctx, &pkt) <= 0)
      break;

    nestegg_packet_track(pkt, &track);
    if (track == input->video_track) {
      nestegg_packet_tstamp(pkt, &tstamp);
      if (i == 1)
        tstamp_temp[0] = tstamp; // Record the first 2 tstamp
      if (i == 2)
        tstamp_temp[1] = tstamp;
      i++;
    }

    nestegg_free_packet(pkt);
  }

  if (nestegg_track_seek(input->nestegg_ctx, input->video_track, 0))
    goto fail;

  // Modify for the special condition
  tstamp_sub = tstamp_temp[1] - (2 * tstamp_temp[0]);
  tstamp_sub = abs(tstamp_sub);
  
  if (tstamp_sub > 1000000) {// It seems we meet the special condition
    float diff = (float)(tstamp_temp[0] / 1000000) / (float)((tstamp_temp[1] -tstamp_temp[0]) / 1000000);
    i += ceil(diff) - 1;
  }

  *fps_num = (i - 1) * 1000000;
  *fps_den = (unsigned int)(tstamp / 1000);
  return 0;
fail:
  nestegg_destroy(input->nestegg_ctx);
  input->nestegg_ctx = NULL;
  rewind(input->infile);
  return 1;
}

static int
webm_base_parse(struct input_ctx *input,
             unsigned int     *video_fourcc,
             unsigned int     *audio_fourcc,
             unsigned int     *width,
             unsigned int     *height,
             unsigned int     *fps_den,
             unsigned int     *fps_num,
             unsigned int     *rate,
             unsigned int     *channels,
             unsigned int     *depth,
             unsigned int     *num_streams) {
  unsigned int i, n;
  int          track_type = -1;
  int          codec_id;

  nestegg_io io = {nestegg_read_cb, nestegg_seek_cb, nestegg_tell_cb, 0};
  nestegg_video_params video_params;
  nestegg_audio_params audio_params;

  io.userdata = input->infile;
  if (nestegg_init(&input->nestegg_ctx, io, NULL))
    goto fail;

  if (nestegg_track_count(input->nestegg_ctx, &n))
    goto fail;

  for (i = 0; i < n; i++) {
    track_type = nestegg_track_type(input->nestegg_ctx, i);

    if (track_type == NESTEGG_TRACK_VIDEO) {
      codec_id = nestegg_track_codec_id(input->nestegg_ctx, i);
      if (codec_id == NESTEGG_CODEC_VP8) {
        *video_fourcc = VP8_FOURCC_MASK;
      } else if (codec_id == NESTEGG_CODEC_VP9) {
        *video_fourcc = VP9_FOURCC_MASK;
      } else {
        fprintf(stderr, "Not VPx video, quitting.\n");
        exit(1);
      }

      input->video_track = i;

      if (nestegg_track_video_params(input->nestegg_ctx, i, &video_params))
        goto fail;

      *fps_den = 0;
      *fps_num = 0;
      *width = video_params.width;
      *height = video_params.height;

    } else if (track_type == NESTEGG_TRACK_AUDIO) {
      codec_id = nestegg_track_codec_id(input->nestegg_ctx, i);
      if (codec_id == NESTEGG_CODEC_VORBIS) 
        *audio_fourcc = VORBIS_FOURCC_MASK;
      else {
        fprintf(stderr, "Not VORBIS audio, quitting.\n");
        exit(1);
      }

      input->audio_track = i;

      if (nestegg_track_audio_params(input->nestegg_ctx, i, &audio_params))
        goto fail;

      *rate = (unsigned int)audio_params.rate;
      *channels = audio_params.channels;
      *depth = audio_params.depth;
    } else if (track_type < 0)
      goto fail;
  }

  *num_streams = n;
  return 0;
fail:
  input->nestegg_ctx = NULL;
  rewind(input->infile);
  return 1;
}

static struct webm_reader_port *create_webm_port(pj_pool_t *pool)
{
  const pj_str_t name = pj_str("file");
  struct webm_reader_port *port;

  port = PJ_POOL_ZALLOC_T(pool, struct webm_reader_port);
  if (!port)
  return NULL;

  /* Put in default values.
    * These will be overriden once the file is read.
    */
  pjmedia_port_info_init(&port->base.info, &name, SIGNATURE, 
			  8000, 1, 16, 80);

  port->base.get_frame = &webm_get_frame;
  port->base.on_destroy = &webm_on_destroy;

  return port;
}

/*
 * Create WEBM player port
 */
PJ_DEF(pj_status_t)
pjmedia_webm_player_create_streams(pj_pool_t *pool, const char *filename,
                                   unsigned options, pjmedia_webm_streams **p_streams)
{
  unsigned int            video_fourcc;
  unsigned int            audio_fourcc;
  unsigned int            width;
  unsigned int            height;
  unsigned int            fps_den;
  unsigned int            fps_num;
  unsigned int            rate;
  unsigned int            channels;
  unsigned int            depth;
  unsigned int            num_streams;
  const pjmedia_video_format_info *vfi;
  struct webm_reader_port *fport[PJMEDIA_WEBM_MAX_NUM_STREAMS];
  unsigned i, nstr = 0;
  pj_status_t status = PJ_SUCCESS;

  /* Check arguments. */
  PJ_ASSERT_RETURN(pool && filename && p_streams, PJ_EINVAL);

  /* Check the file really exists. */
  if (!pj_file_exists(filename)) {
    return PJ_ENOTFOUND;
  }

  /* Create fport instance. */
  fport[0] = create_webm_port(pool);
    if (!fport[0]) {
    return PJ_ENOMEM;
  }

  /* Get the file size. */
  fport[0]->fsize = pj_file_size(filename);

  /* Open file. */
  fport[0]->input_ctx.infile = fopen(filename, "rb");
  if (!fport[0]->input_ctx.infile) {
    fprintf(stderr, "Failed to open file '%s'",
           filename);
    return EXIT_FAILURE;
  }

  /* TODO: Check if the file is unsupported */

  /* Parse Webm. */
  status = webm_base_parse(&fport[0]->input_ctx, &video_fourcc, &audio_fourcc, &width, &height, &fps_den, &fps_num, &rate, &channels, &depth, &num_streams);
  if (status != PJ_SUCCESS)
    goto on_error;

  if (webm_guess_framerate(&fport[0]->input_ctx, &fps_den, &fps_num)) {
    fprintf(stderr, "Failed to guess framerate -- error parsing "
            "webm file?\n");
    return EXIT_FAILURE;
  }
  
  PJ_LOG(5, (THIS_FILE, "The WEBM file has %d streams.",
               num_streams));

  for (i = 0, nstr = 0; i < num_streams; i++) {
    pjmedia_format_id fmt_id;
    nestegg_io io = {nestegg_read_cb, nestegg_seek_cb, nestegg_tell_cb, 0};

    int               track_type = -1;

    track_type = nestegg_track_type(fport[0]->input_ctx.nestegg_ctx, i);
    if (track_type != NESTEGG_TRACK_AUDIO && track_type != NESTEGG_TRACK_VIDEO)
      continue;
    if (track_type == NESTEGG_TRACK_VIDEO) {
      if (video_fourcc == VP8_FOURCC_MASK)
        fmt_id = PJMEDIA_FORMAT_VP8;
      else {
        PJ_LOG(4, (THIS_FILE, "Unsupported video stream"));
        continue;
      }
    } else {
      if (audio_fourcc == VORBIS_FOURCC_MASK)
        fmt_id = PJMEDIA_FORMAT_PCM;
      else {
        PJ_LOG(4, (THIS_FILE, "Unsupported audio stream"));
        continue;
      }
    }
    
    if (nstr > 0) {
      /* Create fport instance. */
      fport[nstr] = create_webm_port(pool);
      if (!fport[nstr]) {
        status = PJ_ENOMEM;
        goto on_error;
      }

      /* Open file. */
      fport[nstr]->input_ctx.infile = fopen(filename, "rb");
      if (!fport[nstr]->input_ctx.infile) {
        fprintf(stderr, "Failed to open file '%s'",
               filename);
        return EXIT_FAILURE;
      }
      io.userdata = fport[nstr]->input_ctx.infile;
      if (nestegg_init(&fport[nstr]->input_ctx.nestegg_ctx, io, NULL)) {
        fport[nstr]->input_ctx.nestegg_ctx = NULL;
        rewind(fport[nstr]->input_ctx.infile);
        return 1;
      }
      fport[nstr]->input_ctx.video_track = fport[0]->input_ctx.video_track;
      fport[nstr]->input_ctx.audio_track = fport[0]->input_ctx.audio_track;
      
    }

    fport[nstr]->stream_id = i;
    fport[nstr]->fmt_id = fmt_id;

    nstr++;
  }

  if (nstr == 0) {
    status = PJMEDIA_EAVIUNSUPP;
    goto on_error;
  }

  for (i = 0; i < nstr; i++) {
    /* Initialize. */
    fport[i]->options = options;
    fport[i]->fsize = fport[0]->fsize;

    if (fport[i]->fmt_id == PJMEDIA_FORMAT_VP8) {
      fport[i]->usec_per_frame = fps_den/(fps_num / 1000000);

      pjmedia_format_init_video(&fport[i]->base.info.fmt,
                                          fport[i]->fmt_id,
                                          width,
                                          height,
                                          fps_num/1000,
                                          fps_den/1000);
    } else {
      /* Due with ogg vorbis's first 3 packet. */
      unsigned int codec_data_count;
      unsigned char *codec_data;
      size_t length;
      int result;

      vorbis_info_init(&fport[i]->ogg_ctx.vi);
      vorbis_comment_init(&fport[i]->ogg_ctx.vc);
      // parse private data (the first packet - header)
      nestegg_track_codec_data(fport[i]->input_ctx.nestegg_ctx, fport[i]->input_ctx.audio_track, 0, &codec_data, &length);
      fport[i]->ogg_ctx.op.packet = codec_data;
      fport[i]->ogg_ctx.op.bytes = length;
      fport[i]->ogg_ctx.op.packetno = 0;
      fport[i]->ogg_ctx.op.b_o_s = 256;
      if(vorbis_synthesis_headerin(&fport[i]->ogg_ctx.vi, &fport[i]->ogg_ctx.vc, &fport[i]->ogg_ctx.op)<0){ 
        /* error case; not a vorbis header */
        fprintf(stderr,"This Ogg bitstream does not contain Vorbis "
                "audio data.\n");
        exit(1);
      }

      // parse private data (the second and the third packet - comment and codebook)
      while (fport[i]->ogg_ctx.op.packetno < 2) {
        nestegg_track_codec_data(fport[i]->input_ctx.nestegg_ctx, fport[i]->input_ctx.audio_track, fport[i]->ogg_ctx.op.packetno+1, &codec_data, &length);
        fport[i]->ogg_ctx.op.packet = codec_data;
        fport[i]->ogg_ctx.op.bytes = length;
        fport[i]->ogg_ctx.op.packetno++;
        fport[i]->ogg_ctx.op.b_o_s = 0;
        result = vorbis_synthesis_headerin(&fport[i]->ogg_ctx.vi, &fport[i]->ogg_ctx.vc, &fport[i]->ogg_ctx.op);
        if(result<0){
          fprintf(stderr,"Corrupt secondary header.  Exiting.\n");
          exit(1);
        }
      }

      /* Throw the comments plus a few lines about the bitstream we're
         decoding */
      {
        char **ptr=fport[i]->ogg_ctx.vc.user_comments;
        while(*ptr){
          fprintf(stderr,"%s\n",*ptr);
          ++ptr;
        }
        fprintf(stderr,"\nBitstream is %d channel, %ldHz\n",fport[i]->ogg_ctx.vi.channels,fport[i]->ogg_ctx.vi.rate);
        fprintf(stderr,"Encoded by: %s\n\n",fport[i]->ogg_ctx.vc.vendor);
      }

      /* OK, got and parsed all three headers. Initialize the Vorbis
         packet->PCM decoder. */
      if(vorbis_synthesis_init(&fport[i]->ogg_ctx.vd,&fport[i]->ogg_ctx.vi)==0) /* central decode state */
        vorbis_block_init(&fport[i]->ogg_ctx.vd,&fport[i]->ogg_ctx.vb);          /* local state for most of the decode
                                                                                    so multiple block decodes can
                                                                                    proceed in parallel. We could init
                                                                                    multiple vorbis_block structures
                                                                                    for vd here */
      else 
        fprintf(stderr,"Error: Corrupt header during playback initialization.\n");

      fport[i]->bits_per_sample = depth;
      fport[i]->usec_per_frame = fps_den/(fps_num / 1000000);
      pjmedia_format_init_audio(&fport[i]->base.info.fmt,
                                fport[i]->fmt_id,
                                rate,
                                channels,
                                16,  // We deocode the vorbis into PCM(16 bits per sample)
                                20000,
                                //fport[i]->usec_per_frame,
                                rate * 16 * channels, // The PCM data's bps
                                rate * 16 * channels);
                                //fport[i]->ogg_ctx.vi.bitrate_nominal,  // The compressed data's bps, maybe useless
                                //fport[i]->ogg_ctx.vi.bitrate_nominal);
                                


    } 
    pj_strdup2(pool, &fport[i]->base.info.name, filename);
  }

  /* Done. */
  *p_streams = pj_pool_alloc(pool, sizeof(pjmedia_webm_streams));
  (*p_streams)->num_streams = nstr;
  (*p_streams)->streams = pj_pool_calloc(pool, (*p_streams)->num_streams,
                                          sizeof(pjmedia_port *));
  for (i = 0; i < nstr; i++)
      (*p_streams)->streams[i] = &fport[i]->base;

  PJ_LOG(4,(THIS_FILE, 
	    "WEBM file player '%.*s' created with "
	    "%d media ports",
	    (int)fport[0]->base.info.name.slen,
	    fport[0]->base.info.name.ptr,
            (*p_streams)->num_streams));

  return PJ_SUCCESS;

on_error:
  fport[0]->base.on_destroy(&fport[0]->base);
  for (i = 1; i < nstr; i++)
      fport[i]->base.on_destroy(&fport[i]->base);
  return status;
}

PJ_DEF(unsigned)
pjmedia_webm_streams_get_num_streams(pjmedia_webm_streams *streams)
{
    pj_assert(streams);
    return streams->num_streams;
}

PJ_DEF(pjmedia_webm_stream *)
pjmedia_webm_streams_get_stream(pjmedia_webm_streams *streams,
                               unsigned idx)
{
    pj_assert(streams);
    return (idx >=0 && idx < streams->num_streams ?
            streams->streams[idx] : NULL);
}

PJ_DEF(pjmedia_webm_stream *)
pjmedia_webm_streams_get_stream_by_media(pjmedia_webm_streams *streams,
                                        unsigned start_idx,
                                        pjmedia_type media_type)
{
   unsigned i;

  pj_assert(streams);
  for (i = start_idx; i < streams->num_streams; i++)
    if (streams->streams[i]->info.fmt.type == media_type)
      return streams->streams[i];
  return NULL;
}

/*
 * Get frame from file.
 */

static int read_video_frame(struct input_ctx      *input,
                      uint8_t               **buf,
                      size_t                *buf_sz,
                      size_t                *buf_alloc_sz) {
  nestegg_io io = {nestegg_read_cb, nestegg_seek_cb, nestegg_tell_cb, 0};
  int result;
  if (input->chunk >= input->chunks) {
    unsigned int track;
    do {
      /* End of this packet, get another. */
      if (input->pkt)
        nestegg_free_packet(input->pkt);
      result = nestegg_read_packet(input->nestegg_ctx, &input->pkt);
      if (result == 0) {
        input->nestegg_ctx = NULL;
        rewind(input->infile);
        io.userdata = input->infile;
        if(nestegg_init(&input->nestegg_ctx, io, NULL)) {
          input->nestegg_ctx = NULL;
          rewind(input->infile);
          return 1;
        }
        result = nestegg_read_packet(input->nestegg_ctx, &input->pkt);
      }
      if (result <= 0
          || nestegg_packet_track(input->pkt, &track))
        return 1;
    } while (track != input->video_track);

    if (nestegg_packet_count(input->pkt, &input->chunks))
      return 1;
    input->chunk = 0;
  }

  if (nestegg_packet_data(input->pkt, input->chunk, buf, buf_sz))
    return 1;
  input->chunk++;

  return 0;
}

static int read_audio_frame(struct input_ctx *input,
                            struct ogg_ctx *ogg,
                            uint8_t **buf,
                            size_t  *buf_sz,
                            size_t  *buf_alloc_sz) {
  nestegg_io io = {nestegg_read_cb, nestegg_seek_cb, nestegg_tell_cb, 0};
  int i;
  float **pcm;
  int samples;
  int result;
  int convsize=4096;
  
  convsize=4096/(ogg->vi.channels);

  
  
  if (input->chunk >= input->chunks) {
    unsigned int track;
    do {
      /* End of this packet, get another. */
      if (input->pkt)
        nestegg_free_packet(input->pkt);
      result = nestegg_read_packet(input->nestegg_ctx, &input->pkt);
      if (result == 0) {
        input->nestegg_ctx = NULL;
        rewind(input->infile);
        io.userdata = input->infile;
        if(nestegg_init(&input->nestegg_ctx, io, NULL)) {
          input->nestegg_ctx = NULL;
          rewind(input->infile);
          return 1;
        }
        ogg->op.packetno = 2;
        result = nestegg_read_packet(input->nestegg_ctx, &input->pkt);
      }
      if (result <= 0
          || nestegg_packet_track(input->pkt, &track))
        return 1;
    } while (track != input->audio_track);

    if (nestegg_packet_count(input->pkt, &input->chunks))
      return 1;
    input->chunk = 0;
  }

  if (nestegg_packet_data(input->pkt, input->chunk, buf, buf_sz))
    return 1;

  ogg->op.packet = (unsigned char*)*buf;
  ogg->op.bytes = *buf_sz;
  ogg->op.packetno++;

  if(vorbis_synthesis(&ogg->vb,&ogg->op)==0) /* test for success! */
    vorbis_synthesis_blockin(&ogg->vd,&ogg->vb);

  /* 
                   
  **pcm is a multichannel float vector.  In stereo, for
  example, pcm[0] is left, and pcm[1] is right.  samples is
  the size of each channel.  Convert the float values
  (-1.<=range<=1.) to whatever PCM format and write it out */
                
  while((samples=vorbis_synthesis_pcmout(&ogg->vd,&pcm))>0){
    int j;
    int clipflag=0;
    int bout=(samples<convsize?samples:convsize);
                  
    /* convert floats to 16 bit signed ints (host order) and
        interleave */
    for(i=0;i<ogg->vi.channels;i++){
      ogg_int16_t *ptr=convbuffer+i;
      float  *mono=pcm[i];
      for(j=0;j<bout;j++){
#if 1
        int val=floor(mono[j]*32767.f+.5f);
#else /* optional dither */
        int val=mono[j]*32767.f+drand48()-0.5f;
#endif
        /* might as well guard against clipping */
        if(val>32767){
          val=32767;
          clipflag=1;
        }
        if(val<-32768){
          val=-32768;
          clipflag=1;
        }
        *ptr=val;
        ptr+=ogg->vi.channels;
      }
    }
                  
    if(clipflag)
      fprintf(stderr,"Clipping in frame %ld\n",(long)(ogg->vd.sequence));
                  
    *buf = (uint8_t*)convbuffer;
    *buf_sz = (size_t)2*ogg->vi.channels * bout;
                  
    vorbis_synthesis_read(&ogg->vd,bout); /* tell libvorbis how
                                        many samples we
                                        actually consumed */
    input->chunk++;
    return 0;
  }
  *buf_sz = 0;
  input->chunk++;

  return 0;

}

static pj_status_t webm_get_frame(pjmedia_port *this_port, 
			         pjmedia_frame *frame)
{
  struct webm_reader_port *fport = (struct webm_reader_port*)this_port;
  pj_status_t status;
  pj_ssize_t size_read = 0, size_to_read = 0;
  uint8_t *buf = NULL;
  size_t buf_sz = 0, buf_alloc_sz = 0;
  
  pj_assert(fport->base.info.signature == SIGNATURE);

  /*
   * TODO: encounter end of file ?
   */
  frame->type = (fport->base.info.fmt.type == PJMEDIA_TYPE_VIDEO ?
                 PJMEDIA_FRAME_TYPE_VIDEO : PJMEDIA_FRAME_TYPE_AUDIO);

  if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO) {
  /* Fill frame buffer. */
    size_to_read = frame->size;

    do {
      
      if (fport->size_left > 0 && fport->size_left < size_to_read) {
        pj_memcpy(frame->buf, fport->p_last, fport->size_left);
        size_to_read -= fport->size_left;
        size_read += fport->size_left;
        fport->size_left = 0;
        
      }

      /* Read new data. */
      if (fport->size_left == 0) {
        fport->p_last = (pj_uint8_t *)convbuffer;
        do {
          status = read_audio_frame(&fport->input_ctx, &fport->ogg_ctx, &buf, &buf_sz, &buf_alloc_sz);
        } while (buf_sz == 0);
        fport->size_left = buf_sz;
      }

      if (size_to_read > fport->size_left) {
        pj_memcpy((char*)frame->buf + size_read, fport->p_last, fport->size_left);
        size_read += fport->size_left;
        size_to_read -= fport->size_left;
        fport->size_left = 0;
        continue;
      }
      pj_memcpy((char*)frame->buf + frame->size - size_to_read, fport->p_last, size_to_read);
      fport->size_left -= size_to_read;
      fport->p_last += size_to_read;
      break;
    } while(1);
  } 

  else {  // Video
    status = read_video_frame(&fport->input_ctx, &buf, &buf_sz, &buf_alloc_sz);
    if (status != PJ_SUCCESS)
      goto on_error2;
    //frame->buf = malloc(buf_sz);
    //memcpy(frame->buf, buf, buf_sz);    
    pj_memcpy(frame->buf, buf, buf_sz);
    frame->size = buf_sz;
  }

  frame->timestamp.u64 = fport->next_ts.u64;
  if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO) {
    if (fport->usec_per_frame) {
      fport->next_ts.u64 += (fport->usec_per_frame *
                             fport->base.info.fmt.det.aud.clock_rate /
                             1000000);
    } else {
      fport->next_ts.u64 += (frame->size *
                             fport->base.info.fmt.det.aud.clock_rate /
                             (fport->base.info.fmt.det.aud.avg_bps / 8));
    }
  } else {
    if (fport->usec_per_frame) {
	  fport->next_ts.u64 += (fport->usec_per_frame * VIDEO_CLOCK_RATE /
				  1000000);
    } else {
	  fport->next_ts.u64 += (frame->size * VIDEO_CLOCK_RATE /
				  (fport->base.info.fmt.det.vid.avg_bps / 8));
    }
  }

  return PJ_SUCCESS;

on_error2:
  return status;
}

/*
 * Destroy port.
 */
static pj_status_t webm_on_destroy(pjmedia_port *this_port)
{
  struct webm_reader_port *fport = (struct webm_reader_port*) this_port;
  pj_assert(this_port->info.signature == SIGNATURE);

  if (fport->input_ctx.infile)
    fclose(fport->input_ctx.infile);
  return PJ_SUCCESS;
}

#endif /* PJMEDIA_HAS_VIDEO */