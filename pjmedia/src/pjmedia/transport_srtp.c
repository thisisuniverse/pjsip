/* $Id$ */
/*
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <pjmedia/transport_srtp.h>
#include <pjmedia/endpoint.h>
#include <pjlib-util/base64.h>
#include <pj/array.h>
#include <pj/assert.h>
#include <pj/ctype.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/pool.h>

#if defined(PJMEDIA_HAS_SRTP) && (PJMEDIA_HAS_SRTP != 0)

#if defined(PJ_HAS_SSL_SOCK) && (PJ_HAS_SSL_SOCK != 0)
#  include <openssl/rand.h>
#  include <openssl/opensslv.h>

/* Suppress compile warning of OpenSSL deprecation (OpenSSL is deprecated
 * since MacOSX 10.7).
 */
#if defined(PJ_DARWINOS) && PJ_DARWINOS==1
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#endif

#if defined(PJMEDIA_EXTERNAL_SRTP) && (PJMEDIA_EXTERNAL_SRTP != 0)
#  include <srtp/srtp.h>
#  include <srtp/crypto_kernel.h>
#else
#  include <srtp.h>
#  include <crypto_kernel.h>
#endif

#define THIS_FILE   "transport_srtp.c"

/* Maximum size of outgoing packet */
#define MAX_RTP_BUFFER_LEN	    PJMEDIA_MAX_MTU
#define MAX_RTCP_BUFFER_LEN	    PJMEDIA_MAX_MTU

/* Maximum SRTP crypto key length */
#define MAX_KEY_LEN		    128

/* Initial value of probation counter. When probation counter > 0,
 * it means SRTP is in probation state, and it may restart when
 * srtp_unprotect() returns err_status_replay_*
 */
#define PROBATION_CNT_INIT	    100

#define DEACTIVATE_MEDIA(pool, m)   pjmedia_sdp_media_deactivate(pool, m)

static const pj_str_t ID_RTP_AVP  = { "RTP/AVP", 7 };
static const pj_str_t ID_RTP_SAVP = { "RTP/SAVP", 8 };
static const pj_str_t ID_INACTIVE = { "inactive", 8 };
static const pj_str_t ID_CRYPTO   = { "crypto", 6 };

typedef void (*crypto_method_t)(srtp_crypto_policy_t *policy);

typedef struct crypto_suite
{
    char		*name;
    srtp_cipher_type_id_t cipher_type;
    unsigned		 cipher_key_len;    /* key + salt length    */
    unsigned		 cipher_salt_len;   /* salt only length	    */
    srtp_auth_type_id_t	 auth_type;
    unsigned		 auth_key_len;
    unsigned		 srtp_auth_tag_len;
    unsigned		 srtcp_auth_tag_len;
    srtp_sec_serv_t	 service;
    /* This is an attempt to validate crypto support by libsrtp, i.e: it should
     * raise linking error if the libsrtp does not support the crypto. 
     */
    srtp_cipher_type_t  *ext_cipher_type;
    crypto_method_t      ext_crypto_method;
} crypto_suite;

extern srtp_cipher_type_t srtp_aes_gcm_256_openssl;
extern srtp_cipher_type_t srtp_aes_gcm_128_openssl;
extern srtp_cipher_type_t srtp_aes_icm_192;

/* https://www.iana.org/assignments/sdp-security-descriptions/sdp-security-descriptions.xhtml */
static crypto_suite crypto_suites[] = {
    /* plain RTP/RTCP (no cipher & no auth) */
    {"NULL", SRTP_NULL_CIPHER, 0, SRTP_NULL_AUTH, 0, 0, 0, sec_serv_none},

#if defined(PJMEDIA_SRTP_HAS_AES_GCM_256)&&(PJMEDIA_SRTP_HAS_AES_GCM_256!=0)

    /* cipher AES_GCM, NULL auth, auth tag len = 16 octets */
    {"AEAD_AES_256_GCM", SRTP_AES_GCM_256, 44, 12,
	SRTP_NULL_AUTH, 0, 16, 16, sec_serv_conf_and_auth,
	&srtp_aes_gcm_256_openssl},

    /* cipher AES_GCM, NULL auth, auth tag len = 8 octets */
    {"AEAD_AES_256_GCM_8", SRTP_AES_GCM_256, 44, 12,
	SRTP_NULL_AUTH, 0, 8, 8, sec_serv_conf_and_auth,
	&srtp_aes_gcm_256_openssl},
#endif
#if defined(PJMEDIA_SRTP_HAS_AES_CM_256)&&(PJMEDIA_SRTP_HAS_AES_CM_256!=0)

    /* cipher AES_CM_256, auth SRTP_HMAC_SHA1, auth tag len = 10 octets */
    {"AES_256_CM_HMAC_SHA1_80", SRTP_AES_ICM_256, 46, 14,
	SRTP_HMAC_SHA1, 20, 10, 10, sec_serv_conf_and_auth,
	NULL, &srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80},

    /* cipher AES_CM_256, auth SRTP_HMAC_SHA1, auth tag len = 10 octets */
    {"AES_256_CM_HMAC_SHA1_32", SRTP_AES_ICM_256, 46, 14,
	SRTP_HMAC_SHA1, 20, 4, 10, sec_serv_conf_and_auth,
	NULL, &srtp_crypto_policy_set_aes_cm_256_hmac_sha1_32},
#endif
#if defined(PJMEDIA_SRTP_HAS_AES_CM_192)&&(PJMEDIA_SRTP_HAS_AES_CM_192!=0)

    /* cipher AES_CM_192, auth SRTP_HMAC_SHA1, auth tag len = 10 octets */
    {"AES_192_CM_HMAC_SHA1_80", SRTP_AES_ICM_192, 38, 14,
	SRTP_HMAC_SHA1, 20, 10, 10, sec_serv_conf_and_auth,
	&srtp_aes_icm_192},

    /* cipher AES_CM_192, auth SRTP_HMAC_SHA1, auth tag len = 4 octets */
    {"AES_192_CM_HMAC_SHA1_32", SRTP_AES_ICM_192, 38, 14,
	SRTP_HMAC_SHA1, 20, 4, 10, sec_serv_conf_and_auth,
	&srtp_aes_icm_192},
#endif
#if defined(PJMEDIA_SRTP_HAS_AES_GCM_128)&&(PJMEDIA_SRTP_HAS_AES_GCM_128!=0)

    /* cipher AES_GCM, NULL auth, auth tag len = 16 octets */
    {"AEAD_AES_128_GCM", SRTP_AES_GCM_128, 28, 12,
	SRTP_NULL_AUTH, 0, 16, 16, sec_serv_conf_and_auth,
	&srtp_aes_gcm_128_openssl},

    /* cipher AES_GCM, NULL auth, auth tag len = 8 octets */
    {"AEAD_AES_128_GCM_8", SRTP_AES_GCM_128, 28, 12,
	SRTP_NULL_AUTH, 0, 8, 8, sec_serv_conf_and_auth,
	&srtp_aes_gcm_128_openssl},
#endif
#if defined(PJMEDIA_SRTP_HAS_AES_CM_128)&&(PJMEDIA_SRTP_HAS_AES_CM_128!=0)

    /* cipher AES_CM_128, auth SRTP_HMAC_SHA1, auth tag len = 10 octets */
    {"AES_CM_128_HMAC_SHA1_80", SRTP_AES_ICM_128, 30, 14,
	SRTP_HMAC_SHA1, 20, 10, 10, sec_serv_conf_and_auth},

    /* cipher AES_CM_128, auth SRTP_HMAC_SHA1, auth tag len = 4 octets */
    {"AES_CM_128_HMAC_SHA1_32", SRTP_AES_ICM_128, 30, 14,
	SRTP_HMAC_SHA1, 20, 4, 10, sec_serv_conf_and_auth},
#endif

    /*
     * F8_128_HMAC_SHA1_8 not supported by libsrtp?
     * {"F8_128_HMAC_SHA1_8", NULL_CIPHER, 0, 0, NULL_AUTH, 0, 0, 0,
     *	sec_serv_none}
     */
};


