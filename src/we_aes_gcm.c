/* we_aes_gcm.c
 *
 * Copyright (C) 2006-2019 wolfSSL Inc.
 *
 * This file is part of wolfengine.
 *
 * wolfengine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * wolfengine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

#include <wolfengine/we_internal.h>

/* May not be available in FIPS builds of wolfSSL */
#ifndef GCM_NONCE_MAX_SZ
/* Maximum size of a nonce. */
#define GCM_NONCE_MAX_SZ        16
#endif
#ifndef GCM_NONCE_MID_SZ
/* Normal size of a nonce - used as is, otherwise GMACed. */
#define GCM_NONCE_MID_SZ        12
#endif

#ifdef WE_HAVE_AESGCM

/*
 * AES-GCM
 */

/**
 * Data required to complete an AES-GCM encrypt/decrypt operation.
 */
typedef struct we_AesGcm
{
    /** The wolfSSL AES data object. */
    Aes            aes;
    /** IV to use with encrypt/decrypt. */
    unsigned char  iv[GCM_NONCE_MAX_SZ];
    /** Length of IV data. */
    int            ivLen;
    /** Tag created when encrypting or tag set for decryption. */
    unsigned char  tag[AES_BLOCK_SIZE];
    /** Length of tag data stored.  */
    int            tagLen;
    /** Additional Authentication Data (AAD) - cumulative. */
    unsigned char *aad;
    /** Length of AAD stored. */
    int            aadLen;
#ifdef WE_AES_GCM_DECRYPT_ON_FINAL
    /* Buffer to hold input data when we_aes_gcm_cipher called to decrypt
     * with no tag. */
    unsigned char *decryptBuf;
    int            decryptBufLen;
#endif
    /** Flag to indicate if there was a tag error during decryption. */
    int            tagErr:1;
    /** Flag to indicate whether object initialized. */
    unsigned int   init:1;
    /** Flag to indicate whether we are doing encrypt (1) or decrpyt (0). */
    unsigned int   enc:1;
    /** Flag to indicate whether dping this for TLS */
    unsigned int   tls:1;
    /** IV set. */
    int            ivSet:1;
    /** IV increment. */
    int            ivInc:1;
} we_AesGcm;

/**
 * Initialize the AES-GCM encrypt/decrypt operation using wolfSSL.
 *
 * @param  ctx  [in,out]  EVP cipher context of operation.
 * @param  key  [in]      AES key - 16/24/32 bytes.
 * @param  iv   [in]      Initialization Vector/nonce - 12 bytes.
 * @param  enc  [in]      1 when initializing for encrypt and 0 when decrypt.
 * @return  1 on success and 0 on failure.
 */
static int we_aes_gcm_init(EVP_CIPHER_CTX *ctx, const unsigned char *key,
                           const unsigned char *iv, int enc)
{
    int ret = 1;
    int rc;
    we_AesGcm *aes;

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_aes_gcm_init");
    WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "ARGS [ctx = %p, key = %p, iv = %p, "
                           "enc = %d]", ctx, key, iv, enc);

    /* Get the internal AES-GCM object. */
    aes = (we_AesGcm *)EVP_CIPHER_CTX_get_cipher_data(ctx);
    if (aes == NULL) {
        WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                   "EVP_CIPHER_CTX_get_cipher_data", aes);
        ret = 0;
    }

    if ((ret == 1) && (key != NULL)) {
        rc = wc_AesInit(&aes->aes, NULL, INVALID_DEVID);
        if (rc != 0) {
            WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesInit", rc);
            ret = 0;
        }
        aes->init = (ret == 1);
    }

    if ((ret == 1) && (key != NULL)) {
        WOLFENGINE_MSG(WE_LOG_CIPHER, "Setting AES-GCM key (%d bytes)",
                       EVP_CIPHER_CTX_key_length(ctx));
        /* Set the AES-GCM key. */
        rc = wc_AesGcmSetKey(&aes->aes, key, EVP_CIPHER_CTX_key_length(ctx));
        if (rc != 0) {
            WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesGcmSetKey", rc);
            ret = 0;
        }
    }
    if ((ret == 1) && (iv != NULL)) {
        /* Cache IV - see ctrl func for other ways to set IV. */
        WOLFENGINE_MSG(WE_LOG_CIPHER, "Caching IV into aes->iv");
        XMEMCPY(aes->iv, iv, aes->ivLen);
        aes->ivSet = 0;
    }

    if (ret == 1) {
        aes->enc = enc;
    }

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_aes_gcm_init", ret);

    return ret;
}

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
/**
 * Cleanup the internal AES-GCM object. Does not free object.
 *
 * AAD will be left allocated only if encrypt/decrypt operations is not
 * completed.
 *
 * @param  ctx  [in]  EVP cipher context.
 * @returns  1 on success and 0 on failure.
 */
