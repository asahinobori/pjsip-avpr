#include <pjmedia-videodev/videodev_imp.h>
#include <pjmedia-videodev/webm_dev.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/rand.h>
#include <pjmedia/vid_codec.h>

#if defined(PJMEDIA_VIDEO_DEV_HAS_WEBM) && PJMEDIA_VIDEO_DEV_HAS_WEBM != 0 && \
    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

#define THIS_FILE		"webm_dev.c"
#define DRIVER_NAME		"WEBMDev"
#define DEFAULT_CLOCK_RATE	90000
#define DEFAULT_WIDTH		640
#define DEFAULT_HEIGHT		480
#define DEFAULT_FPS		25

typedef struct webm_dev_strm webm_dev_strm;

/* webm_ device info */
struct webm_dev_info
{
    pjmedia_vid_dev_info	 info;

    pj_pool_t			*pool;
    pj_str_t			 fpath;
    pj_str_t			 title;
    pjmedia_webm_streams		*webm;
    pjmedia_port                *vid;
    webm_dev_strm		*strm;
    pjmedia_vid_codec           *codec;
    pj_uint8_t                  *enc_buf;
    pj_size_t                    enc_buf_size;
};

/* webm_factory */
struct webm_factory
{
    pjmedia_vid_dev_factory	 base;
    pj_pool_t			*pool;
    pj_pool_factory		*pf;

    unsigned			 dev_count;
    struct webm_dev_info		*dev_info;
};

/* Video stream. */
struct webm_dev_strm
{
    pjmedia_vid_dev_stream	     base;	    /**< Base stream	    */
    pjmedia_vid_dev_param	     param;	    /**< Settings	    */
    pj_pool_t			    *pool;          /**< Memory pool.       */
    struct webm_dev_info		    *adi;

    pjmedia_vid_dev_cb		     vid_cb;	    /**< Stream callback.   */
    void			    *user_data;	    /**< Application data.  */
};


/* Prototypes */
static pj_status_t webm_factory_init(pjmedia_vid_dev_factory *f);
static pj_status_t webm_factory_destroy(pjmedia_vid_dev_factory *f);
static pj_status_t webm_factory_refresh(pjmedia_vid_dev_factory *f);
static unsigned    webm_factory_get_dev_count(pjmedia_vid_dev_factory *f);
static pj_status_t webm_factory_get_dev_info(pjmedia_vid_dev_factory *f,
					     unsigned index,
					     pjmedia_vid_dev_info *info);
static pj_status_t webm_factory_default_param(pj_pool_t *pool,
                                              pjmedia_vid_dev_factory *f,
					      unsigned index,
					      pjmedia_vid_dev_param *param);
static pj_status_t webm_factory_create_stream(
					pjmedia_vid_dev_factory *f,
					pjmedia_vid_dev_param *param,
					const pjmedia_vid_dev_cb *cb,
					void *user_data,
					pjmedia_vid_dev_stream **p_vid_strm);

static pj_status_t webm_dev_strm_get_param(pjmedia_vid_dev_stream *strm,
					  pjmedia_vid_dev_param *param);
static pj_status_t webm_dev_strm_get_cap(pjmedia_vid_dev_stream *strm,
				        pjmedia_vid_dev_cap cap,
				        void *value);
static pj_status_t webm_dev_strm_set_cap(pjmedia_vid_dev_stream *strm,
				        pjmedia_vid_dev_cap cap,
				        const void *value);
static pj_status_t webm_dev_strm_get_frame(pjmedia_vid_dev_stream *strm,
                                          pjmedia_frame *frame);
static pj_status_t webm_dev_strm_start(pjmedia_vid_dev_stream *strm);
static pj_status_t webm_dev_strm_stop(pjmedia_vid_dev_stream *strm);
static pj_status_t webm_dev_strm_destroy(pjmedia_vid_dev_stream *strm);

static void reset_dev_info(struct webm_dev_info *adi);