/* SRTP transport */
typedef struct transport_srtp
{
    pjmedia_transport	 base;		    /**< Base transport interface.  */
    pj_pool_t		*pool;		    /**< Pool for transport SRTP.   */
    pj_lock_t		*mutex;		    /**< Mutex for libsrtp contexts.*/
    char		 rtp_tx_buffer[MAX_RTP_BUFFER_LEN];
    char		 rtcp_tx_buffer[MAX_RTCP_BUFFER_LEN];
    pjmedia_srtp_setting setting;
    unsigned		 media_option;

    /* SRTP policy */
    pj_bool_t		 session_inited;
    pj_bool_t		 offerer_side;
    pj_bool_t		 bypass_srtp;
    char		 tx_key[MAX_KEY_LEN];
    char		 rx_key[MAX_KEY_LEN];
    pjmedia_srtp_crypto  tx_policy;
    pjmedia_srtp_crypto  rx_policy;

    /* Temporary policy for negotiation */
    pjmedia_srtp_crypto  tx_policy_neg;
    pjmedia_srtp_crypto  rx_policy_neg;

    /* libSRTP contexts */
    srtp_t		 srtp_tx_ctx;
    srtp_t		 srtp_rx_ctx;

    /* Stream information */
    void		*user_data;
    void		(*rtp_cb)( void *user_data,
				   void *pkt,
				   pj_ssize_t size);
    void		(*rtcp_cb)(void *user_data,
				   void *pkt,
				   pj_ssize_t size);

    /* Transport information */
    pjmedia_transport	*member_tp; /**< Underlying transport.       */
    pj_bool_t		 member_tp_attached;

    /* SRTP usage policy of peer. This field is updated when media is starting.
     * This is useful when SRTP is in optional mode and peer is using mandatory
     * mode, so when local is about to reinvite/update, it should offer
     * RTP/SAVP instead of offering RTP/AVP.
     */
    pjmedia_srtp_use	 peer_use;

    /* When probation counter > 0, it means SRTP is in probation state,
     * and it may restart when srtp_unprotect() returns err_status_replay_*
     */
    unsigned		 probation_cnt;

    /* SRTP keying methods. The keying is implemented using media transport
     * abstraction, so it will also be invoked when the SRTP media transport
     * operation is invoked.
     *
     * As there can be multiple keying methods enabled (currently only SDES &
     * DTLS-SRTP), each keying method will be given the chance to respond to
     * remote SDP. If any keying operation returns non-success, it will be
     * removed from the session. And once SRTP key is obtained via a keying
     * method, any other keying methods will be stopped and destroyed.
     */
    unsigned		 keying_cnt;
    pjmedia_transport	*keying[2];

    /* If not zero, keying nego is ongoing (async-ly, e.g: by DTLS-SRTP).
     * This field may be updated by keying method.
     */
    unsigned		 keying_pending_cnt;

} transport_srtp;


/*
 * This callback is called by transport when incoming rtp is received
 */
static void srtp_rtp_cb( void *user_data, void *pkt, pj_ssize_t size);

/*
 * This callback is called by transport when incoming rtcp is received
 */
static void srtp_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size);


/*
 * These are media transport operations.
 */
static pj_status_t transport_get_info (pjmedia_transport *tp,
				       pjmedia_transport_info *info);
//static pj_status_t transport_attach   (pjmedia_transport *tp,
//				       void *user_data,
//				       const pj_sockaddr_t *rem_addr,
//				       const pj_sockaddr_t *rem_rtcp,
//				       unsigned addr_len,
//				       void (*rtp_cb)(void*,
//						      void*,
//						      pj_ssize_t),
//				       void (*rtcp_cb)(void*,
//						       void*,
//						       pj_ssize_t));
static void	   transport_detach   (pjmedia_transport *tp,
				       void *strm);