static int we_aes_gcm_cleanup(EVP_CIPHER_CTX *ctx)
{
    int ret = 1;
    we_AesGcm *aes;

    /* Get the AES-GCM data to work with. */
    aes = (we_AesGcm *)EVP_CIPHER_CTX_get_cipher_data(ctx);
    if (aes == NULL) {
        WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                   "EVP_CIPHER_CTX_get_cipher_data", aes);
        ret = 0;
    }

    if (ret == 1) {
        /* Dispose of the AAD if not freed in encrypt/decrypt operation. */
        if (aes->aad != NULL) {
            OPENSSL_free(aes->aad);
        }
    #ifdef WE_AES_GCM_DECRYPT_ON_FINAL
        if (aes->decryptBuf != NULL) {
            OPENSSL_free(aes->decryptBuf);
            aes->decryptBuf = NULL;
        }
        aes->decryptBufLen = 0;
    #endif
        aes->tagErr = 0;
        wc_AesFree(&aes->aes);
    }

    return ret;
}
#endif

static int we_aes_gcm_tls_cipher(we_AesGcm *aes, unsigned char *out,
                                 const unsigned char *in, size_t len)
{
    int ret = 1;
    int rc;

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_aes_gcm_tls_cipher");

    /* Doing the TLS variation. */
    if (aes->enc) {
        /* Plaintext is input buffer without IV and tag. */
        word32 encLen = (word32)len - EVP_GCM_TLS_EXPLICIT_IV_LEN
                                    - EVP_GCM_TLS_TAG_LEN;
        if (len != 0) {
            /* Copy the explicit part of the IV into out. */
            XMEMCPY(out, aes->iv + EVP_GCM_TLS_FIXED_IV_LEN,
                    EVP_GCM_TLS_EXPLICIT_IV_LEN);

            /* Move to start of plaintext and cipher text. */
            in += EVP_GCM_TLS_EXPLICIT_IV_LEN;
            out += EVP_GCM_TLS_EXPLICIT_IV_LEN;
            /* Encrypt the data except explicit IV.
             * Tag goes at end of output buffer.
             */
            rc = wc_AesGcmEncrypt(&aes->aes, out, in, encLen, aes->iv,
                aes->ivLen, out + encLen, EVP_GCM_TLS_TAG_LEN, aes->aad,
                aes->aadLen);
            if (rc != 0) {
                WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesGcmEncrypt_ex", rc);
                ret = -1;
            }
        }
        if (ret == 1) {
            WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "Encrypted %d bytes "
                                   "(AES-GCM):", encLen);
            WOLFENGINE_BUFFER(WE_LOG_CIPHER, out, encLen);
            WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "Generated tag:");
            WOLFENGINE_BUFFER(WE_LOG_CIPHER, out + encLen, EVP_GCM_TLS_TAG_LEN);

            ret = (int)len;
        }
    }
    else {
        /* Cipher text is input buffer without IV and tag. */
        word32 decLen = (word32)len - EVP_GCM_TLS_EXPLICIT_IV_LEN
                                    - EVP_GCM_TLS_TAG_LEN;
        if (len != 0) {
            /* Copy the explicit part of the IV from input. */
            XMEMCPY(aes->iv + EVP_GCM_TLS_FIXED_IV_LEN, in,
                    EVP_GCM_TLS_EXPLICIT_IV_LEN);

            /* Decrypt the data except explicit IV.
             * Tag is at end of input buffer.
             */
            rc = wc_AesGcmDecrypt(&aes->aes,
                                  out + EVP_GCM_TLS_EXPLICIT_IV_LEN,
                                  in + EVP_GCM_TLS_EXPLICIT_IV_LEN,
                                  decLen, aes->iv, aes->ivLen,
                                  in + len - EVP_GCM_TLS_TAG_LEN,
                                  EVP_GCM_TLS_TAG_LEN, aes->aad, aes->aadLen);
            if (rc != 0) {
                WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesGcmDecrypt", rc);
                ret = -1;
            }
        }
        if (ret == 1) {
            WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "Decrypted %d bytes "
                                   "(AES-GCM):", decLen);
            WOLFENGINE_BUFFER(WE_LOG_CIPHER, out + EVP_GCM_TLS_EXPLICIT_IV_LEN,
                              decLen);
#if OPENSSL_VERSION_NUMBER < 0x30000000L
            ret = (int)decLen;
#else
            ret = (int)len;
#endif
        }
    }

    /* Dispose of any AAD - all used now. */
    OPENSSL_free(aes->aad);
    aes->aad = NULL;
    aes->aadLen = 0;

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_aes_gcm_tls_cipher", ret);

    return ret;
}

