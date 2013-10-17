#include <pjmedia/opus_port.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/pool.h>
#include <pjmedia/errno.h>
#include <pjmedia/lpc.h>
#include <pjmedia/unicode_support.h>
#include <pjmedia/stack_alloc.h>
#include <pjmedia/diag_range.h>

#include <time.h>

#define THIS_FILE   "opus_writer.c"
#define SIGNATURE   PJMEDIA_SIG_PORT_OPUS_WRITER
#define PACKAGE_NAME  "pjsip_opus-tools"
#define PACKAGE_VERSION "0.1.7" 

#define VG_UNDEF(x,y)
#define VG_CHECK(x,y)

struct opus_port {
  pjmedia_port    base;
  pj_uint16_t     bytes_per_sample;
  pjmedia_frame   *tempframe;
  pjmedia_frame   *frame;
  pj_oshandle_t   fd;
  pj_size_t       cb_size;
  pj_status_t     (*cb)(pjmedia_port*, void*);
  struct opus_tool_param *p_otp;
};

struct opus_tool_param {
  OpusMSEncoder      *st;
  const char         *opus_version;
  unsigned char      *packet;
  float              *input;
  /*I/O*/
  oe_enc_opt         inopt;
  char               *outFile;
  char               *range_file;
  FILE               *fout;
  FILE               *frange;
  ogg_stream_state   os;
  ogg_page           og;
  ogg_packet         op;
  ogg_int64_t        last_granulepos;
  ogg_int64_t        enc_granulepos;
  ogg_int64_t        original_samples;
  ogg_int32_t        id;
  int                last_segments;
  int                eos;
  OpusHeader         header;
  char               ENCODER_string[1024];
  /*Counters*/
  opus_int64         nb_encoded;
  opus_int64         bytes_written;
  opus_int64         pages_out;
  opus_int64         total_bytes;
  opus_int64         total_samples;
  opus_int32         nbBytes;
  opus_int32         nb_samples;
  opus_int32         peak_bytes;
  opus_int32         min_bytes;
  time_t             start_time;
  /*Settings*/
  int                max_frame_bytes;
  opus_int32         bitrate;
  opus_int32         rate;
  opus_int32         coding_rate;
  opus_int32         frame_size;
  int                chan;
  int                with_hard_cbr;
  int                with_cvbr;
  int                expect_loss;
  int                complexity;
  int                downmix;
  int                *opt_ctls_ctlval;
  int                opt_ctls;
  int                max_ogg_delay; /*48kHz samples*/
  int                comment_padding;
  int                serialno;
  opus_int32         lookahead;
};

typedef struct {
    audio_read_func real_reader;
    void *real_readdata;
    ogg_int64_t *original_samples;
    int channels;
    int lpc_ptr;
    int *extra_samples;
    float *lpc_out;
} padder;

static pj_status_t opus_put_frame(pjmedia_port *this_port, 
				pjmedia_frame *frame);
static pj_status_t opus_get_frame(pjmedia_port *this_port, 
				pjmedia_frame *frame);
static pj_status_t opus_on_destroy(pjmedia_port *this_port);

static void comment_init(char **comments, int* length, const char *vendor_string);
static void comment_pad(char **comments, int* length, int amount);
static int oe_write_page(ogg_page *page, FILE *fp);

/*Write an Ogg page to a file pointer*/
static __inline int oe_write_page(ogg_page *page, FILE *fp)
{
   int written;
   written=fwrite(page->header,1,page->header_len, fp);
   written+=fwrite(page->body,1,page->body_len, fp);
   return written;
}

#define IMIN(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum int value.   */
#define IMAX(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum int value.   */

/*
 * Create opus writer port.
 */
