/*
 * Copyright 2018-2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "../../ssl_local.h"
#include "../record_local.h"
#include "recmethod_local.h"

/* mod 128 saturating subtract of two 64-bit values in big-endian order */
static int satsub64be(const unsigned char *v1, const unsigned char *v2)
{
    int64_t ret;
    uint64_t l1, l2;

    n2l8(v1, l1);
    n2l8(v2, l2);

    ret = l1 - l2;

    /* We do not permit wrap-around */
    if (l1 > l2 && ret < 0)
        return 128;
    else if (l2 > l1 && ret > 0)
        return -128;

    if (ret > 128)
        return 128;
    else if (ret < -128)
        return -128;
    else
        return (int)ret;
}

static int dtls_record_replay_check(OSSL_RECORD_LAYER *rl, DTLS_BITMAP *bitmap)
{
    int cmp;
    unsigned int shift;
    const unsigned char *seq = rl->sequence;

    cmp = satsub64be(seq, bitmap->max_seq_num);
    if (cmp > 0) {
        SSL3_RECORD_set_seq_num(&rl->rrec[0], seq);
        return 1;               /* this record in new */
    }
    shift = -cmp;
    if (shift >= sizeof(bitmap->map) * 8)
        return 0;               /* stale, outside the window */
    else if (bitmap->map & ((uint64_t)1 << shift))
        return 0;               /* record previously received */

    SSL3_RECORD_set_seq_num(&rl->rrec[0], seq);
    return 1;
}

static void dtls_record_bitmap_update(OSSL_RECORD_LAYER *rl,
                                      DTLS_BITMAP *bitmap)
{
    int cmp;
    unsigned int shift;
    const unsigned char *seq = rl->sequence;

    cmp = satsub64be(seq, bitmap->max_seq_num);
    if (cmp > 0) {
        shift = cmp;
        if (shift < sizeof(bitmap->map) * 8)
            bitmap->map <<= shift, bitmap->map |= 1UL;
        else
            bitmap->map = 1UL;
        memcpy(bitmap->max_seq_num, seq, SEQ_NUM_SIZE);
    } else {
        shift = -cmp;
        if (shift < sizeof(bitmap->map) * 8)
            bitmap->map |= (uint64_t)1 << shift;
    }
}

static DTLS_BITMAP *dtls_get_bitmap(OSSL_RECORD_LAYER *rl, SSL3_RECORD *rr,
                                    unsigned int *is_next_epoch)
{
    *is_next_epoch = 0;

    /* In current epoch, accept HM, CCS, DATA, & ALERT */
    if (rr->epoch == rl->epoch)
        return &rl->bitmap;

    /*
     * Only HM and ALERT messages can be from the next epoch and only if we
     * have already processed all of the unprocessed records from the last
     * epoch
     */
    else if (rr->epoch == (unsigned long)(rl->epoch + 1)
             && rl->unprocessed_rcds.epoch != rl->epoch
             && (rr->type == SSL3_RT_HANDSHAKE || rr->type == SSL3_RT_ALERT)) {
        *is_next_epoch = 1;
        return &rl->next_bitmap;
    }

    return NULL;
}

static void dtls_set_in_init(OSSL_RECORD_LAYER *rl, int in_init)
{
    rl->in_init = in_init;
}