/* Operations */
static pjmedia_vid_dev_factory_op factory_op =
{
    &webm_factory_init,
    &webm_factory_destroy,
    &webm_factory_get_dev_count,
    &webm_factory_get_dev_info,
    &webm_factory_default_param,
    &webm_factory_create_stream,
    &webm_factory_refresh
};

static pjmedia_vid_dev_stream_op stream_op =
{
    &webm_dev_strm_get_param,
    &webm_dev_strm_get_cap,
    &webm_dev_strm_set_cap,
    &webm_dev_strm_start,
    &webm_dev_strm_get_frame,
    NULL,
    &webm_dev_strm_stop,
    &webm_dev_strm_destroy
};


/****************************************************************************
 * Factory operations
 */

/* API */
PJ_DEF(pj_status_t) pjmedia_webm_dev_create_factory(
				    pj_pool_factory *pf,
				    unsigned max_dev,
				    pjmedia_vid_dev_factory **p_ret)
{
    struct webm_factory *cf;
    pj_pool_t *pool;
    pj_status_t status;

    pool = pj_pool_create(pf, "webmdevfc%p", 512, 512, NULL);
    cf = PJ_POOL_ZALLOC_T(pool, struct webm_factory);
    cf->pf = pf;
    cf->pool = pool;
    cf->dev_count = max_dev;
    cf->base.op = &factory_op;

    cf->dev_info = (struct webm_dev_info*)
 		   pj_pool_calloc(cf->pool, cf->dev_count,
 				  sizeof(struct webm_dev_info));

    if (p_ret) {
	*p_ret = &cf->base;
    }

    status = pjmedia_vid_register_factory(NULL, &cf->base);
    if (status != PJ_SUCCESS)
	return status;

    PJ_LOG(4, (THIS_FILE, "WEBM dev factory created with %d virtual device(s)",
	       cf->dev_count));

    return PJ_SUCCESS;
}

/* API: init factory */
static pj_status_t webm_factory_init(pjmedia_vid_dev_factory *f)
{
    struct webm_factory *cf = (struct webm_factory*)f;
    unsigned i;

    for (i=0; i<cf->dev_count; ++i) {
	reset_dev_info(&cf->dev_info[i]);
    }

    return PJ_SUCCESS;
}

/* API: destroy factory */
static pj_status_t webm_factory_destroy(pjmedia_vid_dev_factory *f)
{
    struct webm_factory *cf = (struct webm_factory*)f;
    pj_pool_t *pool = cf->pool;

    cf->pool = NULL;
    pj_pool_release(pool);

    return PJ_SUCCESS;
}

/* API: refresh the list of devices */
static pj_status_t webm_factory_refresh(pjmedia_vid_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_SUCCESS;
}

/* API: get number of devices */
static unsigned webm_factory_get_dev_count(pjmedia_vid_dev_factory *f)
{
    struct webm_factory *cf = (struct webm_factory*)f;
    return cf->dev_count;
}

/* API: get device info */
static pj_status_t webm_factory_get_dev_info(pjmedia_vid_dev_factory *f,
					     unsigned index,
					     pjmedia_vid_dev_info *info)
{
    struct webm_factory *cf = (struct webm_factory*)f;

    PJ_ASSERT_RETURN(index < cf->dev_count, PJMEDIA_EVID_INVDEV);

    pj_memcpy(info, &cf->dev_info[index].info, sizeof(*info));

    return PJ_SUCCESS;
}

/* API: create default device parameter */
static pj_status_t webm_factory_default_param(pj_pool_t *pool,
                                              pjmedia_vid_dev_factory *f,
					      unsigned index,
					      pjmedia_vid_dev_param *param)
{
    struct webm_factory *cf = (struct webm_factory*)f;
    struct webm_dev_info *di = &cf->dev_info[index];

    PJ_ASSERT_RETURN(index < cf->dev_count, PJMEDIA_EVID_INVDEV);

    PJ_UNUSED_ARG(pool);

    pj_bzero(param, sizeof(*param));
    param->dir = PJMEDIA_DIR_CAPTURE;
    param->cap_id = index;
    param->rend_id = PJMEDIA_VID_INVALID_DEV;
    param->flags = PJMEDIA_VID_DEV_CAP_FORMAT;
    param->clock_rate = DEFAULT_CLOCK_RATE;
    pj_memcpy(&param->fmt, &di->info.fmt[0], sizeof(param->fmt));

    return PJ_SUCCESS;
}