PJ_DEF(pj_status_t) pjmedia_opus_writer_port_create( pj_pool_t *pool,
						     const char *filename,
						     unsigned sampling_rate,
						     unsigned channel_count,
						     unsigned samples_per_frame,
						     unsigned bits_per_sample,
						     unsigned flags,
						     pj_ssize_t buff_size,
						     pjmedia_port **p_port ) {
  struct opus_port *oport;
  struct opus_tool_param *otp;
  pj_str_t         name;
  pj_status_t      status;
  pjmedia_frame    tempframe;
  int              i,ret;

  /* Check arguments. */
  PJ_ASSERT_RETURN(pool && filename && p_port, PJ_EINVAL);

  /* Only supports 16bits per sample for now. */
  PJ_ASSERT_RETURN(bits_per_sample == 16, PJ_EINVAL);

  /* Create file port instance. */
  oport = PJ_POOL_ZALLOC_T(pool, struct opus_port);
  PJ_ASSERT_RETURN(oport != NULL, PJ_ENOMEM);

  /* Initialize port info. */
  pj_strdup2(pool, &name, filename);
  pjmedia_port_info_init(&oport->base.info, &name, SIGNATURE,
      sampling_rate, channel_count, bits_per_sample,
      samples_per_frame);

  oport->base.get_frame = &opus_get_frame;
  oport->base.put_frame = &opus_put_frame;
  oport->base.on_destroy = &opus_on_destroy;
  oport->bytes_per_sample = 2;


  /**********************
   * opus-tool Start
   *********************/
  otp = PJ_POOL_ZALLOC_T(pool, struct opus_tool_param);
  PJ_ASSERT_RETURN(otp != NULL, PJ_ENOMEM);

  /* Initialize some default parameters. */
  otp->opt_ctls_ctlval=NULL;
  otp->frange=NULL;
  otp->range_file=NULL;
  otp->inopt.channels=channel_count;
  otp->inopt.rate=otp->coding_rate=otp->rate=sampling_rate;

  /* Some default value */
  otp->frame_size=960;
  otp->bitrate=-1;
  otp->with_hard_cbr=0;
  otp->with_cvbr=0;
  otp->complexity=10;
  otp->expect_loss=0;
  otp->lookahead=0;
  otp->bytes_written=0;
  otp->pages_out=0;
  otp->comment_padding=512;
  otp->eos=0;
  otp->nb_samples=-1;
  otp->id=-1;
  otp->nb_encoded=0;
  otp->enc_granulepos=0;
  otp->total_bytes=0;
  otp->peak_bytes=0;
  otp->max_ogg_delay=48000;

  /* 0 dB gain is recommended unless you know what you're doing */
  otp->inopt.gain=0;
  otp->inopt.samplesize=(oport->bytes_per_sample)*8;
  otp->inopt.endianness=0;
  otp->inopt.rawmode=0;
  otp->inopt.ignorelength=0;
  otp->inopt.copy_comments=1;
  otp->inopt.skip=0;

  otp->start_time = time(NULL);
  srand(((_getpid()&65535)<<15)^(otp->start_time));
  otp->serialno=rand();

  otp->opus_version="libopus 1.0.3";
  /* Vendor string should just be the encoder library,
     the ENCODER comment specifies the tool used. */
  comment_init(&otp->inopt.comments, &otp->inopt.comments_length, otp->opus_version);
  snprintf(otp->ENCODER_string, sizeof(otp->ENCODER_string), "opusenc from %s %s", PACKAGE_NAME, PACKAGE_VERSION);
  comment_add(&otp->inopt.comments, &otp->inopt.comments_length, "ENCODER", otp->ENCODER_string);

  otp->outFile=(char *)filename;
  otp->rate=otp->inopt.rate;
  otp->chan=otp->inopt.channels;
  otp->inopt.skip=0;

  if(otp->rate>24000)otp->coding_rate=48000;
  else if(otp->rate>16000)otp->coding_rate=24000;
  else if(otp->rate>12000)otp->coding_rate=16000;
  else if(otp->rate>8000)otp->coding_rate=12000;
  else otp->coding_rate=8000;

  otp->frame_size=otp->frame_size/(48000/otp->coding_rate);

  /*OggOpus headers*/ /*FIXME: broke forcemono*/
  otp->header.channels=otp->chan;
  otp->header.channel_mapping=otp->header.channels>8?255:otp->chan>2;
  otp->header.input_sample_rate=otp->rate;
  otp->header.gain=otp->inopt.gain;

  /* Initialize OPUS encoder */
  /* Framesizes <10ms can only use the MDCT modes, so we switch on RESTRICTED_LOWDELAY
     to save the extra 2.5ms of codec lookahead when we'll be using only small frames. */
  otp->st=opus_multistream_surround_encoder_create(otp->coding_rate, otp->chan, otp->header.channel_mapping, &otp->header.nb_streams, &otp->header.nb_coupled,
    otp->header.stream_map, otp->frame_size<480/(48000/otp->coding_rate)?OPUS_APPLICATION_RESTRICTED_LOWDELAY:OPUS_APPLICATION_AUDIO, &ret);

  if(ret!=OPUS_OK) {
    fprintf(stderr, "Error cannot create encoder: %s\n", opus_strerror(ret));
    exit(1);
  }

  otp->min_bytes=otp->max_frame_bytes=(1275*3+7)*otp->header.nb_streams;
  otp->packet=(unsigned char*)malloc(sizeof(unsigned char)*otp->max_frame_bytes);
  if(otp->packet==NULL) {
    fprintf(stderr, "Error allocating packet buffer.\n");
    exit(1);
  }

  if(otp->bitrate<0) {
    /* Lower default rate for sampling rates [8000-44100) by a factor of (rate+16k)/(64k) */
    otp->bitrate=((64000*otp->header.nb_streams+32000*otp->header.nb_coupled)*
        (IMIN(48,IMAX(8,((otp->rate<44100?otp->rate:48000)+1000)/1000))+16)+32)>>6;
  }

  if(otp->bitrate>(1024000*otp->chan)||otp->bitrate<500){
    fprintf(stderr,"Error: Bitrate %d bits/sec is insane.\nDid you mistake bits for kilobits?\n",otp->bitrate);
    fprintf(stderr,"--bitrate values from 6-256 kbit/sec per channel are meaningful.\n");
    exit(1);
  }
  otp->bitrate=IMIN(otp->chan*256000,otp->bitrate);

  ret=opus_multistream_encoder_ctl(otp->st, OPUS_SET_BITRATE(otp->bitrate));

  if(ret!=OPUS_OK){
    fprintf(stderr,"Error OPUS_SET_BITRATE returned: %s\n",opus_strerror(ret));
    exit(1);
  }

  ret=opus_multistream_encoder_ctl(otp->st, OPUS_SET_VBR(!otp->with_hard_cbr));
  if(ret!=OPUS_OK){
    fprintf(stderr,"Error OPUS_SET_VBR returned: %s\n",opus_strerror(ret));
    exit(1);
  }

  if(!otp->with_hard_cbr){
    ret=opus_multistream_encoder_ctl(otp->st, OPUS_SET_VBR_CONSTRAINT(otp->with_cvbr));
    if(ret!=OPUS_OK){
      fprintf(stderr,"Error OPUS_SET_VBR_CONSTRAINT returned: %s\n",opus_strerror(ret));
      exit(1);
    }
  }

  ret=opus_multistream_encoder_ctl(otp->st, OPUS_SET_COMPLEXITY(otp->complexity));
  if(ret!=OPUS_OK){
    fprintf(stderr,"Error OPUS_SET_COMPLEXITY returned: %s\n",opus_strerror(ret));
    exit(1);
  }

  ret=opus_multistream_encoder_ctl(otp->st, OPUS_SET_PACKET_LOSS_PERC(otp->expect_loss));
  if(ret!=OPUS_OK){
    fprintf(stderr,"Error OPUS_SET_PACKET_LOSS_PERC returned: %s\n",opus_strerror(ret));
    exit(1);
  }

#ifdef OPUS_SET_LSB_DEPTH
  ret=opus_multistream_encoder_ctl(otp->st, OPUS_SET_LSB_DEPTH(IMAX(8,IMIN(24,otp->inopt.samplesize))));
  if(ret!=OPUS_OK){
    fprintf(stderr,"Warning OPUS_SET_LSB_DEPTH returned: %s\n",opus_strerror(ret));
  }
#endif

  /*This should be the last set of CTLs, except the lookahead get, so it can override the defaults.*/
  for(i=0;i<otp->opt_ctls;i++){
    int target=otp->opt_ctls_ctlval[i*3];
    if(target==-1){
      ret=opus_multistream_encoder_ctl(otp->st,otp->opt_ctls_ctlval[i*3+1],otp->opt_ctls_ctlval[i*3+2]);
      if(ret!=OPUS_OK){
        fprintf(stderr,"Error opus_multistream_encoder_ctl(st,%d,%d) returned: %s\n",otp->opt_ctls_ctlval[i*3+1],otp->opt_ctls_ctlval[i*3+2],opus_strerror(ret));
        exit(1);
      }
    }else if(target<otp->header.nb_streams){
      OpusEncoder *oe;
      opus_multistream_encoder_ctl(otp->st,OPUS_MULTISTREAM_GET_ENCODER_STATE(i,&oe));
      ret=opus_encoder_ctl(oe, otp->opt_ctls_ctlval[i*3+1],otp->opt_ctls_ctlval[i*3+2]);
      if(ret!=OPUS_OK){
        fprintf(stderr,"Error opus_encoder_ctl(st[%d],%d,%d) returned: %s\n",target,otp->opt_ctls_ctlval[i*3+1],otp->opt_ctls_ctlval[i*3+2],opus_strerror(ret));
        exit(1);
      }
    }else{
      fprintf(stderr,"Error --set-ctl-int target stream %d is higher than the maximum stream number %d.\n",target,otp->header.nb_streams-1);
      exit(1);
    }
  }

  /*We do the lookahead check late so user CTLs can change it*/
  ret=opus_multistream_encoder_ctl(otp->st, OPUS_GET_LOOKAHEAD(&otp->lookahead));
  if(ret!=OPUS_OK){
    fprintf(stderr,"Error OPUS_GET_LOOKAHEAD returned: %s\n",opus_strerror(ret));
    exit(1);
  }
  otp->inopt.skip+=otp->lookahead;
  /*Regardless of the rate we're coding at the ogg timestamping/skip is
    always timed at 48000.*/
  otp->header.preskip=otp->inopt.skip*(48000./otp->coding_rate);
  /* Extra samples that need to be read to compensate for the pre-skip */
  otp->inopt.extraout=(int)otp->header.preskip*(otp->rate/48000.);

  otp->fout=fopen_utf8(otp->outFile, "wb");
  if(!otp->fout) {
    perror(otp->outFile);
    exit(1);
  }

  /*Initialize Ogg stream struct*/
  if(ogg_stream_init(&otp->os, otp->serialno)==-1){
    fprintf(stderr,"Error: stream init failed\n");
    exit(1);
  }

  /*Write header*/
  {
    unsigned char header_data[100];
    int packet_size=opus_header_to_packet(&otp->header, header_data, 100);
    otp->op.packet=header_data;
    otp->op.bytes=packet_size;
    otp->op.b_o_s=1;
    otp->op.e_o_s=0;
    otp->op.granulepos=0;
    otp->op.packetno=0;
    ogg_stream_packetin(&otp->os, &otp->op);

    while((ret=ogg_stream_flush(&otp->os, &otp->og))){
      if(!ret)break;
      ret=oe_write_page(&otp->og, otp->fout);
      if(ret!=otp->og.header_len+otp->og.body_len){
        fprintf(stderr,"Error: failed writing header to output stream\n");
        exit(1);
      }
      otp->bytes_written+=ret;
      otp->pages_out++;
    }

    comment_pad(&otp->inopt.comments, &otp->inopt.comments_length, otp->comment_padding);
    otp->op.packet=(unsigned char *)otp->inopt.comments;
    otp->op.bytes=otp->inopt.comments_length;
    otp->op.b_o_s=0;
    otp->op.e_o_s=0;
    otp->op.granulepos=0;
    otp->op.packetno=1;
    ogg_stream_packetin(&otp->os, &otp->op);
  }

  /* writing the rest of the opus header packets */
  while((ret=ogg_stream_flush(&otp->os, &otp->og))){
    if(!ret)break;
    ret=oe_write_page(&otp->og, otp->fout);
    if(ret!=otp->og.header_len + otp->og.body_len){
      fprintf(stderr,"Error: failed writing header to output stream\n");
      exit(1);
    }
    otp->bytes_written+=ret;
    otp->pages_out++;
  }

  free(otp->inopt.comments);

  otp->input=(float*)malloc(sizeof(float)*otp->frame_size*otp->chan);
  if(otp->input==NULL){
    fprintf(stderr,"Error: couldn't allocate sample buffer.\n");
    exit(1);
  }

  oport->p_otp=otp;

  /**********************
   * opus-tool End
   *********************/
  oport->tempframe=(pjmedia_frame *)pj_pool_alloc(pool, sizeof(pjmedia_frame));
  oport->tempframe->buf=pj_pool_alloc(pool, (otp->frame_size)*otp->chan*bits_per_sample/8);

  /* Done. */
  *p_port = &oport->base;

  PJ_LOG(4,(THIS_FILE,
    "File writer '%.*s' created: samp.rate=%d, channel=%d",
    (int)oport->base.info.name.slen,
    oport->base.info.name.ptr,
    PJMEDIA_PIA_SRATE(&oport->base.info),
    PJMEDIA_PIA_CCNT(&oport->base.info)));
  
  return PJ_SUCCESS;
}