/**
 * Decrypt the ciphertext and check the tag. If the tag is bad, aes->tagErr will
 * be set to 1, but the return value will still be 1 (success).
 *
 * @param  aes  [in,out]  wolfEngine AES object.
 * @param  out  [out]     Buffer to store decrypted result.
 * @param  in   [in]      Ciphertext to decrypt.
 * @param  len  [in]      Length of ciphertext.
 * @return 1 on success and -1 on failure.
 */
static int we_aes_gcm_decrypt(we_AesGcm* aes, unsigned char *out,
                              const unsigned char *in, size_t len)
{
    int ret = 1;
    int rc = 0;

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_aes_gcm_decrypt");
    WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "ARGS [aes = %p, out = %p, in = %p, "
                           "len = %zu]", aes, out, in, len);
    if (aes == NULL || out == NULL || in == NULL) {
        WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, "Bad parameter.");
        ret = -1;
    }

    if (ret == 1) {
        /* Decrypt the data, use with AAD to verify tag is correct. */
        rc = wc_AesGcmDecrypt(&aes->aes, out, in, (word32)len, aes->iv,
                              aes->ivLen, aes->tag, aes->tagLen,
                              aes->aad, aes->aadLen);
        if (rc == AES_GCM_AUTH_E) {
            /* Defer reporting the tag failure until "final" gets
             * called.*/
            aes->tagErr = 1;
        }
        else if (rc != 0) {
            WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesGcmDecrypt",
                rc);
            ret = -1;
        }
        else {
            WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "Decrypted %zu bytes "
                "(AES-GCM):", len);
            WOLFENGINE_BUFFER(WE_LOG_CIPHER, out, (unsigned int)len);
        }
    }

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_aes_gcm_decrypt", ret);

    return ret;
}

/**
 * Encrypt/decrypt the data.
 * One-shot encrypt/decrypt - not streaming.
 *
 * @param  ctx  [in,out]  EVP cipher context of operation.
 * @param  out  [out]     Buffer to store enciphered result.<br>
 *                        NULL indicates AAD in.
 * @param  in   [in]      AAD or data to encrypt/decrypt.
 * @param  len  [in]      Length of AAD or data to encrypt/decrypt.
 * @return  When out is NULL, length of input data on success and -1 on failure.
 *          <br>
 *          When out is not NULL, length of output data on success and -1 on
 *          failure.
 */