/* reset dev info */
static void reset_dev_info(struct webm_dev_info *adi)
{
    /* Close webm streams */
    if (adi->webm) {
	unsigned i, cnt;

	cnt = pjmedia_webm_streams_get_num_streams(adi->webm);
	for (i=0; i<cnt; ++i) {
	    pjmedia_webm_stream *as;

	    as = pjmedia_webm_streams_get_stream(adi->webm, i);
	    if (as) {
		pjmedia_port *port;
		port = pjmedia_webm_stream_get_port(as);
		pjmedia_port_destroy(port);
	    }
	}
	adi->webm = NULL;
    }

    if (adi->codec) {
        pjmedia_vid_codec_close(adi->codec);
        adi->codec = NULL;
    }

    if (adi->pool)
	pj_pool_release(adi->pool);

    pj_bzero(adi, sizeof(*adi));

    /* Fill up with *dummy" device info */
    pj_ansi_strncpy(adi->info.name, "WEBM Player", sizeof(adi->info.name)-1);
    pj_ansi_strncpy(adi->info.driver, DRIVER_NAME, sizeof(adi->info.driver)-1);
    adi->info.dir = PJMEDIA_DIR_CAPTURE;
    adi->info.has_callback = PJ_FALSE;
}

/* API: release resources */
PJ_DEF(pj_status_t) pjmedia_webm_dev_free(pjmedia_vid_dev_index id)
{
    pjmedia_vid_dev_factory *f;
    struct webm_factory *cf;
    unsigned local_idx;
    struct webm_dev_info *adi;
    pj_status_t status;

    /* Lookup the factory and local device index */
    status = pjmedia_vid_dev_get_local_index(id, &f, &local_idx);
    if (status != PJ_SUCCESS)
	return status;

    /* The factory must be WEBM factory */
    PJ_ASSERT_RETURN(f->op->init == &webm_factory_init, PJMEDIA_EVID_INVDEV);
    cf = (struct webm_factory*)f;

    /* Device index should be valid */
    PJ_ASSERT_RETURN(local_idx <= cf->dev_count, PJ_EBUG);
    adi = &cf->dev_info[local_idx];

    /* Cannot configure if stream is running */
    if (adi->strm)
	return PJ_EBUSY;

    /* Reset */
    reset_dev_info(adi);
    return PJ_SUCCESS;
}

/* API: get param */
PJ_DEF(pj_status_t) pjmedia_webm_dev_get_param(pjmedia_vid_dev_index id,
                                              pjmedia_webm_dev_param *prm)
{
    pjmedia_vid_dev_factory *f;
    struct webm_factory *cf;
    unsigned local_idx;
    struct webm_dev_info *adi;
    pj_status_t status;

    /* Lookup the factory and local device index */
    status = pjmedia_vid_dev_get_local_index(id, &f, &local_idx);
    if (status != PJ_SUCCESS)
	return status;

    /* The factory must be factory */
    PJ_ASSERT_RETURN(f->op->init == &webm_factory_init, PJMEDIA_EVID_INVDEV);
    cf = (struct webm_factory*)f;

    /* Device index should be valid */
    PJ_ASSERT_RETURN(local_idx <= cf->dev_count, PJ_EBUG);
    adi = &cf->dev_info[local_idx];

    pj_bzero(prm, sizeof(*prm));
    prm->path = adi->fpath;
    prm->title = adi->title;
    prm->webm_streams = adi->webm;

    return PJ_SUCCESS;
}