static int dtls_process_record(OSSL_RECORD_LAYER *rl, DTLS_BITMAP *bitmap)
{
    int i;
    int enc_err;
    SSL3_RECORD *rr;
    int imac_size;
    size_t mac_size = 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    SSL_MAC_BUF macbuf = { NULL, 0 };
    int ret = 0;

    rr = &rl->rrec[0];

    /*
     * At this point, rl->packet_length == DTLS1_RT_HEADER_LENGTH + rr->length,
     * and we have that many bytes in rl->packet
     */
    rr->input = &(rl->packet[DTLS1_RT_HEADER_LENGTH]);

    /*
     * ok, we can now read from 'rl->packet' data into 'rr'. rr->input
     * points at rr->length bytes, which need to be copied into rr->data by
     * either the decryption or by the decompression. When the data is 'copied'
     * into the rr->data buffer, rr->input will be pointed at the new buffer
     */

    /*
     * We now have - encrypted [ MAC [ compressed [ plain ] ] ] rr->length
     * bytes of encrypted compressed stuff.
     */

    /* check is not needed I believe */
    if (rr->length > SSL3_RT_MAX_ENCRYPTED_LENGTH) {
        RLAYERfatal(rl, SSL_AD_RECORD_OVERFLOW, SSL_R_ENCRYPTED_LENGTH_TOO_LONG);
        return 0;
    }

    /* decrypt in place in 'rr->input' */
    rr->data = rr->input;
    rr->orig_len = rr->length;

    if (rl->md_ctx != NULL) {
        const EVP_MD *tmpmd = EVP_MD_CTX_get0_md(rl->md_ctx);

        if (tmpmd != NULL) {
            imac_size = EVP_MD_get_size(tmpmd);
            if (!ossl_assert(imac_size >= 0 && imac_size <= EVP_MAX_MD_SIZE)) {
                RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_EVP_LIB);
                return 0;
            }
            mac_size = (size_t)imac_size;
        }
    }

    if (rl->use_etm && rl->md_ctx != NULL) {
        unsigned char *mac;

        if (rr->orig_len < mac_size) {
            RLAYERfatal(rl, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_TOO_SHORT);
            return 0;
        }
        rr->length -= mac_size;
        mac = rr->data + rr->length;
        i = rl->funcs->mac(rl, rr, md, 0 /* not send */);
        if (i == 0 || CRYPTO_memcmp(md, mac, (size_t)mac_size) != 0) {
            RLAYERfatal(rl, SSL_AD_BAD_RECORD_MAC,
                        SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC);
            return 0;
        }
        /*
         * We've handled the mac now - there is no MAC inside the encrypted
         * record
         */
        mac_size = 0;
    }

    /*
     * Set a mark around the packet decryption attempt.  This is DTLS, so
     * bad packets are just ignored, and we don't want to leave stray
     * errors in the queue from processing bogus junk that we ignored.
     */
    ERR_set_mark();
    enc_err = rl->funcs->cipher(rl, rr, 1, 0, &macbuf, mac_size);

    /*-
     * enc_err is:
     *    0: if the record is publicly invalid, or an internal error, or AEAD
     *       decryption failed, or ETM decryption failed.
     *    1: Success or MTE decryption failed (MAC will be randomised)
     */
    if (enc_err == 0) {
        ERR_pop_to_mark();
        if (rl->alert != SSL_AD_NO_ALERT) {
            /* RLAYERfatal() already called */
            goto end;
        }
        /* For DTLS we simply ignore bad packets. */
        rr->length = 0;
        rl->packet_length = 0;
        goto end;
    }
    ERR_clear_last_mark();
    OSSL_TRACE_BEGIN(TLS) {
        BIO_printf(trc_out, "dec %zd\n", rr->length);
        BIO_dump_indent(trc_out, rr->data, rr->length, 4);
    } OSSL_TRACE_END(TLS);

    /* r->length is now the compressed data plus mac */
    if (!rl->use_etm
            && (rl->enc_ctx != NULL)
            && (EVP_MD_CTX_get0_md(rl->md_ctx) != NULL)) {
        /* rl->md_ctx != NULL => mac_size != -1 */

        i = rl->funcs->mac(rl, rr, md, 0 /* not send */);
        if (i == 0 || macbuf.mac == NULL
            || CRYPTO_memcmp(md, macbuf.mac, mac_size) != 0)
            enc_err = 0;
        if (rr->length > SSL3_RT_MAX_COMPRESSED_LENGTH + mac_size)
            enc_err = 0;
    }

    if (enc_err == 0) {
        /* decryption failed, silently discard message */
        rr->length = 0;
        rl->packet_length = 0;
        goto end;
    }

    /* r->length is now just compressed */
    if (rl->compctx != NULL) {
        if (rr->length > SSL3_RT_MAX_COMPRESSED_LENGTH) {
            RLAYERfatal(rl, SSL_AD_RECORD_OVERFLOW,
                        SSL_R_COMPRESSED_LENGTH_TOO_LONG);
            goto end;
        }
        if (!tls_do_uncompress(rl, rr)) {
            RLAYERfatal(rl, SSL_AD_DECOMPRESSION_FAILURE, SSL_R_BAD_DECOMPRESSION);
            goto end;
        }
    }

    /*
     * Check if the received packet overflows the current Max Fragment
     * Length setting.
     */
    if (rr->length > rl->max_frag_len) {
        RLAYERfatal(rl, SSL_AD_RECORD_OVERFLOW, SSL_R_DATA_LENGTH_TOO_LONG);
        goto end;
    }

    rr->off = 0;
    /*-
     * So at this point the following is true
     * ssl->s3.rrec.type   is the type of record
     * ssl->s3.rrec.length == number of bytes in record
     * ssl->s3.rrec.off    == offset to first valid byte
     * ssl->s3.rrec.data   == where to take bytes from, increment
     *                        after use :-).
     */

    /* we have pulled in a full packet so zero things */
    rl->packet_length = 0;

    /* Mark receipt of record. */
    dtls_record_bitmap_update(rl, bitmap);

    ret = 1;
 end:
    if (macbuf.alloced)
        OPENSSL_free(macbuf.mac);
    return ret;
}