static int we_aes_gcm_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
                             const unsigned char *in, size_t len)
{
    int ret = 1;
    int rc;
    we_AesGcm *aes;
    unsigned char *p;
    int decryptFinal = 0;

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_aes_gcm_cipher");
    WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "ARGS [ctx = %p, out = %p, in = %p, "
                           "len = %zu]", ctx, out, in, len);

    /* Get the AES-GCM data to work with. */
    aes = (we_AesGcm *)EVP_CIPHER_CTX_get_cipher_data(ctx);
    if (aes == NULL) {
        WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                   "EVP_CIPHER_CTX_get_cipher_data", aes);
        ret = -1;
    }

    decryptFinal = (!aes->enc && len == 0 && in == NULL);

    if ((ret == 1) && aes->tls) {
        ret = we_aes_gcm_tls_cipher(aes, out, in, len);
    }
    else if ((ret == 1) && (out == NULL) & (len != 0)) {
        WOLFENGINE_MSG(WE_LOG_CIPHER, "Resizing stored AAD and appending "
                       "data, len = %d", (int)len);
        /* Resize stored AAD and append new data. */
        p = (unsigned char*)OPENSSL_realloc(aes->aad, aes->aadLen + (int)len);
        if (p == NULL) {
            WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER, "OPENSSL_realloc", p);
            ret = -1;
        }
        else {
            /* Copy in new data after existing data. */
            aes->aad = p;
            XMEMCPY(aes->aad + aes->aadLen, in, len);
            aes->aadLen += len;
            ret = (int)len;
        }
    }
    /* Length may be zero for cases with AAD data only (GMAC) */
    else if ((ret == 1) && (out != NULL) && (in != NULL || aes->aadLen > 0) &&
             !decryptFinal) {
        if (!aes->ivSet) {
            /* Set extern IV. */
            WOLFENGINE_MSG(WE_LOG_CIPHER, "Setting external IV");
            rc = wc_AesGcmSetExtIV(&aes->aes, aes->iv, aes->ivLen);
            if (rc != 0) {
                WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesGcmSetExtIV", rc);
                ret = -1;
            }
            else {
                aes->ivSet = 1;
            }
        }
        if ((ret == 1) && (aes->enc)) {
            WOLFENGINE_MSG(WE_LOG_CIPHER, "AES-GCM Encrypting");
            /* Tag always full size on calculation. */
            aes->tagLen = EVP_GCM_TLS_TAG_LEN;
            /* Encrypt the data, process AAD data, generate tag */
            rc = wc_AesGcmEncrypt_ex(&aes->aes, out, in, (word32)len, aes->iv,
                    aes->ivLen, aes->tag, aes->tagLen, aes->aad, aes->aadLen);
            if (rc != 0) {
                WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER, "wc_AesGcmEncrypt_ex", rc);
                ret = -1;
            }
            if (ret == 1) {
                WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "Encrypted %zu bytes "
                                       "(AES-GCM):", len);
                WOLFENGINE_BUFFER(WE_LOG_CIPHER, out, (unsigned int)len);
                WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "AES-GCM tag:");
                WOLFENGINE_BUFFER(WE_LOG_CIPHER, aes->tag, aes->tagLen);
            }
        }
        else if (ret == 1) {
            WOLFENGINE_MSG(WE_LOG_CIPHER, "AES-GCM Decrypting");
        #ifdef WE_AES_GCM_DECRYPT_ON_FINAL
            if (aes->tagLen == 0) {
                WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "No tag yet, deferring "
                    "decryption until \"final\" called with tag.");
                if (aes->decryptBuf != NULL) {
                    OPENSSL_free(aes->decryptBuf);
                }
                aes->decryptBuf = (unsigned char*)OPENSSL_malloc(len);
                if (aes->decryptBuf == NULL) {
                    WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                        "OPENSSL_malloc", aes->decryptBuf);
                    ret = -1;
                }
                else {
                    XMEMCPY(aes->decryptBuf, in, len);
                    aes->decryptBufLen = len;
                }
                /* No data decrypted, yet, so return 0. */
                ret = 0;
            }
            else
        #endif
            {
                ret = we_aes_gcm_decrypt(aes, out, in, len);
            }
        }

    #ifdef WE_AES_GCM_DECRYPT_ON_FINAL
        if (aes->enc || (!aes->enc && aes->tagLen > 0))
    #endif
        {
            /* Dispose of any AAD - all used now. */
            OPENSSL_free(aes->aad);
            aes->aad = NULL;
            aes->aadLen = 0;
        }
        if (ret == 1) {
            if (aes->ivInc) {
                int i;
                for (i = aes->ivLen - 1; i >= aes->ivLen - 8; i--) {
                    if ((++aes->iv[i]) != 0) {
                        break;
                    }
                }
                aes->ivInc = 0;
                aes->ivSet = 0;
            }
            ret = (int)len;
        }
    }
    else if ((ret == 1) && (len == 0)) {
    #ifdef WE_AES_GCM_DECRYPT_ON_FINAL
        if (!aes->enc) {
            ret = we_aes_gcm_decrypt(aes, out, aes->decryptBuf,
                aes->decryptBufLen);
            aes->tagLen = 0;
        }
    #endif

        if (ret == 1) {
            if (aes->tagErr) {
                WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, "AES-GCM: tag error.");
                ret = -1;
            }
            else {
                ret = 0;
            }
        }

    #ifdef WE_AES_GCM_DECRYPT_ON_FINAL
        if (!aes->enc) {
            if (ret != -1) {
                /* Return the length of decryption now that we've actually
                 * decrypted. */
                ret = aes->decryptBufLen;
            }

            OPENSSL_free(aes->decryptBuf);
            aes->decryptBuf = NULL;
            aes->decryptBufLen = 0;
        }
    #endif

        if (aes->aad != NULL) {
            OPENSSL_free(aes->aad);
            aes->aad = NULL;
            aes->aadLen = 0;
        }
        aes->tagErr = 0;
    }

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_aes_gcm_cipher", ret);

    return ret;
}