static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
				       const pj_sockaddr_t *addr,
				       unsigned addr_len,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_media_create(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       unsigned options,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_media_start (pjmedia_transport *tp,
				       pj_pool_t *pool,
				       const pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_media_stop(pjmedia_transport *tp);
static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
				       pjmedia_dir dir,
				       unsigned pct_lost);
static pj_status_t transport_destroy  (pjmedia_transport *tp);
static pj_status_t transport_attach2  (pjmedia_transport *tp,
				       pjmedia_transport_attach_param *param);



static pjmedia_transport_op transport_srtp_op =
{
    &transport_get_info,
    NULL, //&transport_attach,
    &transport_detach,
    &transport_send_rtp,
    &transport_send_rtcp,
    &transport_send_rtcp2,
    &transport_media_create,
    &transport_encode_sdp,
    &transport_media_start,
    &transport_media_stop,
    &transport_simulate_lost,
    &transport_destroy,
    &transport_attach2
};

/* Get crypto index from crypto name */
static int get_crypto_idx(const pj_str_t* crypto_name);

/* Is crypto empty (i.e: no name or key)? */
static pj_bool_t srtp_crypto_empty(const pjmedia_srtp_crypto* c);

/* Compare crypto, return zero if same */
static int srtp_crypto_cmp(const pjmedia_srtp_crypto* c1,
			   const pjmedia_srtp_crypto* c2);

/* Start SRTP */
static pj_status_t start_srtp(transport_srtp *srtp);


/* This function may also be used by other module, e.g: pjmedia/errno.c,
 * it should have C compatible declaration.
 */
PJ_BEGIN_DECL
    const char* get_libsrtp_errstr(int err);
PJ_END_DECL

const char* get_libsrtp_errstr(int err)
{
#if defined(PJ_HAS_ERROR_STRING) && (PJ_HAS_ERROR_STRING != 0)
    static char *liberr[] = {
	"ok",				    /* srtp_err_status_ok       = 0  */
	"unspecified failure",		    /* err_status_fail          = 1  */
	"unsupported parameter",	    /* err_status_bad_param     = 2  */
	"couldn't allocate memory",	    /* err_status_alloc_fail    = 3  */
	"couldn't deallocate properly",	    /* err_status_dealloc_fail  = 4  */
	"couldn't initialize",		    /* err_status_init_fail     = 5  */
	"can't process as much data as requested",
					    /* err_status_terminus      = 6  */
	"authentication failure",	    /* err_status_auth_fail     = 7  */
	"cipher failure",		    /* err_status_cipher_fail   = 8  */
	"replay check failed (bad index)",  /* err_status_replay_fail   = 9  */
	"replay check failed (index too old)",
					    /* err_status_replay_old    = 10 */
	"algorithm failed test routine",    /* err_status_algo_fail     = 11 */
	"unsupported operation",	    /* err_status_no_such_op    = 12 */
	"no appropriate context found",	    /* err_status_no_ctx        = 13 */
	"unable to perform desired validation",
					    /* err_status_cant_check    = 14 */
	"can't use key any more",	    /* err_status_key_expired   = 15 */
	"error in use of socket",	    /* err_status_socket_err    = 16 */
	"error in use POSIX signals",	    /* err_status_signal_err    = 17 */
	"nonce check failed",		    /* err_status_nonce_bad     = 18 */
	"couldn't read data",		    /* err_status_read_fail     = 19 */
	"couldn't write data",		    /* err_status_write_fail    = 20 */
	"error pasring data",		    /* err_status_parse_err     = 21 */
	"error encoding data",		    /* err_status_encode_err    = 22 */
	"error while using semaphores",	    /* err_status_semaphore_err = 23 */
	"error while using pfkey"	    /* err_status_pfkey_err     = 24 */
    };
    if (err >= 0 && err < (int)PJ_ARRAY_SIZE(liberr)) {
	return liberr[err];
    } else {
	static char msg[32];
	pj_ansi_snprintf(msg, sizeof(msg), "Unknown libsrtp error %d", err);
	return msg;
    }
#else
    static char msg[32];
    pj_ansi_snprintf(msg, sizeof(msg), "libsrtp error %d", err);
    return msg;
#endif
}

/* SRTP keying method: Session Description */
#if defined(PJMEDIA_SRTP_HAS_SDES) && (PJMEDIA_SRTP_HAS_SDES != 0)
#  include "transport_srtp_sdes.c"
#endif

/* SRTP keying method: DTLS */
#if defined(PJMEDIA_SRTP_HAS_DTLS) && (PJMEDIA_SRTP_HAS_DTLS != 0)
#  include "transport_srtp_dtls.c"
#else
PJ_DEF(pj_status_t) pjmedia_transport_srtp_dtls_start_nego(
				pjmedia_transport *srtp,
				const pjmedia_srtp_dtls_nego_param *param)
{
    PJ_UNUSED_ARG(srtp);
    PJ_UNUSED_ARG(param);
    return PJ_ENOTSUP;
}
PJ_DEF(pj_status_t) pjmedia_transport_srtp_dtls_get_fingerprint(
				pjmedia_transport *srtp,
				const char *hash,
				char *buf, pj_size_t *len)
{
    PJ_UNUSED_ARG(srtp);
    PJ_UNUSED_ARG(hash);
    PJ_UNUSED_ARG(buf);
    PJ_UNUSED_ARG(len);
    return PJ_ENOTSUP;
}
#endif


static pj_bool_t libsrtp_initialized;
static void pjmedia_srtp_deinit_lib(pjmedia_endpt *endpt);

PJ_DEF(pj_status_t) pjmedia_srtp_init_lib(pjmedia_endpt *endpt)
{
    pj_status_t status = PJ_SUCCESS;

    if (libsrtp_initialized)
	return PJ_SUCCESS;

#if PJMEDIA_LIBSRTP_AUTO_INIT_DEINIT
    /* Init libsrtp */
    {
	srtp_err_status_t err;

	err = srtp_init();
	if (err != srtp_err_status_ok) {
	    PJ_LOG(4, (THIS_FILE, "Failed to initialize libsrtp: %s",
		       get_libsrtp_errstr(err)));
	    return PJMEDIA_ERRNO_FROM_LIBSRTP(err);
	}
    }
#endif

#if defined(PJMEDIA_SRTP_HAS_DTLS) && (PJMEDIA_SRTP_HAS_DTLS != 0)
    dtls_init();
#endif

    if (pjmedia_endpt_atexit(endpt, pjmedia_srtp_deinit_lib) != PJ_SUCCESS)
    {
	/* There will be memory leak when it fails to schedule libsrtp
	 * deinitialization, however the memory leak could be harmless,
	 * since in modern OS's memory used by an application is released
	 * when the application terminates.
	 */
	PJ_LOG(4, (THIS_FILE, "Failed to register libsrtp deinit."));
    }

    libsrtp_initialized = PJ_TRUE;

    return status;
}

static void pjmedia_srtp_deinit_lib(pjmedia_endpt *endpt)
{
    srtp_err_status_t err;

    /* Note that currently this SRTP init/deinit is not equipped with
     * reference counter, it should be safe as normally there is only
     * one single instance of media endpoint and even if it isn't, the
     * pjmedia_transport_srtp_create() will invoke SRTP init (the only
     * drawback should be the delay described by #788).
     */

    PJ_UNUSED_ARG(endpt);

#if !defined(PJMEDIA_SRTP_HAS_DEINIT) && !defined(PJMEDIA_SRTP_HAS_SHUTDOWN)
# define PJMEDIA_SRTP_HAS_SHUTDOWN 1
#endif

#if PJMEDIA_LIBSRTP_AUTO_INIT_DEINIT

# if defined(PJMEDIA_SRTP_HAS_DEINIT) && PJMEDIA_SRTP_HAS_DEINIT!=0
    err = srtp_deinit();
# elif defined(PJMEDIA_SRTP_HAS_SHUTDOWN) && PJMEDIA_SRTP_HAS_SHUTDOWN!=0
    err = srtp_shutdown();
# else
    err = srtp_err_status_ok;
# endif
    if (err != srtp_err_status_ok) {
	PJ_LOG(4, (THIS_FILE, "Failed to deinitialize libsrtp: %s",
		   get_libsrtp_errstr(err)));
    }
#endif // PJMEDIA_LIBSRTP_AUTO_INIT_DEINIT

#if defined(PJMEDIA_SRTP_HAS_DTLS) && (PJMEDIA_SRTP_HAS_DTLS != 0)
    dtls_deinit();
#endif

    libsrtp_initialized = PJ_FALSE;
}


static int get_crypto_idx(const pj_str_t* crypto_name)
{
    int i;
    int cs_cnt = sizeof(crypto_suites)/sizeof(crypto_suites[0]);

    /* treat unspecified crypto_name as crypto 'NULL' */
    if (crypto_name->slen == 0)
	return 0;

    for (i=0; i<cs_cnt; ++i) {
	if (!pj_stricmp2(crypto_name, crypto_suites[i].name))
	    return i;
    }

    return -1;
}


static int srtp_crypto_cmp(const pjmedia_srtp_crypto* c1,
			   const pjmedia_srtp_crypto* c2)
{
    int r;

    r = pj_strcmp(&c1->key, &c2->key);
    if (r != 0)
	return r;

    r = pj_stricmp(&c1->name, &c2->name);
    if (r != 0)
	return r;

    return (c1->flags != c2->flags);
}


static pj_bool_t srtp_crypto_empty(const pjmedia_srtp_crypto* c)
{
    return (c->name.slen==0 || c->key.slen==0);
}


PJ_DEF(void) pjmedia_srtp_setting_default(pjmedia_srtp_setting *opt)
{
    unsigned i;

    pj_assert(opt);

    pj_bzero(opt, sizeof(pjmedia_srtp_setting));
    opt->close_member_tp = PJ_TRUE;
    opt->use = PJMEDIA_SRTP_OPTIONAL;

    /* Copy default crypto-suites, but skip crypto 'NULL' */
    opt->crypto_count = sizeof(crypto_suites)/sizeof(crypto_suites[0]) - 1;
    for (i=0; i<opt->crypto_count; ++i)
	opt->crypto[i].name = pj_str(crypto_suites[i+1].name);

    /* Keying method */
    opt->keying_count = PJMEDIA_SRTP_KEYINGS_COUNT;
    opt->keying[0] = PJMEDIA_SRTP_KEYING_SDES;
    opt->keying[1] = PJMEDIA_SRTP_KEYING_DTLS_SRTP;

    /* Just for reminder to add any new keying to the array above */
    pj_assert(PJMEDIA_SRTP_KEYINGS_COUNT == 2);
}


/*
 * Create an SRTP media transport.
 */
PJ_DEF(pj_status_t) pjmedia_transport_srtp_create(
				       pjmedia_endpt *endpt,
				       pjmedia_transport *tp,
				       const pjmedia_srtp_setting *opt,
				       pjmedia_transport **p_tp)
{
    pj_pool_t *pool;
    transport_srtp *srtp;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(endpt && tp && p_tp, PJ_EINVAL);

    /* Check crypto availability */
    if (opt && opt->crypto_count == 0 &&
	opt->use == PJMEDIA_SRTP_MANDATORY)
	return PJMEDIA_SRTP_ESDPREQCRYPTO;

    /* Check crypto */
    if (opt && opt->use != PJMEDIA_SRTP_DISABLED) {
	for (i=0; i < opt->crypto_count; ++i) {
	    int cs_idx = get_crypto_idx(&opt->crypto[i].name);

	    /* check crypto name */
	    if (cs_idx == -1)
		return PJMEDIA_SRTP_ENOTSUPCRYPTO;

	    /* check key length */
	    if (opt->crypto[i].key.slen &&
		opt->crypto[i].key.slen <
		(pj_ssize_t)crypto_suites[cs_idx].cipher_key_len)
		return PJMEDIA_SRTP_EINKEYLEN;
	}
    }

    /* Init libsrtp. */
    status = pjmedia_srtp_init_lib(endpt);
    if (status != PJ_SUCCESS)
	return status;

    pool = pjmedia_endpt_create_pool(endpt, "srtp%p", 1000, 1000);
    srtp = PJ_POOL_ZALLOC_T(pool, transport_srtp);

    srtp->pool = pool;
    srtp->session_inited = PJ_FALSE;
    srtp->bypass_srtp = PJ_FALSE;
    srtp->probation_cnt = PROBATION_CNT_INIT;

    if (opt) {
	srtp->setting = *opt;
	if (opt->use == PJMEDIA_SRTP_DISABLED)
	    srtp->setting.crypto_count = 0;

	for (i=0; i < srtp->setting.crypto_count; ++i) {
	    int cs_idx = get_crypto_idx(&opt->crypto[i].name);
	    pj_str_t tmp_key = opt->crypto[i].key;

	    /* re-set crypto */
	    srtp->setting.crypto[i].name = pj_str(crypto_suites[cs_idx].name);
	    /* cut key length */
	    if (tmp_key.slen)
		tmp_key.slen = crypto_suites[cs_idx].cipher_key_len;
	    pj_strdup(pool, &srtp->setting.crypto[i].key, &tmp_key);
	}
    } else {
	pjmedia_srtp_setting_default(&srtp->setting);
    }

    status = pj_lock_create_recursive_mutex(pool, pool->obj_name, &srtp->mutex);
    if (status != PJ_SUCCESS) {
	pj_pool_release(pool);
	return status;
    }

    /* Initialize base pjmedia_transport */
    pj_memcpy(srtp->base.name, pool->obj_name, PJ_MAX_OBJ_NAME);
    if (tp)
	srtp->base.type = tp->type;
    else
	srtp->base.type = PJMEDIA_TRANSPORT_TYPE_UDP;
    srtp->base.op = &transport_srtp_op;
    srtp->base.user_data = srtp->setting.user_data;

    /* Set underlying transport */
    srtp->member_tp = tp;

    /* Initialize peer's SRTP usage mode. */
    srtp->peer_use = srtp->setting.use;

    /* Initialize SRTP keying method. */
    for (i = 0; i < srtp->setting.keying_count; ++i) {
	switch(srtp->setting.keying[i]) {

	case PJMEDIA_SRTP_KEYING_SDES:
#if defined(PJMEDIA_SRTP_HAS_SDES) && (PJMEDIA_SRTP_HAS_SDES != 0)
	    sdes_create(srtp, &srtp->keying[srtp->keying_cnt++]);
#endif
	    break;

	case PJMEDIA_SRTP_KEYING_DTLS_SRTP:
#if defined(PJMEDIA_SRTP_HAS_DTLS) && (PJMEDIA_SRTP_HAS_DTLS != 0)
	    dtls_create(srtp, &srtp->keying[srtp->keying_cnt++]);
#endif
	    break;

	default:
	    break;
	}
    }

    /* Done */
    *p_tp = &srtp->base;

    return PJ_SUCCESS;
}


/*
 * Initialize and start SRTP session with the given parameters.
 */
PJ_DEF(pj_status_t) pjmedia_transport_srtp_start(
			   pjmedia_transport *tp,
			   const pjmedia_srtp_crypto *tx,
			   const pjmedia_srtp_crypto *rx)
{
    transport_srtp  *srtp = (transport_srtp*) tp;
    srtp_policy_t    tx_;
    srtp_policy_t    rx_;
    srtp_err_status_t err;
    int		     cr_tx_idx = 0;
    int		     au_tx_idx = 0;
    int		     cr_rx_idx = 0;
    int		     au_rx_idx = 0;
    pj_status_t	     status = PJ_SUCCESS;

    PJ_ASSERT_RETURN(tp && tx && rx, PJ_EINVAL);

    pj_lock_acquire(srtp->mutex);

    if (srtp->session_inited) {
	pjmedia_transport_srtp_stop(tp);
    }

    /* Get encryption and authentication method */
    cr_tx_idx = au_tx_idx = get_crypto_idx(&tx->name);
    if (tx->flags & PJMEDIA_SRTP_NO_ENCRYPTION)
	cr_tx_idx = 0;
    if (tx->flags & PJMEDIA_SRTP_NO_AUTHENTICATION)
	au_tx_idx = 0;

    cr_rx_idx = au_rx_idx = get_crypto_idx(&rx->name);
    if (rx->flags & PJMEDIA_SRTP_NO_ENCRYPTION)
	cr_rx_idx = 0;
    if (rx->flags & PJMEDIA_SRTP_NO_AUTHENTICATION)
	au_rx_idx = 0;

    /* Check whether the crypto-suite requested is supported */
    if (cr_tx_idx == -1 || cr_rx_idx == -1 || au_tx_idx == -1 ||
	au_rx_idx == -1)
    {
	status = PJMEDIA_SRTP_ENOTSUPCRYPTO;
	goto on_return;
    }

    /* If all options points to 'NULL' method, just bypass SRTP */
    if (cr_tx_idx == 0 && cr_rx_idx == 0 && au_tx_idx == 0 && au_rx_idx == 0) {
	srtp->bypass_srtp = PJ_TRUE;
	goto on_return;
    }

    /* Check key length */
    if (tx->key.slen != (pj_ssize_t)crypto_suites[cr_tx_idx].cipher_key_len ||
        rx->key.slen != (pj_ssize_t)crypto_suites[cr_rx_idx].cipher_key_len)
    {
	status = PJMEDIA_SRTP_EINKEYLEN;
	goto on_return;
    }

    /* Init transmit direction */
    pj_bzero(&tx_, sizeof(srtp_policy_t));
    pj_memmove(srtp->tx_key, tx->key.ptr, tx->key.slen);
    if (cr_tx_idx && au_tx_idx)
	tx_.rtp.sec_serv    = sec_serv_conf_and_auth;
    else if (cr_tx_idx)
	tx_.rtp.sec_serv    = sec_serv_conf;
    else if (au_tx_idx)
	tx_.rtp.sec_serv    = sec_serv_auth;
    else
	tx_.rtp.sec_serv    = sec_serv_none;
    tx_.key		    = (uint8_t*)srtp->tx_key;
    tx_.ssrc.type	    = ssrc_any_outbound;
    tx_.ssrc.value	    = 0;
    tx_.rtp.cipher_type	    = crypto_suites[cr_tx_idx].cipher_type;
    tx_.rtp.cipher_key_len  = crypto_suites[cr_tx_idx].cipher_key_len;
    tx_.rtp.auth_type	    = crypto_suites[au_tx_idx].auth_type;
    tx_.rtp.auth_key_len    = crypto_suites[au_tx_idx].auth_key_len;
    tx_.rtp.auth_tag_len    = crypto_suites[au_tx_idx].srtp_auth_tag_len;
    tx_.rtcp		    = tx_.rtp;
    tx_.rtcp.auth_tag_len   = crypto_suites[au_tx_idx].srtcp_auth_tag_len;
    tx_.next		    = NULL;
    err = srtp_create(&srtp->srtp_tx_ctx, &tx_);
    if (err != srtp_err_status_ok) {
	status = PJMEDIA_ERRNO_FROM_LIBSRTP(err);
	goto on_return;
    }
    srtp->tx_policy = *tx;
    pj_strset(&srtp->tx_policy.key,  srtp->tx_key, tx->key.slen);
    srtp->tx_policy.name=pj_str(crypto_suites[get_crypto_idx(&tx->name)].name);


    /* Init receive direction */
    pj_bzero(&rx_, sizeof(srtp_policy_t));
    pj_memmove(srtp->rx_key, rx->key.ptr, rx->key.slen);
    if (cr_rx_idx && au_rx_idx)
	rx_.rtp.sec_serv    = sec_serv_conf_and_auth;
    else if (cr_rx_idx)
	rx_.rtp.sec_serv    = sec_serv_conf;
    else if (au_rx_idx)
	rx_.rtp.sec_serv    = sec_serv_auth;
    else
	rx_.rtp.sec_serv    = sec_serv_none;
    rx_.key		    = (uint8_t*)srtp->rx_key;
    rx_.ssrc.type	    = ssrc_any_inbound;
    rx_.ssrc.value	    = 0;
    rx_.rtp.sec_serv	    = crypto_suites[cr_rx_idx].service;
    rx_.rtp.cipher_type	    = crypto_suites[cr_rx_idx].cipher_type;
    rx_.rtp.cipher_key_len  = crypto_suites[cr_rx_idx].cipher_key_len;
    rx_.rtp.auth_type	    = crypto_suites[au_rx_idx].auth_type;
    rx_.rtp.auth_key_len    = crypto_suites[au_rx_idx].auth_key_len;
    rx_.rtp.auth_tag_len    = crypto_suites[au_rx_idx].srtp_auth_tag_len;
    rx_.rtcp		    = rx_.rtp;
    rx_.rtcp.auth_tag_len   = crypto_suites[au_rx_idx].srtcp_auth_tag_len;
    rx_.next		    = NULL;
    err = srtp_create(&srtp->srtp_rx_ctx, &rx_);
    if (err != srtp_err_status_ok) {
	srtp_dealloc(srtp->srtp_tx_ctx);
	status = PJMEDIA_ERRNO_FROM_LIBSRTP(err);
	goto on_return;
    }
    srtp->rx_policy = *rx;
    pj_strset(&srtp->rx_policy.key,  srtp->rx_key, rx->key.slen);
    srtp->rx_policy.name=pj_str(crypto_suites[get_crypto_idx(&rx->name)].name);

    /* Declare SRTP session initialized */
    srtp->session_inited = PJ_TRUE;

    /* Logging stuffs */
#if PJ_LOG_MAX_LEVEL >= 5
    {
	char b64[PJ_BASE256_TO_BASE64_LEN(MAX_KEY_LEN)];
	int b64_len;

	/* TX crypto and key */
	b64_len = sizeof(b64);
	status = pj_base64_encode((pj_uint8_t*)tx->key.ptr, tx->key.slen,
				  b64, &b64_len);
	if (status != PJ_SUCCESS)
	    b64_len = pj_ansi_sprintf(b64, "--key too long--");
	else
	    b64[b64_len] = '\0';

	PJ_LOG(5, (srtp->pool->obj_name, "TX: %s key=%s",
		   srtp->tx_policy.name.ptr, b64));
	if (srtp->tx_policy.flags) {
	    PJ_LOG(5,(srtp->pool->obj_name, "TX: disable%s%s",
		      (cr_tx_idx?"":" enc"),
		      (au_tx_idx?"":" auth")));
	}

	/* RX crypto and key */
	b64_len = sizeof(b64);
	status = pj_base64_encode((pj_uint8_t*)rx->key.ptr, rx->key.slen,
				  b64, &b64_len);
	if (status != PJ_SUCCESS)
	    b64_len = pj_ansi_sprintf(b64, "--key too long--");
	else
	    b64[b64_len] = '\0';

	PJ_LOG(5, (srtp->pool->obj_name, "RX: %s key=%s",
		   srtp->rx_policy.name.ptr, b64));
	if (srtp->rx_policy.flags) {
	    PJ_LOG(5,(srtp->pool->obj_name,"RX: disable%s%s",
		      (cr_rx_idx?"":" enc"),
		      (au_rx_idx?"":" auth")));
	}
    }
#endif

on_return:
    pj_lock_release(srtp->mutex);
    return status;
}

/*
 * Stop SRTP session.
 */
PJ_DEF(pj_status_t) pjmedia_transport_srtp_stop(pjmedia_transport *srtp)
{
    transport_srtp *p_srtp = (transport_srtp*) srtp;
    srtp_err_status_t err;

    PJ_ASSERT_RETURN(srtp, PJ_EINVAL);

    pj_lock_acquire(p_srtp->mutex);

    if (!p_srtp->session_inited) {
	pj_lock_release(p_srtp->mutex);
	return PJ_SUCCESS;
    }

    err = srtp_dealloc(p_srtp->srtp_rx_ctx);
    if (err != srtp_err_status_ok) {
	PJ_LOG(4, (p_srtp->pool->obj_name,
		   "Failed to dealloc RX SRTP context: %s",
		   get_libsrtp_errstr(err)));
    }
    err = srtp_dealloc(p_srtp->srtp_tx_ctx);
    if (err != srtp_err_status_ok) {
	PJ_LOG(4, (p_srtp->pool->obj_name,
		   "Failed to dealloc TX SRTP context: %s",
		   get_libsrtp_errstr(err)));
    }

    p_srtp->session_inited = PJ_FALSE;
    pj_bzero(&p_srtp->rx_policy, sizeof(p_srtp->rx_policy));
    pj_bzero(&p_srtp->tx_policy, sizeof(p_srtp->tx_policy));

    pj_lock_release(p_srtp->mutex);

    return PJ_SUCCESS;
}


static pj_status_t start_srtp(transport_srtp *srtp)
{
    /* Make sure we have the SRTP policies */
    if (srtp_crypto_empty(&srtp->tx_policy_neg) ||
	srtp_crypto_empty(&srtp->rx_policy_neg))
    {
	srtp->bypass_srtp = PJ_TRUE;
	srtp->peer_use = PJMEDIA_SRTP_DISABLED;
	if (srtp->session_inited) {
	    pjmedia_transport_srtp_stop(&srtp->base);
	}

	return PJ_SUCCESS;
    }

    /* Reset probation counts */
    srtp->probation_cnt = PROBATION_CNT_INIT;

    /* Got policy_local & policy_remote, let's initalize the SRTP */

    /* Ticket #1075: media_start() is called whenever media description
     * gets updated, e.g: call hold, however we should restart SRTP only
     * when the SRTP policy settings are updated.
     */
    if (srtp_crypto_cmp(&srtp->tx_policy_neg, &srtp->tx_policy) ||
	srtp_crypto_cmp(&srtp->rx_policy_neg, &srtp->rx_policy))
    {
	pj_status_t status;
	status = pjmedia_transport_srtp_start(&srtp->base,
					      &srtp->tx_policy_neg,
					      &srtp->rx_policy_neg);
	if (status != PJ_SUCCESS)
	    return status;
    }

    srtp->bypass_srtp = PJ_FALSE;

    return PJ_SUCCESS;
}


PJ_DEF(pjmedia_transport *) pjmedia_transport_srtp_get_member(
						pjmedia_transport *tp)
{
    transport_srtp *srtp = (transport_srtp*) tp;

    PJ_ASSERT_RETURN(tp, NULL);

    return srtp->member_tp;
}


static pj_status_t transport_get_info(pjmedia_transport *tp,
				      pjmedia_transport_info *info)
{
    transport_srtp *srtp = (transport_srtp*) tp;
    pjmedia_srtp_info srtp_info;
    int spc_info_idx;
    unsigned i;

    PJ_ASSERT_RETURN(tp && info, PJ_EINVAL);
    PJ_ASSERT_RETURN(info->specific_info_cnt <
		     PJMEDIA_TRANSPORT_SPECIFIC_INFO_MAXCNT, PJ_ETOOMANY);
    PJ_ASSERT_RETURN(sizeof(pjmedia_srtp_info) <=
		     PJMEDIA_TRANSPORT_SPECIFIC_INFO_MAXSIZE, PJ_ENOMEM);

    srtp_info.active = srtp->session_inited;
    srtp_info.rx_policy = srtp->rx_policy;
    srtp_info.tx_policy = srtp->tx_policy;
    srtp_info.use = srtp->setting.use;
    srtp_info.peer_use = srtp->peer_use;

    spc_info_idx = info->specific_info_cnt++;
    info->spc_info[spc_info_idx].type = PJMEDIA_TRANSPORT_TYPE_SRTP;
    info->spc_info[spc_info_idx].cbsize = sizeof(srtp_info);
    pj_memcpy(&info->spc_info[spc_info_idx].buffer, &srtp_info,
	      sizeof(srtp_info));

    /* Invoke get_info() of all keying methods */
    for (i=0; i < srtp->keying_cnt; i++)
	pjmedia_transport_get_info(srtp->keying[i], info);

    return pjmedia_transport_get_info(srtp->member_tp, info);
}

static pj_status_t transport_attach2(pjmedia_transport *tp,
				     pjmedia_transport_attach_param *param)
{
    transport_srtp *srtp = (transport_srtp*) tp;
    pjmedia_transport_attach_param member_param;
    pj_status_t status;

    PJ_ASSERT_RETURN(tp && param, PJ_EINVAL);

    /* Save the callbacks */
    pj_lock_acquire(srtp->mutex);
    srtp->rtp_cb = param->rtp_cb;
    srtp->rtcp_cb = param->rtcp_cb;
    srtp->user_data = param->user_data;
    pj_lock_release(srtp->mutex);

    /* Attach self to member transport */
    member_param = *param;
    member_param.user_data = srtp;
    member_param.rtp_cb = &srtp_rtp_cb;
    member_param.rtcp_cb = &srtp_rtcp_cb;
    status = pjmedia_transport_attach2(srtp->member_tp, &member_param);
    if (status != PJ_SUCCESS) {
	pj_lock_acquire(srtp->mutex);
	srtp->rtp_cb = NULL;
	srtp->rtcp_cb = NULL;
	srtp->user_data = NULL;
	pj_lock_release(srtp->mutex);
	return status;
    }

    srtp->member_tp_attached = PJ_TRUE;
    return PJ_SUCCESS;
}

static void transport_detach(pjmedia_transport *tp, void *strm)
{
    transport_srtp *srtp = (transport_srtp*) tp;

    PJ_UNUSED_ARG(strm);
    PJ_ASSERT_ON_FAIL(tp, return);

    if (srtp->member_tp) {
	pjmedia_transport_detach(srtp->member_tp, srtp);
    }

    /* Clear up application infos from transport */
    pj_lock_acquire(srtp->mutex);
    srtp->rtp_cb = NULL;
    srtp->rtcp_cb = NULL;
    srtp->user_data = NULL;
    pj_lock_release(srtp->mutex);
    srtp->member_tp_attached = PJ_FALSE;
}

static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    pj_status_t status;
    transport_srtp *srtp = (transport_srtp*) tp;
    int len = (int)size;
    srtp_err_status_t err;

    if (srtp->bypass_srtp)
	return pjmedia_transport_send_rtp(srtp->member_tp, pkt, size);

    if (size > sizeof(srtp->rtp_tx_buffer) - 10)
	return PJ_ETOOBIG;

    pj_memcpy(srtp->rtp_tx_buffer, pkt, size);

    pj_lock_acquire(srtp->mutex);
    if (!srtp->session_inited) {
	pj_lock_release(srtp->mutex);
	return PJ_EINVALIDOP;
    }
    err = srtp_protect(srtp->srtp_tx_ctx, srtp->rtp_tx_buffer, &len);
    pj_lock_release(srtp->mutex);

    if (err == srtp_err_status_ok) {
	status = pjmedia_transport_send_rtp(srtp->member_tp,
					    srtp->rtp_tx_buffer, len);
    } else {
	status = PJMEDIA_ERRNO_FROM_LIBSRTP(err);
    }

    return status;
}