static int dtls_rlayer_buffer_record(OSSL_RECORD_LAYER *rl, record_pqueue *queue,
                                     unsigned char *priority)
{
    DTLS_RLAYER_RECORD_DATA *rdata;
    pitem *item;

    /* Limit the size of the queue to prevent DOS attacks */
    if (pqueue_size(queue->q) >= 100)
        return 0;

    rdata = OPENSSL_malloc(sizeof(*rdata));
    item = pitem_new(priority, rdata);
    if (rdata == NULL || item == NULL) {
        OPENSSL_free(rdata);
        pitem_free(item);
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return -1;
    }

    rdata->packet = rl->packet;
    rdata->packet_length = rl->packet_length;
    memcpy(&(rdata->rbuf), &rl->rbuf, sizeof(SSL3_BUFFER));
    memcpy(&(rdata->rrec), &rl->rrec[0], sizeof(SSL3_RECORD));

    item->data = rdata;

    rl->packet = NULL;
    rl->packet_length = 0;
    memset(&rl->rbuf, 0, sizeof(SSL3_BUFFER));
    memset(&rl->rrec[0], 0, sizeof(rl->rrec[0]));

    if (!tls_setup_read_buffer(rl)) {
        /* RLAYERfatal() already called */
        OPENSSL_free(rdata->rbuf.buf);
        OPENSSL_free(rdata);
        pitem_free(item);
        return -1;
    }

    if (pqueue_insert(queue->q, item) == NULL) {
        /* Must be a duplicate so ignore it */
        OPENSSL_free(rdata->rbuf.buf);
        OPENSSL_free(rdata);
        pitem_free(item);
    }

    return 1;
}

/* copy buffered record into OSSL_RECORD_LAYER structure */
static int dtls_copy_rlayer_record(OSSL_RECORD_LAYER *rl, pitem *item)
{
    DTLS_RLAYER_RECORD_DATA *rdata;

    rdata = (DTLS_RLAYER_RECORD_DATA *)item->data;

    SSL3_BUFFER_release(&rl->rbuf);

    rl->packet = rdata->packet;
    rl->packet_length = rdata->packet_length;
    memcpy(&rl->rbuf, &(rdata->rbuf), sizeof(SSL3_BUFFER));
    memcpy(&rl->rrec[0], &(rdata->rrec), sizeof(SSL3_RECORD));

    /* Set proper sequence number for mac calculation */
    memcpy(&(rl->sequence[2]), &(rdata->packet[5]), 6);

    return 1;
}

static int dtls_retrieve_rlayer_buffered_record(OSSL_RECORD_LAYER *rl,
                                                record_pqueue *queue)
{
    pitem *item;

    item = pqueue_pop(queue->q);
    if (item) {
        dtls_copy_rlayer_record(rl, item);

        OPENSSL_free(item->data);
        pitem_free(item);

        return 1;
    }

    return 0;
}

/*-
 * Call this to get a new input record.
 * It will return <= 0 if more data is needed, normally due to an error
 * or non-blocking IO.
 * When it finishes, one packet has been decoded and can be found in
 * ssl->s3.rrec.type    - is the type of record
 * ssl->s3.rrec.data    - data
 * ssl->s3.rrec.length  - number of bytes
 */