/**
 * Extra operations for AES-GCM.
 * Supported operations include:
 *  - EVP_CTRL_GET_IV (version 3.0+): get IV from wolfengine object
 *  - EVP_CTRL_AEAD_SET_IVLEN: set the length of an IV/nonce
 *  - EVP_CTRL_GCM_SET_IV_FIXED: set the fixed part of an IV/nonce
 *  - EVP_CTRL_GCM_GET_IVLEN: get the total IV/nonce length
 *  - EVP_CTRL_GCM_IV_GEN: set the generated IV/nonce
 *  - EVP_CTRL_AEAD_GET_TAG: get the tag value after encrypt
 *  - EVP_CTRL_AEAD_SET_TAG: set the tag value before decrypt
 *  - EVP_CTRL_AEAD_TLS1_AAD: set AAD for TLS
 *
 * @param  ctx   [in.out]  EVP cipher context of operation.
 * @param  type  [in]      Type of operation to perform.
 * @param  arg   [in]      Integer argument.
 * @param  ptr   [in]      Pointer argument.
 * @return  1 on success and 0 on failure.
 */
static int we_aes_gcm_ctrl(EVP_CIPHER_CTX *ctx, int type, int arg, void *ptr)
{
    int ret = 1;
    int rc;
    we_AesGcm *aes;
    char errBuff[WOLFENGINE_MAX_LOG_WIDTH];

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_aes_gcm_ctrl");
    WOLFENGINE_MSG_VERBOSE(WE_LOG_CIPHER, "ARGS [ctx = %p, type = %d, "
                           "arg = %d, ptr = %p]", ctx, type, arg, ptr);

    /* Get the AES-GCM data to work with. */
    aes = (we_AesGcm *)EVP_CIPHER_CTX_get_cipher_data(ctx);
    if (aes == NULL) {
        WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                   "EVP_CIPHER_CTX_get_cipher_data", aes);
        ret = 0;
    }
    if (ret == 1) {
        switch (type) {
            case EVP_CTRL_INIT:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_INIT");
                /* No IV yet. */
                aes->ivLen = GCM_NONCE_MID_SZ;
                aes->ivSet = 0;
                aes->ivInc = 0;
                /* No tag set. */
                aes->tagLen = 0;
                /* Start with no AAD. */
                aes->aad = NULL;
                aes->aadLen = 0;
                /* Internal AES-GCM object initialized. */
                aes->init = 1;
                /* Not doing GCM for TLS unless ctrl function called. */
                aes->tls = 0;
            #ifdef WE_AES_GCM_DECRYPT_ON_FINAL
                aes->decryptBuf = NULL;
                aes->decryptBufLen = 0;
            #endif
                aes->tagErr = 0;
                break;

            case EVP_CTRL_AEAD_SET_IVLEN:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_AEAD_SET_IVLEN");
                /* Set the IV/nonce length to use
                 *   arg [in] length of IV/nonce to use
                 *   ptr [in] Unused
                 */
                if (arg <= 0 || arg > GCM_NONCE_MAX_SZ) {
                    XSNPRINTF(errBuff, sizeof(errBuff), "Invalid nonce length "
                              "%d", arg);
                    WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, errBuff);
                    ret = 0;
                }
                else {
                    WOLFENGINE_MSG(WE_LOG_CIPHER, "Set aes->ivLen: %d", arg);
                    aes->ivLen = arg;
                }
                break;

            case EVP_CTRL_GCM_SET_IV_FIXED:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_GCM_SET_IV_FIXED");
                /* Set the fixed part of an IV
                 *   arg [in] size of fixed part of IV/nonce
                 *   ptr [in] fixed part of IV/nonce data
                 */
                if (arg == -1) {
                    /* arg of -1 means copy all data. */
                    WOLFENGINE_MSG(WE_LOG_CIPHER, "Copy all data, IV fixed");
                    if (aes->ivLen == 0)
                        aes->ivLen = GCM_NONCE_MID_SZ;
                    XMEMCPY(aes->iv, ptr, aes->ivLen);
                    XMEMCPY(EVP_CIPHER_CTX_iv_noconst(ctx), ptr, aes->ivLen);
                }
                else {
                    /* Set the fixed IV and have the rest generated. */
                    WOLFENGINE_MSG(WE_LOG_CIPHER, "Set fixed IV, "
                                   "generate the rest");
                    if (aes->ivLen == 0)
                        aes->ivLen = GCM_NONCE_MID_SZ;
                #ifndef WE_SINGLE_THREADED
                    rc = wc_LockMutex(we_rng_mutex);
                    if (rc != 0) {
                        WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER,
                                              "wc_LockMutex", rc);
                        ret = 0;
                    }
                    if (ret == 1)
                #endif
                    {
                        rc = wc_AesGcmSetIV(&aes->aes, aes->ivLen,
                                (const byte*)ptr, arg, we_rng);
                #ifndef WE_SINGLE_THREADED
                        wc_UnLockMutex(we_rng_mutex);
                #endif
                        if (rc != 0) {
                            WOLFENGINE_ERROR_FUNC(WE_LOG_CIPHER,
                                                  "wc_AesGcmSetIV", rc);
                            ret = 0;
                        }
                    }
                    if (ret == 1) {
                       aes->ivSet = 1;
                       XMEMCPY(aes->iv, aes->aes.reg, aes->ivLen);
                       XMEMCPY(EVP_CIPHER_CTX_iv_noconst(ctx), aes->iv,
                               aes->ivLen);
                    }
                }
                break;

            case EVP_CTRL_GET_IVLEN:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_GET_IVLEN");
                /* Get the IV length
                 *   ptr [out] Length of IV
                 */
                *(int *)ptr = aes->ivLen;
                break;

            case EVP_CTRL_GCM_IV_GEN:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_GCM_IV_GEN");
                /* Set the generated IV
                 *   arg [in] size of generated IV/nonce
                 *   ptr [in] generated IV/nonce data
                 */
                if ((arg <= 0) || (arg > GCM_NONCE_MAX_SZ)) {
                    XSNPRINTF(errBuff, sizeof(errBuff), "Invalid nonce length "
                              "%d", arg);
                    WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, errBuff);
                    ret = 0;
                }
                else {
                    XMEMCPY(ptr, aes->iv + aes->ivLen - arg, arg);
                    aes->ivInc = 1;
                }
                break;

            case EVP_CTRL_AEAD_GET_TAG:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_AEAD_GET_TAG");
                /* Get the tag from encryption.
                 *   arg [in] size of buffer
                 *   ptr [in] buffer to copy into
                 */
                if ((!aes->enc) || (arg <= 0) || (arg > aes->tagLen)) {
                    ret = 0;
                }
                else {
                    XMEMCPY(ptr, aes->tag, arg);
                }
                break;

            case EVP_CTRL_AEAD_SET_TAG:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_AEAD_SET_TAG");
                /* Set the tag for decryption.
                 *   arg [in] size of tag
                 *   ptr [in] tag data to copy
                 */
                if ((arg <= 0) || (arg > AES_BLOCK_SIZE)) {
                    WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, "Invalid tag size");
                    ret = 0;
                }
                else if ((!aes->enc) && (ptr == NULL)) {
                    WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, "No tag for decrypt");
                    ret = 0;
                }
                else {
                    if ((!aes->enc) && (ptr != NULL)) {
                        XMEMCPY(aes->tag, ptr, arg);
                    }
                    aes->tagLen = arg;
                }
                break;

            case EVP_CTRL_AEAD_TLS1_AAD:
                WOLFENGINE_MSG(WE_LOG_CIPHER, "EVP_CTRL_AEAD_TLS1_AAD");
                /* Set additional authentication data for TLS
                 *   arg [in] size of AAD
                 *   ptr [in] AAD to use
                 */
                if (arg != EVP_AEAD_TLS1_AAD_LEN) {
                    WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, "Invalid TLS AAD size");
                    ret = 0;
                }
                if (ret == 1) {
                    unsigned int len;

                    /* Set modified AAD based on record header */
                    if (aes->aad != NULL) {
                        OPENSSL_free(aes->aad);
                    }
                    aes->aad = (unsigned char*)OPENSSL_malloc(arg);
                    if (aes->aad == NULL) {
                        WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                                   "OPENSSL_malloc", aes->aad);
                        ret = 0;
                    }
                    if (ret == 1) {
                        XMEMCPY(aes->aad, ptr, arg);
                        aes->aadLen = arg;
                        len = (aes->aad[arg - 2] << 8) | aes->aad[arg - 1];
                        if (len < EVP_GCM_TLS_EXPLICIT_IV_LEN) {
                            WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER,
                                "Length in AAD invalid");
                            ret = 0;
                        }
                    }
                    if (ret == 1) {
                        len -= EVP_GCM_TLS_EXPLICIT_IV_LEN;
                        if (!aes->enc) {
                            if (len < EVP_GCM_TLS_TAG_LEN) {
                                WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER,
                                    "Length in AAD invalid");
                                ret = 0;
                            }
                            else {
                                len -= EVP_GCM_TLS_TAG_LEN;
                            }
                        }
                    }
                    if (ret == 1) {
                        aes->aad[arg - 2] = len >> 8;
                        aes->aad[arg - 1] = len;
                        aes->tls = 1;
                        ret = EVP_GCM_TLS_TAG_LEN;
                    }
                }
                break;

            default:
                XSNPRINTF(errBuff, sizeof(errBuff), "Unsupported ctrl type %d",
                          type);
                WOLFENGINE_ERROR_MSG(WE_LOG_CIPHER, errBuff);
                ret = 0;
                break;
        }
    }

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_aes_gcm_ctrl", ret);

    return ret;
}