static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    return transport_send_rtcp2(tp, NULL, 0, pkt, size);
}

static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
				        const pj_sockaddr_t *addr,
				        unsigned addr_len,
				        const void *pkt,
				        pj_size_t size)
{
    pj_status_t status;
    transport_srtp *srtp = (transport_srtp*) tp;
    int len = (int)size;
    srtp_err_status_t err;

    if (srtp->bypass_srtp) {
	return pjmedia_transport_send_rtcp2(srtp->member_tp, addr, addr_len,
	                                    pkt, size);
    }

    if (size > sizeof(srtp->rtcp_tx_buffer) - 10)
	return PJ_ETOOBIG;

    pj_memcpy(srtp->rtcp_tx_buffer, pkt, size);

    pj_lock_acquire(srtp->mutex);
    if (!srtp->session_inited) {
	pj_lock_release(srtp->mutex);
	return PJ_EINVALIDOP;
    }
    err = srtp_protect_rtcp(srtp->srtp_tx_ctx, srtp->rtcp_tx_buffer, &len);
    pj_lock_release(srtp->mutex);

    if (err == srtp_err_status_ok) {
	status = pjmedia_transport_send_rtcp2(srtp->member_tp, addr, addr_len,
					      srtp->rtcp_tx_buffer, len);
    } else {
	status = PJMEDIA_ERRNO_FROM_LIBSRTP(err);
    }

    return status;
}