PJ_DEF(void) pjmedia_webm_dev_param_default(pjmedia_webm_dev_param *p)
{
    pj_bzero(p, sizeof(*p));
}

/* API: configure the WEBM */
PJ_DEF(pj_status_t) pjmedia_webm_dev_alloc( pjmedia_vid_dev_factory *f,
                                           pjmedia_webm_dev_param *p,
                                           pjmedia_vid_dev_index *p_id)
{
    pjmedia_vid_dev_index id;
    struct webm_factory *cf = (struct webm_factory*)f;
    unsigned local_idx;
    struct webm_dev_info *adi = NULL;
    pjmedia_format webm_fmt;
    const pjmedia_video_format_info *vfi;
    pj_status_t status;

    PJ_ASSERT_RETURN(f && p && p_id, PJ_EINVAL);

    if (p_id)
	*p_id = PJMEDIA_VID_INVALID_DEV;

    /* Get a free dev */
    for (local_idx=0; local_idx<cf->dev_count; ++local_idx) {
	if (cf->dev_info[local_idx].webm == NULL) {
	    adi = &cf->dev_info[local_idx];
	    break;
	}
    }

    if (!adi)
	return PJ_ETOOMANY;

    /* Convert local ID to global id */
    status = pjmedia_vid_dev_get_global_index(&cf->base, local_idx, &id);
    if (status != PJ_SUCCESS)
	return status;

    /* Reset */
    if (adi->pool) {
	pj_pool_release(adi->pool);
    }
    pj_bzero(adi, sizeof(*adi));

    /* Reinit */
    PJ_ASSERT_RETURN(p->path.slen, PJ_EINVAL);
    adi->pool = pj_pool_create(cf->pf, "webmdi%p", 512, 512, NULL);


    /* Open the WEBM */
    pj_strdup_with_null(adi->pool, &adi->fpath, &p->path);
    status = pjmedia_webm_player_create_streams(adi->pool, adi->fpath.ptr, 0,
                                               &adi->webm);
    if (status != PJ_SUCCESS) {
	goto on_error;
    }

    adi->vid = pjmedia_webm_streams_get_stream_by_media(adi->webm, 0,
                                                       PJMEDIA_TYPE_VIDEO);
    if (!adi->vid) {
	status = PJMEDIA_EVID_BADFORMAT;
	PJ_LOG(4,(THIS_FILE, "Error: cannot find video in WEBM %s",
		adi->fpath.ptr));
	goto on_error;
    }

    pjmedia_format_copy(&webm_fmt, &adi->vid->info.fmt);
    vfi = pjmedia_get_video_format_info(NULL, webm_fmt.id);
    /* Check whether the frame is encoded. */
    if (!vfi || vfi->bpp == 0) {
        /* Yes, prepare codec */
        const pjmedia_vid_codec_info *codec_info;
        pjmedia_vid_codec_param codec_param;
	pjmedia_video_apply_fmt_param vafp;

        /* Lookup codec */
        status = pjmedia_vid_codec_mgr_get_codec_info2(NULL,
                                                       webm_fmt.id,
                                                       &codec_info);
        if (status != PJ_SUCCESS || !codec_info)
            goto on_error;

        status = pjmedia_vid_codec_mgr_get_default_param(NULL, codec_info,
                                                         &codec_param);
        if (status != PJ_SUCCESS)
            goto on_error;

        /* Open codec */
        status = pjmedia_vid_codec_mgr_alloc_codec(NULL, codec_info,
                                                   &adi->codec);
        if (status != PJ_SUCCESS)
            goto on_error;

        status = pjmedia_vid_codec_init(adi->codec, adi->pool);
        if (status != PJ_SUCCESS)
            goto on_error;

        codec_param.dir = PJMEDIA_DIR_DECODING;
        codec_param.packing = PJMEDIA_VID_PACKING_WHOLE;
        status = pjmedia_vid_codec_open(adi->codec, &codec_param);
        if (status != PJ_SUCCESS)
            goto on_error;

	/* Allocate buffer */
        webm_fmt.id = codec_info->dec_fmt_id[0];
        vfi = pjmedia_get_video_format_info(NULL, webm_fmt.id);
	pj_bzero(&vafp, sizeof(vafp));
	vafp.size = webm_fmt.det.vid.size;
	status = vfi->apply_fmt(vfi, &vafp);
	if (status != PJ_SUCCESS)
	    goto on_error;

	adi->enc_buf = pj_pool_alloc(adi->pool, vafp.framebytes);
	adi->enc_buf_size = vafp.framebytes;
    }

    /* Calculate title */
    if (p->title.slen) {
	pj_strdup_with_null(adi->pool, &adi->title, &p->title);
    } else {
	char *start = p->path.ptr + p->path.slen;
	pj_str_t tmp;

	while (start >= p->path.ptr) {
	    if (*start == '/' || *start == '\\')
		break;
	    --start;
	}
	tmp.ptr = start + 1;
	tmp.slen = p->path.ptr + p->path.slen - tmp.ptr;
	pj_strdup_with_null(adi->pool, &adi->title, &tmp);
    }

    /* Init device info */
    pj_ansi_strncpy(adi->info.name, adi->title.ptr, sizeof(adi->info.name)-1);
    pj_ansi_strncpy(adi->info.driver, DRIVER_NAME, sizeof(adi->info.driver)-1);
    adi->info.dir = PJMEDIA_DIR_CAPTURE;
    adi->info.has_callback = PJ_FALSE;

    adi->info.caps = PJMEDIA_VID_DEV_CAP_FORMAT;
    adi->info.fmt_cnt = 1;
    pjmedia_format_copy(&adi->info.fmt[0], &webm_fmt);

    /* Set out vars */
    if (p_id)
	*p_id = id;
    p->webm_streams = adi->webm;
    if (p->title.slen == 0)
	p->title = adi->title;

    return PJ_SUCCESS;

on_error:
    if (adi->codec) {
        pjmedia_vid_codec_close(adi->codec);
        adi->codec = NULL;
    }
    if (adi->pool) {
	pj_pool_release(adi->pool);
	adi->pool = NULL;
    }
    pjmedia_webm_dev_free(id);
    return status;
}