/** Flags for AES-GCM method. */
#define AES_GCM_FLAGS              \
    (EVP_CIPH_FLAG_CUSTOM_CIPHER | \
     EVP_CIPH_CUSTOM_IV          | \
     EVP_CIPH_CUSTOM_IV_LENGTH   | \
     EVP_CIPH_ALWAYS_CALL_INIT   | \
     EVP_CIPH_CTRL_INIT          | \
     EVP_CIPH_FLAG_AEAD_CIPHER   | \
     EVP_CIPH_FLAG_DEFAULT_ASN1  | \
     EVP_CIPH_GCM_MODE)

/** AES128-GCM EVP cipher method. */
EVP_CIPHER* we_aes128_gcm_ciph = NULL;
/** AES192-GCM EVP cipher method. */
EVP_CIPHER* we_aes192_gcm_ciph = NULL;
/** AES256-GCM EVP cipher method. */
EVP_CIPHER* we_aes256_gcm_ciph = NULL;


/**
 * Initialize an AES-GCM method.
 *
 * @return  1 on success and 0 on failure.
 */
static int we_init_aesgcm_meth(EVP_CIPHER *cipher)
{
    int ret;

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_init_aesgcm_meth");

    ret = EVP_CIPHER_meth_set_iv_length(cipher, GCM_NONCE_MID_SZ);
    if (ret == 1) {
        ret = EVP_CIPHER_meth_set_flags(cipher, AES_GCM_FLAGS);
    }
    if (ret == 1) {
        ret = EVP_CIPHER_meth_set_init(cipher, we_aes_gcm_init);
    }
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (ret == 1) {
        ret = EVP_CIPHER_meth_set_cleanup(cipher, we_aes_gcm_cleanup);
    }
#endif
    if (ret == 1) {
        ret = EVP_CIPHER_meth_set_do_cipher(cipher, we_aes_gcm_cipher);
    }
    if (ret == 1) {
        ret = EVP_CIPHER_meth_set_ctrl(cipher, we_aes_gcm_ctrl);
    }
    if (ret == 1) {
        ret = EVP_CIPHER_meth_set_impl_ctx_size(cipher, sizeof(we_AesGcm));
    }

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_init_aesgcm_meth", ret);

    return ret;
}