static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
					   pjmedia_dir dir,
					   unsigned pct_lost)
{
    transport_srtp *srtp = (transport_srtp *) tp;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    return pjmedia_transport_simulate_lost(srtp->member_tp, dir, pct_lost);
}

static pj_status_t transport_destroy  (pjmedia_transport *tp)
{
    transport_srtp *srtp = (transport_srtp *) tp;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    /* Close keying */
    for (i=0; i < srtp->keying_cnt; i++)
	pjmedia_transport_close(srtp->keying[i]);

    /* Close member if configured */
    if (srtp->setting.close_member_tp && srtp->member_tp) {
	pjmedia_transport_close(srtp->member_tp);
    }

    status = pjmedia_transport_srtp_stop(tp);

    /* In case mutex is being acquired by other thread */
    pj_lock_acquire(srtp->mutex);
    pj_lock_release(srtp->mutex);

    pj_lock_destroy(srtp->mutex);
    pj_pool_release(srtp->pool);

    return status;
}

/*
 * This callback is called by transport when incoming rtp is received
 */
static void srtp_rtp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_srtp *srtp = (transport_srtp *) user_data;
    int len = size;
    srtp_err_status_t err;
    void (*cb)(void*, void*, pj_ssize_t) = NULL;
    void *cb_data = NULL;

    if (srtp->bypass_srtp) {
	srtp->rtp_cb(srtp->user_data, pkt, size);
	return;
    }

    if (size < 0) {
	return;
    }

    /* Give the packet to keying first by invoking its send_rtp() op.
     * Yes, the usage of send_rtp() is rather hacky, but it is convenient
     * as the signature suits the purpose and it is ready to use
     * (no futher registration/setting needed), and it may never be used
     * by any keying method in the future.
     */
    {
	unsigned i;
	pj_status_t status;
	for (i=0; i < srtp->keying_cnt; i++) {
	    if (!srtp->keying[i]->op->send_rtp)
		continue;
	    status = pjmedia_transport_send_rtp(srtp->keying[i], pkt, size);
	    if (status != PJ_EIGNORED) {
		/* Packet is already consumed by the keying method */
		return;
	    }
	}
    }

    /* Make sure buffer is 32bit aligned */
    PJ_ASSERT_ON_FAIL( (((pj_ssize_t)pkt) & 0x03)==0, return );

    if (srtp->probation_cnt > 0)
	--srtp->probation_cnt;

    pj_lock_acquire(srtp->mutex);

    if (!srtp->session_inited) {
	pj_lock_release(srtp->mutex);
	return;
    }
    err = srtp_unprotect(srtp->srtp_rx_ctx, (pj_uint8_t*)pkt, &len);
    if (srtp->probation_cnt > 0 &&
	(err == srtp_err_status_replay_old ||
	 err == srtp_err_status_replay_fail))
    {
	/* Handle such condition that stream is updated (RTP seq is reinited
	 * & SRTP is restarted), but some old packets are still coming
	 * so SRTP is learning wrong RTP seq. While the newly inited RTP seq
	 * comes, SRTP thinks the RTP seq is replayed, so srtp_unprotect()
	 * will return err_status_replay_*. Restarting SRTP can resolve this.
	 */
	pjmedia_srtp_crypto tx, rx;
	pj_status_t status;

	tx = srtp->tx_policy;
	rx = srtp->rx_policy;
	status = pjmedia_transport_srtp_start((pjmedia_transport*)srtp,
					      &tx, &rx);
	if (status != PJ_SUCCESS) {
	    PJ_LOG(5,(srtp->pool->obj_name, "Failed to restart SRTP, err=%s",
		      get_libsrtp_errstr(err)));
	} else if (!srtp->bypass_srtp) {
	    err = srtp_unprotect(srtp->srtp_rx_ctx, (pj_uint8_t*)pkt, &len);
	}
    }

    if (err != srtp_err_status_ok) {
	PJ_LOG(5,(srtp->pool->obj_name,
		  "Failed to unprotect SRTP, pkt size=%d, err=%s",
		  size, get_libsrtp_errstr(err)));
    } else {
	cb = srtp->rtp_cb;
	cb_data = srtp->user_data;
    }

    pj_lock_release(srtp->mutex);

    if (cb) {
	(*cb)(cb_data, pkt, len);
    }
}