int dtls_get_more_records(OSSL_RECORD_LAYER *rl)
{
    int ssl_major, ssl_minor;
    int rret;
    size_t more, n;
    SSL3_RECORD *rr;
    unsigned char *p = NULL;
    unsigned short version;
    DTLS_BITMAP *bitmap;
    unsigned int is_next_epoch;

    rl->num_recs = 0;
    rl->curr_rec = 0;
    rl->num_released = 0;

    rr = rl->rrec;

    if (rl->rbuf.buf == NULL) {
        if (!tls_setup_read_buffer(rl)) {
            /* RLAYERfatal() already called */
            return OSSL_RECORD_RETURN_FATAL;
        }
    }

 again:
    /* if we're renegotiating, then there may be buffered records */
    if (dtls_retrieve_rlayer_buffered_record(rl, &rl->processed_rcds)) {
        rl->num_recs = 1;
        return OSSL_RECORD_RETURN_SUCCESS;
    }

    /* get something from the wire */

    /* check if we have the header */
    if ((rl->rstate != SSL_ST_READ_BODY) ||
        (rl->packet_length < DTLS1_RT_HEADER_LENGTH)) {
        rret = rl->funcs->read_n(rl, DTLS1_RT_HEADER_LENGTH,
                                 SSL3_BUFFER_get_len(&rl->rbuf), 0, 1, &n);
        /* read timeout is handled by dtls1_read_bytes */
        if (rret < OSSL_RECORD_RETURN_SUCCESS) {
            /* RLAYERfatal() already called if appropriate */
            return rret;         /* error or non-blocking */
        }

        /* this packet contained a partial record, dump it */
        if (rl->packet_length != DTLS1_RT_HEADER_LENGTH) {
            rl->packet_length = 0;
            goto again;
        }

        rl->rstate = SSL_ST_READ_BODY;

        p = rl->packet;

        if (rl->msg_callback != NULL)
            rl->msg_callback(0, 0, SSL3_RT_HEADER, p, DTLS1_RT_HEADER_LENGTH,
                            rl->cbarg);

        /* Pull apart the header into the DTLS1_RECORD */
        rr->type = *(p++);
        ssl_major = *(p++);
        ssl_minor = *(p++);
        version = (ssl_major << 8) | ssl_minor;

        /* sequence number is 64 bits, with top 2 bytes = epoch */
        n2s(p, rr->epoch);

        memcpy(&(rl->sequence[2]), p, 6);
        p += 6;

        n2s(p, rr->length);

        /*
         * Lets check the version. We tolerate alerts that don't have the exact
         * version number (e.g. because of protocol version errors)
         */
        if (!rl->is_first_record && rr->type != SSL3_RT_ALERT) {
            if (version != rl->version) {
                /* unexpected version, silently discard */
                rr->length = 0;
                rl->packet_length = 0;
                goto again;
            }
        }

        if (ssl_major !=
                (rl->version == DTLS_ANY_VERSION ? DTLS1_VERSION_MAJOR
                                                 : rl->version >> 8)) {
            /* wrong version, silently discard record */
            rr->length = 0;
            rl->packet_length = 0;
            goto again;
        }

        if (rr->length > SSL3_RT_MAX_ENCRYPTED_LENGTH) {
            /* record too long, silently discard it */
            rr->length = 0;
            rl->packet_length = 0;
            goto again;
        }

        /*
         * If received packet overflows maximum possible fragment length then
         * silently discard it
         */
        if (rr->length > rl->max_frag_len + SSL3_RT_MAX_ENCRYPTED_OVERHEAD) {
            /* record too long, silently discard it */
            rr->length = 0;
            rl->packet_length = 0;
            goto again;
        }

        /* now rl->rstate == SSL_ST_READ_BODY */
    }

    /* rl->rstate == SSL_ST_READ_BODY, get and decode the data */

    if (rr->length > rl->packet_length - DTLS1_RT_HEADER_LENGTH) {
        /* now rl->packet_length == DTLS1_RT_HEADER_LENGTH */
        more = rr->length;
        rret = rl->funcs->read_n(rl, more, more, 1, 1, &n);
        /* this packet contained a partial record, dump it */
        if (rret < OSSL_RECORD_RETURN_SUCCESS || n != more) {
            if (rl->alert != SSL_AD_NO_ALERT) {
                /* read_n() called RLAYERfatal() */
                return OSSL_RECORD_RETURN_FATAL;
            }
            rr->length = 0;
            rl->packet_length = 0;
            goto again;
        }

        /*
         * now n == rr->length,
         * and rl->packet_length ==  DTLS1_RT_HEADER_LENGTH + rr->length
         */
    }
    /* set state for later operations */
    rl->rstate = SSL_ST_READ_HEADER;

    /* match epochs.  NULL means the packet is dropped on the floor */
    bitmap = dtls_get_bitmap(rl, rr, &is_next_epoch);
    if (bitmap == NULL) {
        rr->length = 0;
        rl->packet_length = 0; /* dump this record */
        goto again;             /* get another record */
    }
#ifndef OPENSSL_NO_SCTP
    /* Only do replay check if no SCTP bio */
    if (!BIO_dgram_is_sctp(rl->bio)) {
#endif
        /* Check whether this is a repeat, or aged record. */
        if (!dtls_record_replay_check(rl, bitmap)) {
            rr->length = 0;
            rl->packet_length = 0; /* dump this record */
            goto again;         /* get another record */
        }
#ifndef OPENSSL_NO_SCTP
    }
#endif

    /* just read a 0 length packet */
    if (rr->length == 0)
        goto again;

    /*
     * If this record is from the next epoch (either HM or ALERT), and a
     * handshake is currently in progress, buffer it since it cannot be
     * processed at this time.
     */
    if (is_next_epoch) {
        if (rl->in_init) {
            if (dtls_rlayer_buffer_record(rl, &(rl->unprocessed_rcds),
                                          rr->seq_num) < 0) {
                /* RLAYERfatal() already called */
                return OSSL_RECORD_RETURN_FATAL;
            }
        }
        rr->length = 0;
        rl->packet_length = 0;
        goto again;
    }

    if (!dtls_process_record(rl, bitmap)) {
        if (rl->alert != SSL_AD_NO_ALERT) {
            /* dtls_process_record() called RLAYERfatal */
            return OSSL_RECORD_RETURN_FATAL;
        }
        rr->length = 0;
        rl->packet_length = 0; /* dump this record */
        goto again;             /* get another record */
    }

    rl->num_recs = 1;
    return OSSL_RECORD_RETURN_SUCCESS;
}