/*
 Comments will be stored in the Vorbis style.
 It is describled in the "Structure" section of
    http://www.xiph.org/ogg/vorbis/doc/v-comment.html

 However, Opus and other non-vorbis formats omit the "framing_bit".

The comment header is decoded as follows:
  1) [vendor_length] = read an unsigned integer of 32 bits
  2) [vendor_string] = read a UTF-8 vector as [vendor_length] octets
  3) [user_comment_list_length] = read an unsigned integer of 32 bits
  4) iterate [user_comment_list_length] times {
     5) [length] = read an unsigned integer of 32 bits
     6) this iteration's user comment = read a UTF-8 vector as [length] octets
     }
  7) done.
*/
#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
                           ((buf[base+2]<<16)&0xff0000)| \
                           ((buf[base+1]<<8)&0xff00)| \
                           (buf[base]&0xff))
#define writeint(buf, base, val) do{ buf[base+3]=((val)>>24)&0xff; \
                                     buf[base+2]=((val)>>16)&0xff; \
                                     buf[base+1]=((val)>>8)&0xff; \
                                     buf[base]=(val)&0xff; \
                                 }while(0)

static void comment_init(char **comments, int* length, const char *vendor_string)
{
  /*The 'vendor' field should be the actual encoding library used.*/
  int vendor_length=strlen(vendor_string);
  int user_comment_list_length=0;
  int len=8+4+vendor_length+4;
  char *p=(char*)malloc(len);
  if(p==NULL){
    fprintf(stderr, "malloc failed in comment_init()\n");
    exit(1);
  }
  memcpy(p, "OpusTags", 8);
  writeint(p, 8, vendor_length);
  memcpy(p+12, vendor_string, vendor_length);
  writeint(p, 12+vendor_length, user_comment_list_length);
  *length=len;
  *comments=p;
}