/**
 * Initialize the AES-GCM methods.
 *
 * @return  1 on success and 0 on failure.
 */
int we_init_aesgcm_meths()
{
    int ret = 1;

    WOLFENGINE_ENTER(WE_LOG_CIPHER, "we_init_aesgcm_meths");

    /* AES128-GCM */
    we_aes128_gcm_ciph = EVP_CIPHER_meth_new(NID_aes_128_gcm, 1,
                                             AES_128_KEY_SIZE);
    if (we_aes128_gcm_ciph == NULL) {
        WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                   "EVP_CIPHER_meth_new - AES-128-GCM",
                                   we_aes128_gcm_ciph);
        ret = 0;
    }
    if (ret == 1) {
        ret = we_init_aesgcm_meth(we_aes128_gcm_ciph);
    }

    /* AES192-GCM */
    if (ret == 1) {
        we_aes192_gcm_ciph = EVP_CIPHER_meth_new(NID_aes_192_gcm, 1,
                                                 AES_192_KEY_SIZE);
        if (we_aes192_gcm_ciph == NULL) {
            WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                       "EVP_CIPHER_meth_new - AES-192-GCM",
                                       we_aes192_gcm_ciph);
            ret = 0;
        }
    }
    if (ret == 1) {
        ret = we_init_aesgcm_meth(we_aes192_gcm_ciph);
    }

    /* AES256-GCM */
    if (ret == 1) {
        we_aes256_gcm_ciph = EVP_CIPHER_meth_new(NID_aes_256_gcm, 1,
                                                 AES_256_KEY_SIZE);
        if (we_aes256_gcm_ciph == NULL) {
            WOLFENGINE_ERROR_FUNC_NULL(WE_LOG_CIPHER,
                                       "EVP_CIPHER_meth_new - AES-256-GCM",
                                       we_aes256_gcm_ciph);
            ret = 0;
        }
    }
    if (ret == 1) {
        ret = we_init_aesgcm_meth(we_aes256_gcm_ciph);
    }

    /* Cleanup */
    if ((ret == 0) && (we_aes128_gcm_ciph != NULL)) {
        EVP_CIPHER_meth_free(we_aes128_gcm_ciph);
        we_aes128_gcm_ciph = NULL;
    }
    if ((ret == 0) && (we_aes192_gcm_ciph != NULL)) {
        EVP_CIPHER_meth_free(we_aes192_gcm_ciph);
        we_aes192_gcm_ciph = NULL;
    }
    if ((ret == 0) && (we_aes256_gcm_ciph != NULL)) {
        EVP_CIPHER_meth_free(we_aes256_gcm_ciph);
        we_aes256_gcm_ciph = NULL;
    }

    WOLFENGINE_LEAVE(WE_LOG_CIPHER, "we_init_aesgcm_meths", ret);

    return ret;
}

#endif /* WE_HAVE_AESGCM */