/* API: create stream */
static pj_status_t webm_factory_create_stream(
					pjmedia_vid_dev_factory *f,
					pjmedia_vid_dev_param *param,
					const pjmedia_vid_dev_cb *cb,
					void *user_data,
					pjmedia_vid_dev_stream **p_vid_strm)
{
    struct webm_factory *cf = (struct webm_factory*)f;
    pj_pool_t *pool = NULL;
    struct webm_dev_info *adi;
    struct webm_dev_strm *strm;

    PJ_ASSERT_RETURN(f && param && p_vid_strm, PJ_EINVAL);
    PJ_ASSERT_RETURN(param->fmt.type == PJMEDIA_TYPE_VIDEO &&
		     param->fmt.detail_type == PJMEDIA_FORMAT_DETAIL_VIDEO &&
                     param->dir == PJMEDIA_DIR_CAPTURE,
		     PJ_EINVAL);

    /* Device must have been configured with pjmedia_webm_dev_set_param() */
    adi = &cf->dev_info[param->cap_id];
    PJ_ASSERT_RETURN(adi->webm != NULL, PJ_EINVALIDOP);

    /* Cannot create while stream is already active */
    PJ_ASSERT_RETURN(adi->strm==NULL, PJ_EINVALIDOP);

    /* Create and initialize basic stream descriptor */
    pool = pj_pool_create(cf->pf, "webmdev%p", 512, 512, NULL);
    PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);

    strm = PJ_POOL_ZALLOC_T(pool, struct webm_dev_strm);
    pj_memcpy(&strm->param, param, sizeof(*param));
    strm->pool = pool;
    pj_memcpy(&strm->vid_cb, cb, sizeof(*cb));
    strm->user_data = user_data;
    strm->adi = adi;

    pjmedia_format_copy(&param->fmt, &adi->info.fmt[0]);

    /* Done */
    strm->base.op = &stream_op;
    adi->strm = strm;
    *p_vid_strm = &strm->base;

    return PJ_SUCCESS;
}