void comment_add(char **comments, int* length, char *tag, char *val)
{
  char* p=*comments;
  int vendor_length=readint(p, 8);
  int user_comment_list_length=readint(p, 8+4+vendor_length);
  int tag_len=(tag?strlen(tag)+1:0);
  int val_len=strlen(val);
  int len=(*length)+4+tag_len+val_len;

  p=(char*)realloc(p, len);
  if(p==NULL){
    fprintf(stderr, "realloc failed in comment_add()\n");
    exit(1);
  }

  writeint(p, *length, tag_len+val_len);      /* length of comment */
  if(tag){
    memcpy(p+*length+4, tag, tag_len);        /* comment tag */
    (p+*length+4)[tag_len-1] = '=';           /* separator */
  }
  memcpy(p+*length+4+tag_len, val, val_len);  /* comment */
  writeint(p, 8+4+vendor_length, user_comment_list_length+1);
  *comments=p;
  *length=len;
}

static void comment_pad(char **comments, int* length, int amount)
{
  if(amount>0){
    int i;
    int newlen;
    char* p=*comments;
    /*Make sure there is at least amount worth of padding free, and
       round up to the maximum that fits in the current ogg segments.*/
    newlen=(*length+amount+255)/255*255-1;
    p=(char*)realloc(p,newlen);
    if(p==NULL){
      fprintf(stderr,"realloc failed in comment_pad()\n");
      exit(1);
    }
    for(i=*length;i<newlen;i++)p[i]=0;
    *comments=p;
    *length=newlen;
  }
}
#undef readint
#undef writeint