/*
 * This callback is called by transport when incoming rtcp is received
 */
static void srtp_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_srtp *srtp = (transport_srtp *) user_data;
    int len = size;
    srtp_err_status_t err;
    void (*cb)(void*, void*, pj_ssize_t) = NULL;
    void *cb_data = NULL;

    if (srtp->bypass_srtp) {
	srtp->rtcp_cb(srtp->user_data, pkt, size);
	return;
    }

    if (size < 0) {
	return;
    }

    /* Make sure buffer is 32bit aligned */
    PJ_ASSERT_ON_FAIL( (((pj_ssize_t)pkt) & 0x03)==0, return );

    pj_lock_acquire(srtp->mutex);

    if (!srtp->session_inited) {
	pj_lock_release(srtp->mutex);
	return;
    }
    err = srtp_unprotect_rtcp(srtp->srtp_rx_ctx, (pj_uint8_t*)pkt, &len);
    if (err != srtp_err_status_ok) {
	PJ_LOG(5,(srtp->pool->obj_name,
		  "Failed to unprotect SRTCP, pkt size=%d, err=%s",
		  size, get_libsrtp_errstr(err)));
    } else {
	cb = srtp->rtcp_cb;
	cb_data = srtp->user_data;
    }

    pj_lock_release(srtp->mutex);

    if (cb) {
	(*cb)(cb_data, pkt, len);
    }
}