/* API: Get stream info. */
static pj_status_t webm_dev_strm_get_param(pjmedia_vid_dev_stream *s,
					 pjmedia_vid_dev_param *pi)
{
    struct webm_dev_strm *strm = (struct webm_dev_strm*)s;

    PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);

    pj_memcpy(pi, &strm->param, sizeof(*pi));

    return PJ_SUCCESS;
}

/* API: get capability */
static pj_status_t webm_dev_strm_get_cap(pjmedia_vid_dev_stream *s,
				       pjmedia_vid_dev_cap cap,
				       void *pval)
{
    struct webm_dev_strm *strm = (struct webm_dev_strm*)s;

    PJ_UNUSED_ARG(strm);
    PJ_UNUSED_ARG(cap);
    PJ_UNUSED_ARG(pval);

    PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);

    return PJMEDIA_EVID_INVCAP;
}

/* API: set capability */
static pj_status_t webm_dev_strm_set_cap(pjmedia_vid_dev_stream *s,
				       pjmedia_vid_dev_cap cap,
				       const void *pval)
{
    struct webm_dev_strm *strm = (struct webm_dev_strm*)s;

    PJ_UNUSED_ARG(strm);
    PJ_UNUSED_ARG(cap);
    PJ_UNUSED_ARG(pval);

    PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);

    return PJMEDIA_EVID_INVCAP;
}

/* API: Get frame from stream */
static pj_status_t webm_dev_strm_get_frame(pjmedia_vid_dev_stream *strm,
                                         pjmedia_frame *frame)
{
    struct webm_dev_strm *stream = (struct webm_dev_strm*)strm;
    
    if (stream->adi->codec) {
        pjmedia_frame enc_frame;
        pj_status_t status;

        enc_frame.buf = stream->adi->enc_buf;
        enc_frame.size = stream->adi->enc_buf_size;
        status = pjmedia_port_get_frame(stream->adi->vid, &enc_frame);
        if (status != PJ_SUCCESS)
            return status;

        return pjmedia_vid_codec_decode(stream->adi->codec, 1, &enc_frame,
                                        frame->size, frame);
    } else {
        return pjmedia_port_get_frame(stream->adi->vid, frame);
    }
}

/* API: Start stream. */
static pj_status_t webm_dev_strm_start(pjmedia_vid_dev_stream *strm)
{
    struct webm_dev_strm *stream = (struct webm_dev_strm*)strm;

    PJ_UNUSED_ARG(stream);

    PJ_LOG(4, (THIS_FILE, "Starting webm video stream"));

    return PJ_SUCCESS;
}

/* API: Stop stream. */
static pj_status_t webm_dev_strm_stop(pjmedia_vid_dev_stream *strm)
{
    struct webm_dev_strm *stream = (struct webm_dev_strm*)strm;

    PJ_UNUSED_ARG(stream);

    PJ_LOG(4, (THIS_FILE, "Stopping webm video stream"));

    return PJ_SUCCESS;
}


/* API: Destroy stream. */
static pj_status_t webm_dev_strm_destroy(pjmedia_vid_dev_stream *strm)
{
    struct webm_dev_strm *stream = (struct webm_dev_strm*)strm;

    PJ_ASSERT_RETURN(stream != NULL, PJ_EINVAL);

    webm_dev_strm_stop(strm);

    stream->adi->strm = NULL;
    stream->adi = NULL;
    pj_pool_release(stream->pool);

    return PJ_SUCCESS;
}

#endif	/* PJMEDIA_VIDEO_DEV_HAS_WEBM */