/*
 * Put a frame into the buffer. When the buffer is full, switch to
 * write OggOpus file
 */
static pj_status_t opus_put_frame(pjmedia_port *this_port, 
				  pjmedia_frame *frame) {
  struct opus_port *oport = (struct opus_port *)this_port;
  struct opus_tool_param *otp = oport->p_otp; 
  int i,ret;
  int size_segments,cur_frame_size;
  if (((otp->op.e_o_s != 1)&&(frame->size != 0))||(otp->id != -1)) {
  oport->frame=frame;

  otp->inopt.read_samples = frame_read;
  otp->inopt.readdata = (void *)oport;

  /* In order to code the complete length we'll need to do a little padding */
  setup_padder(&otp->inopt, &otp->original_samples);

  /**********************
   * opus-tool Start
   * Main encoding one frame
   *********************/
  otp->id++;
  if (otp->id==0) {
    pjmedia_frame_copy(oport->tempframe,frame);
  }


  if(otp->nb_samples<0){
    otp->nb_samples = otp->inopt.read_samples(otp->inopt.readdata,otp->input,otp->frame_size);
    otp->total_samples+=otp->nb_samples;
    if(otp->nb_samples<otp->frame_size)otp->op.e_o_s=1;
    else otp->op.e_o_s=0;
    if(otp->max_ogg_delay>5760)
      return PJ_SUCCESS;
  }
  otp->op.e_o_s|=otp->eos;

  if(otp->start_time==0){
    otp->start_time = time(NULL);
  }

  cur_frame_size=otp->frame_size;

  /*No fancy end padding, just fill with zeros for now.*/
  if(otp->nb_samples<cur_frame_size)for(i=otp->nb_samples*otp->chan;i<cur_frame_size*otp->chan;i++)otp->input[i]=0;

  /*Encode current frame*/
  VG_UNDEF(otp->packet,otp->max_frame_bytes);
  VG_CHECK(otp->input,sizeof(float)*otp->chan*cur_frame_size);
  otp->nbBytes=opus_multistream_encode_float(otp->st, otp->input, cur_frame_size, otp->packet, otp->max_frame_bytes);
  if(otp->nbBytes<0){
    fprintf(stderr, "Encoding failed: %s. Aborting.\n", opus_strerror(otp->nbBytes));
    return !PJ_SUCCESS;
  }
  VG_CHECK(otp->packet,otp->nbBytes);
  VG_UNDEF(otp->input,sizeof(float)*otp->chan*cur_frame_size);
  otp->nb_encoded+=cur_frame_size;
  otp->enc_granulepos+=cur_frame_size*48000/otp->coding_rate;
  otp->total_bytes+=otp->nbBytes;
  size_segments=(otp->nbBytes+255)/255;
  otp->peak_bytes=IMAX(otp->nbBytes,otp->peak_bytes);
  otp->min_bytes=IMIN(otp->nbBytes,otp->min_bytes);

  /*Flush early if adding this packet would make us end up with a
    continued page which we wouldn't have otherwise.*/
  while((((size_segments<=255)&&(otp->last_segments+size_segments>255))||
          (otp->enc_granulepos-otp->last_granulepos>otp->max_ogg_delay))&&
#ifdef OLD_LIBOGG
          ogg_stream_flush(&otp->os, &otp->og)){
#else
          ogg_stream_flush_fill(&otp->os, &otp->og,255*255)){
#endif
    if(ogg_page_packets(&otp->og)!=0)otp->last_granulepos=ogg_page_granulepos(&otp->og);
    otp->last_segments-=otp->og.header[26];
    ret=oe_write_page(&otp->og, otp->fout);
    if(ret!=otp->og.header_len+otp->og.body_len){
        fprintf(stderr,"Error: failed writing data to output stream\n");
        exit(1);
    }
    otp->bytes_written+=ret;
    otp->pages_out++;
  }

  /*The downside of early reading is if the input is an exact
    multiple of the frame_size you'll get an extra frame that needs
    to get cropped off. The downside of late reading is added delay.
    If your ogg_delay is 120ms or less we'll assume you want the
    low delay behavior.*/
  if((!otp->op.e_o_s)&&otp->max_ogg_delay>5760){
    otp->nb_samples = otp->inopt.read_samples(otp->inopt.readdata,otp->input,otp->frame_size);
    otp->total_samples+=otp->nb_samples;
    if(otp->nb_samples<otp->frame_size)otp->eos=1;
    if(otp->nb_samples==0)otp->op.e_o_s=1;
  } else otp->nb_samples=-1;

  otp->op.packet=(unsigned char *)otp->packet;
  otp->op.bytes=otp->nbBytes;
  otp->op.b_o_s=0;
  otp->op.granulepos=otp->enc_granulepos;
  if(otp->op.e_o_s){
    /*We compute the final GP as ceil(len*48k/input_rate). When a resampling
      decoder does the matching floor(len*input/48k) conversion the length will
      be exactly the same as the input.*/
    otp->op.granulepos=((otp->original_samples*48000+otp->rate-1)/otp->rate)+otp->header.preskip;
  }
  otp->op.packetno=2+otp->id;
  ogg_stream_packetin(&otp->os, &otp->op);
  otp->last_segments+=size_segments;

  /*If the stream is over or we're sure that the delayed flush will fire,
    go ahead and flush now to avoid adding delay.*/
  while((otp->op.e_o_s||(otp->enc_granulepos+(otp->frame_size*48000/otp->coding_rate)-otp->last_granulepos>otp->max_ogg_delay)||
          (otp->last_segments>=255))?
#ifdef OLD_LIBOGG
  /*Libogg > 1.2.2 allows us to achieve lower overhead by
    producing larger pages. For 20ms frames this is only relevant
    above ~32kbit/sec.*/
          ogg_stream_flush(&otp->os, &otp->og):
          ogg_stream_pageout(&otp->os, &otp->og)){
#else
          ogg_stream_flush_fill(&otp->os, &otp->og,255*255):
          ogg_stream_pageout_fill(&otp->os, &otp->og,255*255)){
#endif
    if(ogg_page_packets(&otp->og)!=0)otp->last_granulepos=ogg_page_granulepos(&otp->og);
    otp->last_segments-=otp->og.header[26];
    ret=oe_write_page(&otp->og, otp->fout);
    if(ret!=otp->og.header_len+otp->og.body_len){
        fprintf(stderr,"Error: failed writing data to output stream\n");
        exit(1);
    }
    otp->bytes_written+=ret;
    otp->pages_out++;
  }

  if (otp->op.e_o_s) {
    opus_multistream_encoder_destroy(otp->st);
    ogg_stream_clear(&otp->os);
    free(otp->packet);
    free(otp->input);
    if(otp->opt_ctls)free(otp->opt_ctls_ctlval);
    clear_padder(&otp->inopt);
    otp->id=-1;
  }
  /**********************
   * opus-tool End
   *********************/
  }
  return PJ_SUCCESS;

}