static int dtls_free(OSSL_RECORD_LAYER *rl)
{
    SSL3_BUFFER *rbuf;
    size_t left, written;
    pitem *item;
    DTLS_RLAYER_RECORD_DATA *rdata;
    int ret = 1;

    rbuf = &rl->rbuf;

    left = rbuf->left;
    if (left > 0) {
        /*
         * This record layer is closing but we still have data left in our
         * buffer. It must be destined for the next epoch - so push it there.
         */
        ret = BIO_write_ex(rl->next, rbuf->buf + rbuf->offset, left, &written);
        rbuf->left = 0;
    }

    if (rl->unprocessed_rcds.q != NULL) {
        while ((item = pqueue_pop(rl->unprocessed_rcds.q)) != NULL) {
            rdata = (DTLS_RLAYER_RECORD_DATA *)item->data;
            /* Push to the next record layer */
            ret &= BIO_write_ex(rl->next, rdata->packet, rdata->packet_length,
                                &written);
            OPENSSL_free(rdata->rbuf.buf);
            OPENSSL_free(item->data);
            pitem_free(item);
        }
        pqueue_free(rl->unprocessed_rcds.q);
    }

    if (rl->processed_rcds.q != NULL) {
        while ((item = pqueue_pop(rl->processed_rcds.q)) != NULL) {
            rdata = (DTLS_RLAYER_RECORD_DATA *)item->data;
            OPENSSL_free(rdata->rbuf.buf);
            OPENSSL_free(item->data);
            pitem_free(item);
        }
        pqueue_free(rl->processed_rcds.q);
    }

    return tls_free(rl) && ret;
}