static pj_status_t transport_media_create(pjmedia_transport *tp,
				          pj_pool_t *sdp_pool,
					  unsigned options,
				          const pjmedia_sdp_session *sdp_remote,
					  unsigned media_index)
{
    struct transport_srtp *srtp = (struct transport_srtp*) tp;
    unsigned member_tp_option;
    pj_status_t last_err_st = PJ_EBUG;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    pj_bzero(&srtp->rx_policy_neg, sizeof(srtp->rx_policy_neg));
    pj_bzero(&srtp->tx_policy_neg, sizeof(srtp->tx_policy_neg));

    srtp->media_option = member_tp_option = options;
    srtp->offerer_side = (sdp_remote == NULL);

    if (srtp->offerer_side && srtp->setting.use == PJMEDIA_SRTP_DISABLED)
	srtp->bypass_srtp = PJ_TRUE;
    else
	member_tp_option |= PJMEDIA_TPMED_NO_TRANSPORT_CHECKING;

    status = pjmedia_transport_media_create(srtp->member_tp, sdp_pool,
					    member_tp_option, sdp_remote,
					    media_index);
    if (status != PJ_SUCCESS || srtp->bypass_srtp)
	return status;

    /* Invoke media_create() of all keying methods */
    for (i=0; i < srtp->keying_cnt; ) {
	pj_status_t st;
	st = pjmedia_transport_media_create(srtp->keying[i], sdp_pool,
					    options, sdp_remote,
					    media_index);
	if (st != PJ_SUCCESS) {
	    /* This keying method returns error, remove it */
	    pj_array_erase(srtp->keying, sizeof(srtp->keying[0]),
			   srtp->keying_cnt, i);
	    srtp->keying_cnt--;
	    last_err_st = st;
	    continue;
	} else if (srtp->offerer_side) {
	    /* Currently we can send one keying only in outgoing offer */
	    srtp->keying[0] = srtp->keying[i];
	    srtp->keying_cnt = 1;
	    break;
	}

	++i;
    }

    /* All keying method failed to process remote SDP? */
    if (srtp->keying_cnt == 0)
	return last_err_st;

    return PJ_SUCCESS;
}