/*
 * Get frame, basicy is a no-op operation.
 */
static pj_status_t opus_get_frame(pjmedia_port *this_port, 
				  pjmedia_frame *frame) {
  PJ_UNUSED_ARG(this_port);
  PJ_UNUSED_ARG(frame);
  return PJ_EINVALIDOP;
}

/*
 * Close the port, modify something that is needed.
 */
static pj_status_t opus_on_destroy(pjmedia_port *this_port) {
  pj_status_t status = -1;
  struct opus_port *oport = (struct opus_port *)this_port;
  struct opus_tool_param *otp = oport->p_otp;
  otp->eos=1;
  status = pjmedia_port_put_frame(this_port, oport->tempframe);
  if(otp->fout)fclose(otp->fout);
  if(otp->frange)fclose(otp->frange);
  return status;
}

/*
 * Read Frame which is from the device and push it into buffer
 */
static long frame_read(void *in, float *buffer, int samples)
{
  struct opus_port *oport = (struct opus_port *)in;
  struct opus_tool_param *otp = oport->p_otp;
  int i,j;
  int ch_permute[2]={0,1};
  opus_int64 realsamples;
  pjmedia_frame *frame = oport->frame;
  signed char *buf=(signed char*)frame->buf;
  realsamples = (frame->size)/(oport->bytes_per_sample*otp->inopt.channels);
  for(i=0; i< realsamples; i++) {
    for(j=0; j<otp->inopt.channels;j++) {
      buffer[i*otp->chan+j] = ((buf[i*2*otp->chan + 2*ch_permute[j] + 1]<<8) |
          (buf[i*2*otp->chan + 2*ch_permute[j]] & 0xff))/32768.0f;
    }
  }
  return realsamples;
}