static int
dtls_new_record_layer(OSSL_LIB_CTX *libctx, const char *propq, int vers,
                      int role, int direction, int level, uint16_t epoch,
                      unsigned char *key, size_t keylen, unsigned char *iv,
                      size_t ivlen, unsigned char *mackey, size_t mackeylen,
                      const EVP_CIPHER *ciph, size_t taglen,
                      int mactype,
                      const EVP_MD *md, COMP_METHOD *comp, BIO *prev,
                      BIO *transport, BIO *next, BIO_ADDR *local, BIO_ADDR *peer,
                      const OSSL_PARAM *settings, const OSSL_PARAM *options,
                      const OSSL_DISPATCH *fns, void *cbarg,
                      OSSL_RECORD_LAYER **retrl)
{
    int ret;

    ret = tls_int_new_record_layer(libctx, propq, vers, role, direction, level,
                                   key, keylen, iv, ivlen, mackey, mackeylen,
                                   ciph, taglen, mactype, md, comp, prev,
                                   transport, next, local, peer, settings,
                                   options, fns, cbarg, retrl);

    if (ret != OSSL_RECORD_RETURN_SUCCESS)
        return ret;

    (*retrl)->unprocessed_rcds.q = pqueue_new();
    (*retrl)->processed_rcds.q = pqueue_new();
    if ((*retrl)->unprocessed_rcds.q == NULL
            || (*retrl)->processed_rcds.q == NULL) {
        dtls_free(*retrl);
        *retrl = NULL;
        ERR_raise(ERR_LIB_SSL, ERR_R_SSL_LIB);
        return OSSL_RECORD_RETURN_FATAL;
    }

    (*retrl)->unprocessed_rcds.epoch = epoch + 1;
    (*retrl)->processed_rcds.epoch = epoch;

    (*retrl)->isdtls = 1;
    (*retrl)->epoch = epoch;
    (*retrl)->in_init = 1;

    switch (vers) {
    case DTLS_ANY_VERSION:
        (*retrl)->funcs = &dtls_any_funcs;
        break;
    case DTLS1_2_VERSION:
    case DTLS1_VERSION:
    case DTLS1_BAD_VER:
        (*retrl)->funcs = &dtls_1_funcs;
        break;
    default:
        /* Should not happen */
        ERR_raise(ERR_LIB_SSL, ERR_R_INTERNAL_ERROR);
        ret = OSSL_RECORD_RETURN_FATAL;
        goto err;
    }

    ret = (*retrl)->funcs->set_crypto_state(*retrl, level, key, keylen, iv,
                                            ivlen, mackey, mackeylen, ciph,
                                            taglen, mactype, md, comp);

 err:
    if (ret != OSSL_RECORD_RETURN_SUCCESS) {
        OPENSSL_free(*retrl);
        *retrl = NULL;
    }
    return ret;
}

/*
 * TODO(RECLAYER): Temporary copy of the old ssl3_write_pending() function now
 * replaced by tls_retry_write_records(). Needs to be removed when the DTLS code
 * is converted
 */
/* if SSL3_BUFFER_get_left() != 0, we need to call this
 *
 * Return values are as per SSL_write()
 */
static int ssl3_write_pending(OSSL_RECORD_LAYER *rl, int type,
                              const unsigned char *buf, size_t len,
                              size_t *written)
{
    int i;
    /* TODO(RECLAYER): Remove me */
    SSL_CONNECTION *s = (SSL_CONNECTION *)rl->cbarg;
    SSL3_BUFFER *wb = rl->wbuf;
    size_t currbuf = 0;
    size_t tmpwrit = 0;

    if ((s->rlayer.wpend_tot > len)
        || (!(s->mode & SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER)
            && (s->rlayer.wpend_buf != buf))
        || (s->rlayer.wpend_type != type)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_R_BAD_WRITE_RETRY);
        return -1;
    }

    for (;;) {
        clear_sys_error();
        if (s->wbio != NULL) {
            s->rwstate = SSL_WRITING;

            /*
             * To prevent coalescing of control and data messages,
             * such as in buffer_write, we flush the BIO
             */
            if (BIO_get_ktls_send(s->wbio) && type != SSL3_RT_APPLICATION_DATA) {
                i = BIO_flush(s->wbio);
                if (i <= 0)
                    return i;
                BIO_set_ktls_ctrl_msg(s->wbio, type);
            }
            i = BIO_write(s->wbio, (char *)
                          &(SSL3_BUFFER_get_buf(&wb[currbuf])
                            [SSL3_BUFFER_get_offset(&wb[currbuf])]),
                          (unsigned int)SSL3_BUFFER_get_left(&wb[currbuf]));
            if (i >= 0)
                tmpwrit = i;
        } else {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_R_BIO_NOT_SET);
            i = -1;
        }

        /*
         * When an empty fragment is sent on a connection using KTLS,
         * it is sent as a write of zero bytes.  If this zero byte
         * write succeeds, i will be 0 rather than a non-zero value.
         * Treat i == 0 as success rather than an error for zero byte
         * writes to permit this case.
         */
        if (i >= 0 && tmpwrit == SSL3_BUFFER_get_left(&wb[currbuf])) {
            SSL3_BUFFER_set_left(&wb[currbuf], 0);
            SSL3_BUFFER_add_offset(&wb[currbuf], tmpwrit);
            s->rwstate = SSL_NOTHING;
            *written = s->rlayer.wpend_ret;
            return 1;
        } else if (i <= 0) {
            if (SSL_CONNECTION_IS_DTLS(s)) {
                /*
                 * For DTLS, just drop it. That's kind of the whole point in
                 * using a datagram service
                 */
                SSL3_BUFFER_set_left(&wb[currbuf], 0);
            }
            return i;
        }
        SSL3_BUFFER_add_offset(&wb[currbuf], tmpwrit);
        SSL3_BUFFER_sub_left(&wb[currbuf], tmpwrit);
    }
}