static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
					pj_pool_t *sdp_pool,
					pjmedia_sdp_session *sdp_local,
					const pjmedia_sdp_session *sdp_remote,
					unsigned media_index)
{
    struct transport_srtp *srtp = (struct transport_srtp*) tp;
    pj_status_t last_err_st = PJ_EBUG;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp && sdp_pool && sdp_local, PJ_EINVAL);

    pj_bzero(&srtp->rx_policy_neg, sizeof(srtp->rx_policy_neg));
    pj_bzero(&srtp->tx_policy_neg, sizeof(srtp->tx_policy_neg));

    srtp->offerer_side = (sdp_remote == NULL);

    status = pjmedia_transport_encode_sdp(srtp->member_tp, sdp_pool,
					  sdp_local, sdp_remote, media_index);
    if (status != PJ_SUCCESS || srtp->bypass_srtp)
	return status;

    /* Invoke encode_sdp() of all keying methods */
    for (i=0; i < srtp->keying_cnt; ) {
	pj_status_t st;
	st = pjmedia_transport_encode_sdp(srtp->keying[i], sdp_pool,
					  sdp_local, sdp_remote,
					  media_index);
	if (st != PJ_SUCCESS) {
	    /* This keying method returns error, remove it */
	    pj_array_erase(srtp->keying, sizeof(srtp->keying[0]),
			   srtp->keying_cnt, i);
	    srtp->keying_cnt--;
	    last_err_st = st;
	    continue;
	}

	if (!srtp_crypto_empty(&srtp->tx_policy_neg) &&
	    !srtp_crypto_empty(&srtp->rx_policy_neg))
	{
	    /* SRTP nego is done, let's destroy any other keying. */
	    unsigned j;
	    for (j = 0; j < srtp->keying_cnt; ++j) {
		if (j != i)
		    pjmedia_transport_close(srtp->keying[j]);
	    }
	    srtp->keying_cnt = 1;
	    srtp->keying[0] = srtp->keying[i];
	    srtp->keying_pending_cnt = 0;
	    break;
	}

	i++;
    }

    /* All keying method failed to process remote SDP? */
    if (srtp->keying_cnt == 0)
	return last_err_st;

    return PJ_SUCCESS;
}


static pj_status_t transport_media_start(pjmedia_transport *tp,
				         pj_pool_t *pool,
				         const pjmedia_sdp_session *sdp_local,
				         const pjmedia_sdp_session *sdp_remote,
				         unsigned media_index)
{
    struct transport_srtp *srtp = (struct transport_srtp*) tp;
    pj_status_t last_err_st = PJ_EBUG;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp && pool && sdp_local && sdp_remote, PJ_EINVAL);

    status = pjmedia_transport_media_start(srtp->member_tp, pool,
					   sdp_local, sdp_remote,
				           media_index);
    if (status != PJ_SUCCESS || srtp->bypass_srtp)
	return status;

    /* Invoke media_start() of all keying methods */
    for (i=0; i < srtp->keying_cnt; ) {
	status = pjmedia_transport_media_start(srtp->keying[i], pool,
					       sdp_local, sdp_remote,
					       media_index);
	if (status != PJ_SUCCESS) {
	    /* This keying method returns error, remove it */
	    pj_array_erase(srtp->keying, sizeof(srtp->keying[0]),
			   srtp->keying_cnt, i);
	    srtp->keying_cnt--;
	    last_err_st = status;
	    continue;
	}

	if (!srtp_crypto_empty(&srtp->tx_policy_neg) &&
	    !srtp_crypto_empty(&srtp->rx_policy_neg))
	{
	    /* SRTP nego is done, let's destroy any other keying. */
	    unsigned j;
	    for (j = 0; j < srtp->keying_cnt; ++j) {
		if (j != i)
		    pjmedia_transport_close(srtp->keying[j]);
	    }
	    srtp->keying_cnt = 1;
	    srtp->keying[0] = srtp->keying[i];
	    srtp->keying_pending_cnt = 0;
	    break;
	}

	i++;
    }

    /* All keying method failed to process remote SDP? */
    if (srtp->keying_cnt == 0)
	return last_err_st;

    /* If SRTP key is being negotiated, just return now.
     * The keying method should start the SRTP once keying nego is done.
     */
    if (srtp->keying_pending_cnt)
	return PJ_SUCCESS;

    /* Start SRTP */
    status = start_srtp(srtp);

    return status;
}


static pj_status_t transport_media_stop(pjmedia_transport *tp)
{
    struct transport_srtp *srtp = (struct transport_srtp*) tp;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    /* Invoke media_stop() of all keying methods */
    for (i=0; i < srtp->keying_cnt; ++i) {
	pjmedia_transport_media_stop(srtp->keying[i]);
    }

    /* Invoke media_stop() of member tp */
    status = pjmedia_transport_media_stop(srtp->member_tp);
    if (status != PJ_SUCCESS)
	PJ_LOG(4, (srtp->pool->obj_name,
		   "SRTP failed stop underlying media transport."));

    /* Finally, stop SRTP */
    return pjmedia_transport_srtp_stop(tp);
}


/* Utility */
PJ_DEF(pj_status_t) pjmedia_transport_srtp_decrypt_pkt(pjmedia_transport *tp,
						       pj_bool_t is_rtp,
						       void *pkt,
						       int *pkt_len)
{
    transport_srtp *srtp = (transport_srtp *)tp;
    srtp_err_status_t err;

    if (srtp->bypass_srtp)
	return PJ_SUCCESS;

    PJ_ASSERT_RETURN(tp && pkt && (*pkt_len>0), PJ_EINVAL);
    PJ_ASSERT_RETURN(srtp->session_inited, PJ_EINVALIDOP);

    /* Make sure buffer is 32bit aligned */
    PJ_ASSERT_ON_FAIL( (((pj_ssize_t)pkt) & 0x03)==0, return PJ_EINVAL);

    pj_lock_acquire(srtp->mutex);

    if (!srtp->session_inited) {
	pj_lock_release(srtp->mutex);
	return PJ_EINVALIDOP;
    }

    if (is_rtp)
	err = srtp_unprotect(srtp->srtp_rx_ctx, pkt, pkt_len);
    else
	err = srtp_unprotect_rtcp(srtp->srtp_rx_ctx, pkt, pkt_len);

    if (err != srtp_err_status_ok) {
	PJ_LOG(5,(srtp->pool->obj_name,
		  "Failed to unprotect SRTP, pkt size=%d, err=%s",
		  *pkt_len, get_libsrtp_errstr(err)));
    }

    pj_lock_release(srtp->mutex);

    return (err==srtp_err_status_ok) ? PJ_SUCCESS :
				       PJMEDIA_ERRNO_FROM_LIBSRTP(err);
}

#endif