/* Read audio data, appending padding to make up any gap
 * between the available and requested number of samples
 * with LPC-predicted data to minimize the pertubation of
 * the valid data that falls in the same frame.
 */
static long read_padder(void *data, float *buffer, int samples) {
    padder *d = (padder *)data;
    long in_samples = d->real_reader(d->real_readdata, buffer, samples);
    int i, extra=0;
    const int lpc_order=32;

    if(d->original_samples)*d->original_samples+=in_samples;

    if(in_samples<samples){
      if(d->lpc_ptr<0){
        d->lpc_out=(float *)calloc(d->channels * *d->extra_samples, sizeof(*d->lpc_out));
        if(in_samples>lpc_order*2){
          float *lpc=(float *)alloca(lpc_order*sizeof(*lpc));
          for(i=0;i<d->channels;i++){
            vorbis_lpc_from_data(buffer+i,lpc,in_samples,lpc_order,d->channels);
            vorbis_lpc_predict(lpc,buffer+i+(in_samples-lpc_order)*d->channels,
                               lpc_order,d->lpc_out+i,*d->extra_samples,d->channels);
          }
        }
        d->lpc_ptr=0;
      }
      extra=samples-in_samples;
      if(extra>*d->extra_samples)extra=*d->extra_samples;
      *d->extra_samples-=extra;
    }
    memcpy(buffer+in_samples*d->channels,d->lpc_out+d->lpc_ptr*d->channels,extra*d->channels*sizeof(*buffer));
    d->lpc_ptr+=extra;
    return in_samples+extra;
}

void setup_padder(oe_enc_opt *opt,ogg_int64_t *original_samples) {
    padder *d = (padder *)calloc(1, sizeof(padder));

    d->real_reader = opt->read_samples;
    d->real_readdata = opt->readdata;

    opt->read_samples = read_padder;
    opt->readdata = d;
    d->channels = opt->channels;
    d->extra_samples = &opt->extraout;
    d->original_samples=original_samples;
    d->lpc_ptr = -1;
    d->lpc_out = NULL;
}

void clear_padder(oe_enc_opt *opt) {
    padder *d = (padder *)opt->readdata;

    opt->read_samples = d->real_reader;
    opt->readdata = d->real_readdata;

    if(d->lpc_out)free(d->lpc_out);
    free(d);
}