static int dtls_write_records(OSSL_RECORD_LAYER *rl,
                              OSSL_RECORD_TEMPLATE *templates,
                              size_t numtempl)
{
    /* TODO(RECLAYER): Remove me */
    SSL_CONNECTION *sc = (SSL_CONNECTION *)rl->cbarg;
    unsigned char *p, *pseq;
    int mac_size, clear = 0;
    size_t written;
    int eivlen;
    SSL3_RECORD wr;
    SSL3_BUFFER *wb;
    SSL_SESSION *sess;
    SSL *s = SSL_CONNECTION_GET_SSL(sc);

    sess = sc->session;

    if ((sess == NULL)
            || (sc->enc_write_ctx == NULL)
            || (EVP_MD_CTX_get0_md(sc->write_hash) == NULL))
        clear = 1;

    if (clear)
        mac_size = 0;
    else {
        mac_size = EVP_MD_CTX_get_size(sc->write_hash);
        if (mac_size < 0) {
            SSLfatal(sc, SSL_AD_INTERNAL_ERROR,
                     SSL_R_EXCEEDS_MAX_FRAGMENT_SIZE);
            return -1;
        }
    }

    if (numtempl != 1) {
        /* Should not happen */
        SSLfatal(sc, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return -1;
    }

    if (!rl->funcs->allocate_write_buffers(rl, templates, numtempl, NULL)) {
        /* RLAYERfatal() already called */
        return -1;
    }

    wb = rl->wbuf;
    p = SSL3_BUFFER_get_buf(wb);

    /* write the header */

    *(p++) = templates->type & 0xff;
    SSL3_RECORD_set_type(&wr, templates->type);
    *(p++) = templates->version >> 8;
    *(p++) = templates->version & 0xff;

    /* field where we are to write out packet epoch, seq num and len */
    pseq = p;
    p += 10;

    /* Explicit IV length, block ciphers appropriate version flag */
    if (sc->enc_write_ctx) {
        int mode = EVP_CIPHER_CTX_get_mode(sc->enc_write_ctx);
        if (mode == EVP_CIPH_CBC_MODE) {
            eivlen = EVP_CIPHER_CTX_get_iv_length(sc->enc_write_ctx);
            if (eivlen < 0) {
                SSLfatal(sc, SSL_AD_INTERNAL_ERROR, SSL_R_LIBRARY_BUG);
                return -1;
            }
            if (eivlen <= 1)
                eivlen = 0;
        }
        /* Need explicit part of IV for GCM mode */
        else if (mode == EVP_CIPH_GCM_MODE)
            eivlen = EVP_GCM_TLS_EXPLICIT_IV_LEN;
        else if (mode == EVP_CIPH_CCM_MODE)
            eivlen = EVP_CCM_TLS_EXPLICIT_IV_LEN;
        else
            eivlen = 0;
    } else
        eivlen = 0;

    /* lets setup the record stuff. */
    SSL3_RECORD_set_data(&wr, p + eivlen); /* make room for IV in case of CBC */
    SSL3_RECORD_set_length(&wr, templates->buflen);
    SSL3_RECORD_set_input(&wr, (unsigned char *)templates->buf);

    /*
     * we now 'read' from wr.input, wr.length bytes into wr.data
     */

    /* first we compress */
    if (sc->compress != NULL) {
        if (!ssl3_do_compress(sc, &wr)) {
            SSLfatal(sc, SSL_AD_INTERNAL_ERROR, SSL_R_COMPRESSION_FAILURE);
            return -1;
        }
    } else {
        memcpy(SSL3_RECORD_get_data(&wr), SSL3_RECORD_get_input(&wr),
               SSL3_RECORD_get_length(&wr));
        SSL3_RECORD_reset_input(&wr);
    }

    /*
     * we should still have the output to wr.data and the input from
     * wr.input.  Length should be wr.length. wr.data still points in the
     * wb->buf
     */

    if (!SSL_WRITE_ETM(sc) && mac_size != 0) {
        if (!s->method->ssl3_enc->mac(sc, &wr,
                                      &(p[SSL3_RECORD_get_length(&wr) + eivlen]),
                                      1)) {
            SSLfatal(sc, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return -1;
        }
        SSL3_RECORD_add_length(&wr, mac_size);
    }

    /* this is true regardless of mac size */
    SSL3_RECORD_set_data(&wr, p);
    SSL3_RECORD_reset_input(&wr);

    if (eivlen)
        SSL3_RECORD_add_length(&wr, eivlen);

    if (s->method->ssl3_enc->enc(sc, &wr, 1, 1, NULL, mac_size) < 1) {
        if (!ossl_statem_in_error(sc)) {
            SSLfatal(sc, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        }
        return -1;
    }

    if (SSL_WRITE_ETM(sc) && mac_size != 0) {
        if (!s->method->ssl3_enc->mac(sc, &wr,
                                      &(p[SSL3_RECORD_get_length(&wr)]), 1)) {
            SSLfatal(sc, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return -1;
        }
        SSL3_RECORD_add_length(&wr, mac_size);
    }

    /* record length after mac and block padding */

    /* there's only one epoch between handshake and app data */

    s2n(sc->rlayer.d->w_epoch, pseq);

    memcpy(pseq, &(sc->rlayer.write_sequence[2]), 6);
    pseq += 6;
    s2n(SSL3_RECORD_get_length(&wr), pseq);

    if (sc->msg_callback)
        sc->msg_callback(1, 0, SSL3_RT_HEADER, pseq - DTLS1_RT_HEADER_LENGTH,
                         DTLS1_RT_HEADER_LENGTH, s, sc->msg_callback_arg);

    /*
     * we should now have wr.data pointing to the encrypted data, which is
     * wr->length long
     */
    SSL3_RECORD_set_type(&wr, templates->type); /* not needed but helps for debugging */
    SSL3_RECORD_add_length(&wr, DTLS1_RT_HEADER_LENGTH);

    ssl3_record_sequence_update(&(sc->rlayer.write_sequence[0]));

    /* now let's set up wb */
    SSL3_BUFFER_set_left(wb, SSL3_RECORD_get_length(&wr));
    SSL3_BUFFER_set_offset(wb, 0);

    /*
     * memorize arguments so that ssl3_write_pending can detect bad write
     * retries later
     */
    sc->rlayer.wpend_tot = templates->buflen;
    sc->rlayer.wpend_buf = templates->buf;
    sc->rlayer.wpend_type = templates->type;
    sc->rlayer.wpend_ret = templates->buflen;

    /* we now just need to write the buffer. Calls SSLfatal() as required. */
    return ssl3_write_pending(rl, templates->type, templates->buf,
                              templates->buflen, &written);
}

const OSSL_RECORD_METHOD ossl_dtls_record_method = {
    dtls_new_record_layer,
    dtls_free,
    tls_reset,
    tls_unprocessed_read_pending,
    tls_processed_read_pending,
    tls_app_data_pending,
    tls_write_pending,
    tls_get_max_record_len,
    tls_get_max_records,
    dtls_write_records,
    tls_retry_write_records,
    tls_read_record,
    tls_release_record,
    tls_get_alert_code,
    tls_set1_bio,
    tls_set_protocol_version,
    NULL,
    tls_set_first_handshake,
    tls_set_max_pipelines,
    dtls_set_in_init,
    tls_get_state,
    tls_set_options,
    tls_get_compression,
    tls_set_max_frag_len
};
