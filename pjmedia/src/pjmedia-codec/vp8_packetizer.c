/*
 * Written by Xusheng Chen
 */
#include <pjmedia-codec/vp8_packetizer.h>
#include <pjmedia/types.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/string.h>

#if defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)


#define THIS_FILE	"vp8_packetizer.c"

/* VP8 packetizer definition */
struct pjmedia_vp8_packetizer {
    /* Current settings */
    pjmedia_vp8_packetizer_cfg cfg;
    
    /* Unpacketizer state */
    unsigned	    unpack_last_sync_pos;
    pj_bool_t	    unpack_prev_lost;
    unsigned int    frame_count;
    void            *buf_ptr;
};

/*
 * Create VP8 packetizer.
 */
PJ_DEF(pj_status_t) pjmedia_vp8_packetizer_create(
				pj_pool_t *pool,
				const pjmedia_vp8_packetizer_cfg *cfg,
				pjmedia_vp8_packetizer **p)
{
    pjmedia_vp8_packetizer *p_;

    PJ_ASSERT_RETURN(pool && p, PJ_EINVAL);

    if (cfg && cfg->mode != PJMEDIA_VP8_PACKETIZER_MODE_DRAFTVP8)
	return PJ_ENOTSUP;

    p_ = PJ_POOL_ZALLOC_T(pool, pjmedia_vp8_packetizer);
    if (cfg) {
	pj_memcpy(&p_->cfg, cfg, sizeof(*cfg));
    } else {
	p_->cfg.mode = PJMEDIA_VP8_PACKETIZER_MODE_DRAFTVP8;
	p_->cfg.mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;
    }

    p_->frame_count = 0;
    p_->buf_ptr = pj_pool_alloc(pool, p_->cfg.mtu);

    *p = p_;

    return PJ_SUCCESS;
}

/*
 * Generate an RTP payload from VP8 frame bitstream, in-place processing.
 */
PJ_DEF(pj_status_t) pjmedia_vp8_packetize(pjmedia_vp8_packetizer *pktz,
					   pj_uint8_t *bits,
                                           pj_size_t bits_len,
                                           unsigned *pos,
                                           const pj_uint8_t **payload,
                                           pj_size_t *payload_len)
{
  pj_uint8_t *p, *ptr, *end;

  pj_assert(pktz && bits && pos && payload && payload_len);
  pj_assert(*pos <= bits_len);

  p = bits + *pos;
  ptr = (pj_uint8_t*)pktz->buf_ptr;
  end = bits + bits_len;
  *ptr = 0x90;
  *(ptr+1) = 0x80;
  *(ptr+2) = pktz->frame_count & 0x7f;
  if (*pos!=0) {
    *ptr &= ~0x10;
  }

  if (end - p + 3 > pktz->cfg.mtu)
    end = p + pktz->cfg.mtu - 3;
  else
    pktz->frame_count += 1;
  pj_memcpy(ptr+3, p, end - p); 
  *payload = ptr;
  *payload_len = end - p + 3;
  *pos = end - bits;

  return PJ_SUCCESS;
}

/*
 * Append an RTP payload to a VP8 picture bitstream.
 */
PJ_DEF(pj_status_t) pjmedia_vp8_unpacketize (pjmedia_vp8_packetizer *pktz,
					      const pj_uint8_t *payload,
                                              pj_size_t payload_len,
                                              pj_uint8_t *bits,
                                              pj_size_t bits_size,
					      unsigned *pos)
{
  int start_partition, extended_bits, part_id;
  int pictureid_present = 0, tl0picidx_present = 0, tid_present = 0,
             keyidx_present = 0;
  int pictureid = -1, pictureid_mask = 0;
  const pj_uint8_t *p = payload;
  pj_size_t plen = payload_len;
  pj_uint8_t *q;

  q = bits + *pos;

  /* Check if this is a missing/lost packet */
  if (payload == NULL) {
    pktz->unpack_prev_lost = PJ_TRUE;
    return PJ_EINVAL;
  }

  extended_bits = *p & 0x80;
  start_partition = *p & 0x10;
  part_id = *p & 0x0f;
  p++;
  plen--;

  if (extended_bits) {
    if (plen < 1) {
      pktz->unpack_prev_lost = PJ_TRUE;
      return PJ_EINVAL;
    }
    pictureid_present = *p & 0x80;
    tl0picidx_present = *p & 0x40;
    tid_present       = *p & 0x20;
    keyidx_present    = *p & 0x10;
    p++;
    plen--;
  }

  if (pictureid_present) {
    if (plen < 1) {
      pktz->unpack_prev_lost = PJ_TRUE;
      return PJ_EINVAL;
    }
    if (*p & 0x80) {
      if (plen < 2) {
        pktz->unpack_prev_lost = PJ_TRUE;
        return PJ_EINVAL;
      }
      pictureid = (pj_uint16_t)*p & 0x7fff;
      pictureid_mask = 0x7fff;
      p += 2;
      plen -= 2;
    } else {
      pictureid = *p & 0x7f;
      pictureid_mask = 0x7f;
      p++;
      plen--;
    }
  }
  if (tl0picidx_present) {
    // Ignoring temporal level zero index
    p++;
    plen--;
  }
  if (tid_present || keyidx_present) {
    // Ignoring temporal layer index, layer sync bit and keyframe index
    p++;
    plen--;
  }
  if (plen < 1) {
    pktz->unpack_prev_lost = PJ_TRUE;
    return PJ_EINVAL;
  }

  /* Start writing bistream */
  pj_memcpy(q, p, plen);
  q += plen;

  /* Update the bitstream writing offset */
  *pos = q - bits;

  pktz->unpack_prev_lost = PJ_FALSE;

  return PJ_SUCCESS;
}
#endif /* PJMEDIA_HAS_VIDEO */