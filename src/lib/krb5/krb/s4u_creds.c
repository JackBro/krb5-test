/* -*- mode: c; indent-tabs-mode: nil -*- */
/*
 * lib/krb5/krb/s4u_creds.c
 *
 * Copyright (C) 2009 by the Massachusetts Institute of Technology.
 * All rights reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 *
 *
 */

#include "k5-int.h"
#include "int-proto.h"

/* Convert ticket flags to necessary KDC options */
#define FLAGS2OPTS(flags) (flags & KDC_TKT_COMMON_MASK)

/*
 * Implements S4U2Self, by which a service can request a ticket to
 * itself on behalf of an arbitrary principal.
 */

static krb5_error_code
krb5_get_as_key_noop(
    krb5_context context,
    krb5_principal client,
    krb5_enctype etype,
    krb5_prompter_fct prompter,
    void *prompter_data,
    krb5_data *salt,
    krb5_data *params,
    krb5_keyblock *as_key,
    void *gak_data)
{
    /* force a hard error, we don't actually have the key */
    return KDC_ERR_PREAUTH_FAILED;
}

static krb5_error_code
s4u_identify_user(krb5_context context,
                  krb5_creds *in_creds,
                  krb5_data *subject_cert,
                  krb5_principal *canon_user)
{
    krb5_error_code code;
    krb5_preauthtype ptypes[1] = { KRB5_PADATA_S4U_X509_USER };
    krb5_creds creds;
    int use_master = 0;
    krb5_get_init_creds_opt *opts = NULL;
    krb5_gic_opt_ext *opte = NULL;
    krb5_principal_data client_data;
    krb5_principal client;
    krb5_s4u_userid userid;

    *canon_user = NULL;

    if (in_creds->client == NULL && subject_cert == NULL) {
        return EINVAL;
    }

    if (in_creds->client != NULL &&
        krb5_princ_type(context, in_creds->client) != KRB5_NT_ENTERPRISE_PRINCIPAL) {
        /* we already know the realm of the user */
        return krb5_copy_principal(context, in_creds->client, canon_user);
    }

    memset(&creds, 0, sizeof(creds));

    memset(&userid, 0, sizeof(userid));
    if (subject_cert != NULL)
        userid.subject_cert = *subject_cert;

    code = krb5_get_init_creds_opt_alloc(context, &opts);
    if (code != 0)
        goto cleanup;
    krb5_get_init_creds_opt_set_tkt_life(opts, 15);
    krb5_get_init_creds_opt_set_renew_life(opts, 0);
    krb5_get_init_creds_opt_set_forwardable(opts, 0);
    krb5_get_init_creds_opt_set_proxiable(opts, 0);
    krb5_get_init_creds_opt_set_canonicalize(opts, 1);
    krb5_get_init_creds_opt_set_preauth_list(opts, ptypes, 1);
    code = krb5int_gic_opt_to_opte(context, opts, &opte, 0, "s4u_identify_user");
    if (code != 0)
        goto cleanup;

    if (in_creds->client != NULL)
        client = in_creds->client;
    else {
        client_data.magic = KV5M_PRINCIPAL;
        client_data.realm = in_creds->server->realm;
        client_data.data = NULL; /* should this be NULL, empty or a fixed string? XXX */
        client_data.length = 0;
        client_data.type = KRB5_NT_ENTERPRISE_PRINCIPAL;
        client = &client_data;
    }

    code = krb5_get_init_creds(context, &creds, in_creds->client,
                               NULL, NULL, 0, NULL, opte,
                               krb5_get_as_key_noop, &userid,
                               &use_master, NULL);
    if (code == 0 ||
        code == KDC_ERR_PREAUTH_REQUIRED ||
        code == KDC_ERR_PREAUTH_FAILED) {
        *canon_user = userid.user;
        userid.user = NULL;
        code = 0;
    }

cleanup:
    krb5_free_cred_contents(context, &creds);
    if (opts != NULL)
        krb5_get_init_creds_opt_free(context, opts);
    if (userid.user != NULL)
        krb5_free_principal(context, userid.user);

    return code;
}

static krb5_error_code
make_pa_for_user_checksum(krb5_context context,
                          krb5_keyblock *key,
                          krb5_pa_for_user *req,
                          krb5_checksum *cksum)
{
    krb5_error_code code;
    int i;
    krb5_int32 name_type;
    char *p;
    krb5_data data;
    krb5_cksumtype cksumtype;

    data.length = 4;
    for (i = 0; i < krb5_princ_size(context, req->user); i++) {
        data.length += krb5_princ_component(context, req->user, i)->length;
    }
    data.length += krb5_princ_realm(context, req->user)->length;
    data.length += req->auth_package.length;

    p = data.data = malloc(data.length);
    if (data.data == NULL)
        return ENOMEM;

    name_type = krb5_princ_type(context, req->user);
    p[0] = (name_type >> 0 ) & 0xFF;
    p[1] = (name_type >> 8 ) & 0xFF;
    p[2] = (name_type >> 16) & 0xFF;
    p[3] = (name_type >> 24) & 0xFF;
    p += 4;

    for (i = 0; i < krb5_princ_size(context, req->user); i++) {
        memcpy(p, krb5_princ_component(context, req->user, i)->data,
               krb5_princ_component(context, req->user, i)->length);
        p += krb5_princ_component(context, req->user, i)->length;
    }

    memcpy(p, krb5_princ_realm(context, req->user)->data,
           krb5_princ_realm(context, req->user)->length);
    p += krb5_princ_realm(context, req->user)->length;

    memcpy(p, req->auth_package.data, req->auth_package.length);

    code = krb5int_c_mandatory_cksumtype(context, key->enctype, &cksumtype);
    if (code != 0) {
        free(data.data);
        return code;
    }

    code = krb5_c_make_checksum(context, cksumtype, key,
                                KRB5_KEYUSAGE_APP_DATA_CKSUM, &data,
                                cksum);

    free(data.data);

    return code;
}

static krb5_error_code
build_pa_for_user(krb5_context context,
                  krb5_creds *tgt,
                  krb5_s4u_userid *userid,
                  krb5_pa_data **out_padata)
{
    krb5_error_code code;
    krb5_pa_data *padata;
    krb5_pa_for_user for_user;
    krb5_data *for_user_data = NULL;
    char package[] = "Kerberos";

    if (userid->user == NULL) {
        code = EINVAL;
        goto cleanup;
    }

    memset(&for_user, 0, sizeof(for_user));
    for_user.user = userid->user;
    for_user.auth_package.data = package;
    for_user.auth_package.length = sizeof(package) - 1;

    code = make_pa_for_user_checksum(context, &tgt->keyblock,
                                     &for_user, &for_user.cksum);
    if (code != 0)
        goto cleanup;

    code = encode_krb5_pa_for_user(&for_user, &for_user_data);
    if (code != 0)
        goto cleanup;

    padata = (krb5_pa_data *)malloc(sizeof(*padata));
    if (padata == NULL) {
        code = ENOMEM;
        goto cleanup;
    }

    padata->magic = KV5M_PA_DATA;
    padata->pa_type = KRB5_PADATA_FOR_USER;
    padata->length = for_user_data->length;
    padata->contents = (krb5_octet *)for_user_data->data;

    free(for_user_data);
    for_user_data = NULL;

    *out_padata = padata;

cleanup:
    if (for_user.cksum.contents != NULL)
        krb5_free_checksum_contents(context, &for_user.cksum);
    krb5_free_data(context, for_user_data);

    return code;
}

/*
 * This function is invoked by krb5int_send_tgs() just before
 * the request is encoded; it gives us access to the nonce and
 * subkey without requiring them to be generated by the caller.
 */
static krb5_error_code
build_pa_s4u_x509_user(krb5_context context,
                       krb5_keyblock *subkey,
                       krb5_kdc_req *tgsreq,
                       void *gcvt_data)
{
    krb5_error_code code;
    krb5_pa_s4u_x509_user *s4u_user = (krb5_pa_s4u_x509_user *)gcvt_data;
    krb5_data *data = NULL;
    krb5_pa_data **padata;
    krb5_cksumtype cksumtype;
    int i;

    assert(s4u_user->cksum.contents == NULL);

    s4u_user->user_id.nonce = tgsreq->nonce;

    code = encode_krb5_s4u_userid(&s4u_user->user_id, &data);
    if (code != 0)
        goto cleanup;

    /* [MS-SFU] 2.2.2: unusual to say the least, but enc_padata secures it */
    if (subkey->enctype == ENCTYPE_ARCFOUR_HMAC ||
        subkey->enctype == ENCTYPE_ARCFOUR_HMAC_EXP) {
        cksumtype = CKSUMTYPE_RSA_MD4;
    } else {
        code = krb5int_c_mandatory_cksumtype(context, subkey->enctype,
                                             &cksumtype);
    }
    if (code != 0)
        goto cleanup;

    code = krb5_c_make_checksum(context, cksumtype, subkey,
                                KRB5_KEYUSAGE_PA_S4U_X509_USER_REQUEST, data,
                                &s4u_user->cksum);
    if (code != 0)
        goto cleanup;

    krb5_free_data(context, data);
    data = NULL;

    code = encode_krb5_pa_s4u_x509_user(s4u_user, &data);
    if (code != 0)
        goto cleanup;

    assert(tgsreq->padata != NULL);

    for (i = 0; tgsreq->padata[i] != NULL; i++)
        ;

    padata = (krb5_pa_data **)realloc(tgsreq->padata,
                                      (i + 2) * sizeof(krb5_pa_data *));
    if (padata == NULL) {
        code = ENOMEM;
        goto cleanup;
    }
    tgsreq->padata = padata;

    padata[i] = (krb5_pa_data *)malloc(sizeof(krb5_pa_data));
    if (padata[i] == NULL) {
        code = ENOMEM;
        goto cleanup;
    }
    padata[i]->magic = KV5M_PA_DATA;
    padata[i]->pa_type = KRB5_PADATA_S4U_X509_USER;
    padata[i]->length = data->length;
    padata[i]->contents = (krb5_octet *)data->data;

    padata[i + 1] = NULL;

    free(data);
    data = NULL;

cleanup:
    if (code != 0 && s4u_user->cksum.contents != NULL) {
        krb5_free_checksum_contents(context, &s4u_user->cksum);
        s4u_user->cksum.contents = NULL;
    }
    krb5_free_data(context, data);

    return code;
}

static krb5_error_code
verify_s4u2self_reply(krb5_context context,
                      krb5_keyblock *subkey,
                      krb5_pa_s4u_x509_user *req_s4u_user,
                      krb5_pa_data **rep_padata,
                      krb5_pa_data **enc_padata)
{
    krb5_error_code code;
    krb5_pa_data *rep_s4u_padata, *enc_s4u_padata;
    krb5_pa_s4u_x509_user *rep_s4u_user = NULL;
    krb5_data data, *datap = NULL;
    krb5_keyusage usage;
    krb5_boolean valid;
    krb5_boolean not_newer;

    assert(req_s4u_user != NULL);

    switch (subkey->enctype) {
    case ENCTYPE_DES_CBC_CRC:
    case ENCTYPE_DES_CBC_MD4:
    case ENCTYPE_DES_CBC_MD5:
    case ENCTYPE_DES3_CBC_SHA1:
    case ENCTYPE_DES3_CBC_RAW:
    case ENCTYPE_ARCFOUR_HMAC:
    case ENCTYPE_ARCFOUR_HMAC_EXP :
        not_newer = TRUE;
        break;
    default:
        not_newer = FALSE;
        break;
    }

    enc_s4u_padata = krb5int_find_pa_data(context, enc_padata, KRB5_PADATA_S4U_X509_USER);

    /* XXX this will break newer enctypes with a MIT 1.7 KDC */
    rep_s4u_padata = krb5int_find_pa_data(context, rep_padata, KRB5_PADATA_S4U_X509_USER);
    if (rep_s4u_padata == NULL) {
        if (not_newer == FALSE || enc_s4u_padata != NULL)
            return KRB5_KDCREP_MODIFIED;
        else
            return 0;
    }

    data.length = rep_s4u_padata->length;
    data.data = (char *)rep_s4u_padata->contents;

    code = decode_krb5_pa_s4u_x509_user(&data, &rep_s4u_user);
    if (code != 0)
        goto cleanup;

    if (rep_s4u_user->user_id.nonce != req_s4u_user->user_id.nonce) {
        code = KRB5_KDCREP_MODIFIED;
        goto cleanup;
    }

    code = encode_krb5_s4u_userid(&rep_s4u_user->user_id, &datap);
    if (code != 0)
        goto cleanup;

    if (rep_s4u_user->user_id.options & KRB5_S4U_OPTS_USE_REPLY_KEY_USAGE)
        usage = KRB5_KEYUSAGE_PA_S4U_X509_USER_REPLY;
    else
        usage = KRB5_KEYUSAGE_PA_S4U_X509_USER_REQUEST;

    code = krb5_c_verify_checksum(context, subkey, usage, datap,
                                  &rep_s4u_user->cksum, &valid);
    if (code != 0)
        goto cleanup;
    if (valid == FALSE) {
        code = KRB5_KDCREP_MODIFIED;
        goto cleanup;
    }

    /*
     * KDCs that support KRB5_S4U_OPTS_USE_REPLY_KEY_USAGE also return
     * S4U enc_padata for older (pre-AES) encryption types only.
     */
    if (not_newer) {
        if (enc_s4u_padata == NULL) {
            if (rep_s4u_user->user_id.options & KRB5_S4U_OPTS_USE_REPLY_KEY_USAGE) {
                code = KRB5_KDCREP_MODIFIED;
                goto cleanup;
            }
        } else {
            if (enc_s4u_padata->length != req_s4u_user->cksum.length + rep_s4u_user->cksum.length) {
                code = KRB5_KDCREP_MODIFIED;
                goto cleanup;
            }
            if (memcmp(enc_s4u_padata->contents,
                       req_s4u_user->cksum.contents, req_s4u_user->cksum.length) ||
                memcmp(&enc_s4u_padata->contents[req_s4u_user->cksum.length],
                       rep_s4u_user->cksum.contents, rep_s4u_user->cksum.length)) {
                code = KRB5_KDCREP_MODIFIED;
                goto cleanup;
            }
        }
    } else if (!krb5_c_is_keyed_cksum(rep_s4u_user->cksum.checksum_type)) {
        code = KRB5KRB_AP_ERR_INAPP_CKSUM;
        goto cleanup;
    }

cleanup:
    krb5_free_pa_s4u_x509_user(context, rep_s4u_user);
    krb5_free_data(context, datap);

    return code;
}

static krb5_error_code
krb5_get_self_cred_from_kdc(krb5_context context,
                            krb5_ccache ccache,
                            krb5_creds *in_creds,
                            krb5_data *subject_cert,
                            krb5_data *user_realm,
                            krb5_creds **out_creds,
                            krb5_creds ***tgts,
                            krb5_flags kdcopt)
{
    krb5_error_code code;
    krb5_principal tgs = NULL;
    krb5_creds tgtq, s4u_creds, *tgt = NULL, *tgtptr;
    krb5_creds *referral_tgts[KRB5_REFERRAL_MAXHOPS];
    krb5_pa_s4u_x509_user s4u_user;
    int referral_count = 0, i;

    memset(&tgtq, 0, sizeof(tgtq));
    memset(&s4u_creds, 0, sizeof(s4u_creds));
    memset(referral_tgts, 0, sizeof(referral_tgts));
    *out_creds = NULL;

    memset(&s4u_user, 0, sizeof(s4u_user));

    if (in_creds->client != NULL && krb5_princ_size(context, in_creds->client)) {
        if (krb5_princ_type(context, in_creds->client) == KRB5_NT_ENTERPRISE_PRINCIPAL) {
            code = krb5_build_principal_ext(context, &s4u_user.user_id.user,
                                            user_realm->length,
                                            user_realm->data,
                                            in_creds->client->data[0].length,
                                            in_creds->client->data[0].data,
                                            0);
            if (code != 0)
                goto cleanup;
            s4u_user.user_id.user->type = KRB5_NT_ENTERPRISE_PRINCIPAL;
        } else {
            code = krb5_copy_principal(context, in_creds->client, &s4u_user.user_id.user);
            if (code != 0)
                goto cleanup;
        }
    } else {
        code = krb5_build_principal_ext(context, &s4u_user.user_id.user,
                                        user_realm->length,
                                        user_realm->data);
        if (code != 0)
            goto cleanup;
        s4u_user.user_id.user->type = KRB5_NT_ENTERPRISE_PRINCIPAL;
    }
    if (subject_cert != NULL)
        s4u_user.user_id.subject_cert = *subject_cert;
    s4u_user.user_id.options = KRB5_S4U_OPTS_USE_REPLY_KEY_USAGE;

    /* First, acquire a TGT to the user's realm. */
    code = krb5_tgtname(context, user_realm,
                        krb5_princ_realm(context, in_creds->server), &tgs);
    if (code != 0)
        goto cleanup;

    tgtq.client = in_creds->server;
    tgtq.server = tgs;

    code = krb5_get_cred_from_kdc_opt(context, ccache, &tgtq,
                                      &tgt, tgts, kdcopt);
    if (code != 0)
        goto cleanup;

    tgtptr = tgt;

    code = krb5int_copy_creds_contents(context, in_creds, &s4u_creds);
    if (code != 0)
        goto cleanup;

    if (s4u_creds.client != NULL) {
        krb5_free_principal(context, s4u_creds.client);
        s4u_creds.client = NULL;
    }

    code = krb5_copy_principal(context, in_creds->server, &s4u_creds.client);
    if (code != 0)
        goto cleanup;

    /* Then, walk back the referral path to S4U2Self for user */
    for (referral_count = 0;
         referral_count < KRB5_REFERRAL_MAXHOPS;
         referral_count++)
    {
        krb5_pa_data **in_padata = NULL;
        krb5_pa_data **out_padata = NULL;
        krb5_pa_data **enc_padata = NULL;
        krb5_keyblock *subkey = NULL;

        if (s4u_user.user_id.user != NULL &&
            krb5_princ_size(context, s4u_user.user_id.user)) {
            in_padata = (krb5_pa_data **)calloc(2, sizeof(krb5_pa_data *));
            if (in_padata == NULL) {
                code = ENOMEM;
                goto cleanup;
            }
            code = build_pa_for_user(context, tgtptr, &s4u_user.user_id, &in_padata[0]);
            if (code != 0) {
                krb5_free_pa_data(context, in_padata);
                goto cleanup;
            }
        }

        /*
         * We need to rewrite the server realm, both so that libkrb5
         * can locate the correct KDC, and so that the KDC accepts
         * the response. This is unclear in the specification.
         */
        krb5_free_data_contents(context, &s4u_creds.server->realm);

        code = krb5int_copy_data_contents(context,
                                          &tgtptr->server->data[1],
                                          &s4u_creds.server->realm);
        if (code != 0)
            goto cleanup;

        code = krb5_get_cred_via_tkt_ext(context, tgtptr,
                                         KDC_OPT_CANONICALIZE |
                                         FLAGS2OPTS(tgtptr->ticket_flags) |
                                         kdcopt,
                                         tgtptr->addresses,
                                         in_padata, &s4u_creds,
                                         build_pa_s4u_x509_user, &s4u_user,
                                         &out_padata, &enc_padata,
                                         out_creds, &subkey);
        if (code != 0) {
            krb5_free_checksum_contents(context, &s4u_user.cksum);
            krb5_free_pa_data(context, in_padata);
            goto cleanup;
        }

        code = verify_s4u2self_reply(context, subkey, &s4u_user,
                                     out_padata, enc_padata);

        krb5_free_checksum_contents(context, &s4u_user.cksum);
        krb5_free_pa_data(context, in_padata);
        krb5_free_pa_data(context, out_padata);
        krb5_free_pa_data(context, enc_padata);
        krb5_free_keyblock(context, subkey);

        if (code != 0)
            goto cleanup;

        if (krb5_principal_compare(context, in_creds->server, (*out_creds)->server)) {
            assert(!krb5_principal_compare(context, in_creds->server, (*out_creds)->client));
            code = 0;
            goto cleanup;
        } else if (IS_TGS_PRINC(context, (*out_creds)->server)) {
            krb5_data *r1 = &tgtptr->server->data[1];
            krb5_data *r2 = &(*out_creds)->server->data[1];

            if (data_eq(*r1, *r2)) {
                krb5_free_creds(context, *out_creds);
                *out_creds = NULL;
                code = KRB5_ERR_HOST_REALM_UNKNOWN;
                break;
            }
            for (i = 0; i < referral_count; i++) {
                if (krb5_principal_compare(context,
                                           (*out_creds)->server,
                                           referral_tgts[i]->server)) {
                    code = KRB5_KDC_UNREACH;
                    goto cleanup;
                }
            }
            tgtptr = *out_creds;
            referral_tgts[referral_count] = *out_creds;
            *out_creds = NULL;
        } else {
            krb5_free_creds(context, *out_creds);
            *out_creds = NULL;
            code = KRB5_ERR_HOST_REALM_UNKNOWN; /* XXX */
            break;
        }
    }

cleanup:
    if (referral_tgts[0] != NULL) {
        krb5_creds **tgts2;

        if (*tgts != NULL) {
            for (i = 0; (*tgts)[i] != NULL; i++)
                ;
        } else
            i = 0;

        /* storing the first referral only mirrors krb5_get_cred_from_kdc_opt() */
        tgts2 = (krb5_creds **)realloc(*tgts, (i + 2) * sizeof(krb5_creds *));
        tgts2[i] = referral_tgts[0];
        referral_tgts[0] = NULL;

        tgts2[i + 1] = NULL;

        *tgts = tgts2;
    }

    for (i = 0; i < KRB5_REFERRAL_MAXHOPS; i++) {
        if (referral_tgts[i] != NULL)
            krb5_free_creds(context, referral_tgts[i]);
    }
    if (tgs != NULL)
        krb5_free_principal(context, tgs);
    if (tgt != NULL)
        krb5_free_creds(context, tgt);
    krb5_free_cred_contents(context, &s4u_creds);
    if (s4u_user.user_id.user != NULL)
        krb5_free_principal(context, s4u_user.user_id.user);
    krb5_free_checksum_contents(context, &s4u_user.cksum);

    return code;
}

krb5_error_code KRB5_CALLCONV
krb5_get_credentials_for_user(krb5_context context, krb5_flags options,
                              krb5_ccache ccache, krb5_creds *in_creds,
                              krb5_data *subject_cert,
                              krb5_creds **out_creds)
{
    krb5_error_code code;
    krb5_principal realm = NULL;
    krb5_creds **tgts = NULL;
    krb5_flags kdcopt;

    *out_creds = NULL;

    if (in_creds->client != NULL) {
        /* Uncanonicalised check */
        code = krb5_get_credentials(context, options | KRB5_GC_CACHED,
                                    ccache, in_creds, out_creds);
        if (code != KRB5_CC_NOTFOUND && code != KRB5_CC_NOT_KTYPE)
            goto cleanup;
    }

    code = s4u_identify_user(context, in_creds, subject_cert, &realm);
    if (code != 0)
        goto cleanup;

    code = krb5_get_credentials(context, options | KRB5_GC_CACHED,
                                ccache, in_creds, out_creds);
    if (code != KRB5_CC_NOTFOUND && code != KRB5_CC_NOT_KTYPE)
        goto cleanup;

    kdcopt = 0;
    if (options & KRB5_GC_CANONICALIZE)
        kdcopt |= KDC_OPT_CANONICALIZE;

    code = krb5_get_self_cred_from_kdc(context, ccache,
                                       in_creds, subject_cert,
                                       krb5_princ_realm(context, realm),
                                       out_creds, &tgts, kdcopt);
    if (code != 0)
        goto cleanup;

    assert(*out_creds != NULL);

    code = krb5_cc_store_cred(context, ccache, *out_creds);
    if (code != 0)
        goto cleanup;

    if (tgts != NULL) {
        int i = 0;
        krb5_error_code code2;

        while (tgts[i] != NULL) {
            code2 = krb5_cc_store_cred(context, ccache, tgts[i]);
            if (code2 != 0) {
                code = code2;
                break;
            }
            i++;
        }
        krb5_free_tgt_creds(context, tgts);
    }


cleanup:
    if (code != 0 && *out_creds != NULL) {
        krb5_free_creds(context, *out_creds);
        *out_creds = NULL;
    }

    if (realm != NULL)
        krb5_free_principal(context, realm);

    return code;
}

/*
 * Implements S4U2Proxy, by which a service can request a ticket to
 * a service with the client name set to a principal that authenticated
 * to the original service.
 */

static krb5_error_code
krb5_get_proxy_cred_from_kdc(krb5_context context,
                             krb5_ccache ccache,
                             krb5_creds *in_creds,
                             krb5_ticket *evidence_tkt,
                             krb5_creds **out_creds,
                             krb5_creds ***tgts,
                             krb5_flags kdcopt)

{
    krb5_error_code code;
    krb5_data *evidence_tkt_data = NULL;
    krb5_creds ecreds;

    *out_creds = NULL;
    *tgts = NULL;

    if (in_creds == NULL || (kdcopt & KDC_OPT_ENC_TKT_IN_SKEY)) {
        code = EINVAL;
        goto cleanup;
    }

    code = encode_krb5_ticket(evidence_tkt, &evidence_tkt_data);
    if (code != 0)
        goto cleanup;

    ecreds = *in_creds;
    ecreds.client = evidence_tkt->server;
    ecreds.second_ticket = *evidence_tkt_data;

    kdcopt |= KDC_OPT_FORWARDABLE | KDC_OPT_CNAME_IN_ADDL_TKT;

    code = krb5_get_cred_from_kdc_opt(context, ccache, &ecreds, out_creds,
                                      tgts, kdcopt);
    if (code != 0)
        goto cleanup;

    /*
     * Check client name because we couldn't compare that inside
     * krb5_get_cred_via_tkt_ext(); server should have been checked
     * there, but just to be sure we check that too.
     */
    if (!krb5_principal_compare(context, evidence_tkt->enc_part2->client, (*out_creds)->client) ||
        !krb5_principal_compare(context, in_creds->server, (*out_creds)->server)) {
        code = KRB5_KDCREP_MODIFIED; /* XXX */
        goto cleanup;
    }
    /*
     * Check the returned ticket is marked as forwardable.
     */
    if (((*out_creds)->ticket_flags & TKT_FLG_FORWARDABLE) == 0) {
        code = KRB5_TKT_NOT_FORWARDABLE;
        goto cleanup;
    }

cleanup:
    if (evidence_tkt_data != NULL)
        krb5_free_data(context, evidence_tkt_data);
    if (*out_creds != NULL && code != 0) {
        krb5_free_creds(context, *out_creds);
        *out_creds = NULL;
    }

    return code;
}

/*
 * Exported API for constrained delegation (S4U2Proxy).
 */
krb5_error_code KRB5_CALLCONV
krb5_get_credentials_for_proxy(krb5_context context,
                               krb5_flags options,
                               krb5_ccache ccache,
                               krb5_creds *in_creds,
                               krb5_ticket *evidence_tkt,
                               krb5_creds **out_creds)
{
    krb5_error_code code;
    krb5_creds mcreds;
    krb5_creds *ncreds = NULL;
    krb5_creds **tgts = NULL;
    krb5_flags fields;
    int kdcopt;

    *out_creds = NULL;

    if (in_creds == NULL || in_creds->client == NULL ||
        evidence_tkt == NULL || evidence_tkt->enc_part2 == NULL) {
        code = EINVAL;
        goto cleanup;
    }

    /* Caller should have set in_creds->client to match evidence ticket client */
    if (!krb5_principal_compare(context, evidence_tkt->enc_part2->client, in_creds->client)) {
        code = EINVAL;
        goto cleanup;
    }

    code = krb5_get_credentials_core(context, options, in_creds, &mcreds, &fields);
    if (code != 0)
        goto cleanup;

    ncreds = (krb5_creds *)calloc(1, sizeof(*ncreds));
    if (ncreds == NULL) {
        code = ENOMEM;
        goto cleanup;
    }
    ncreds->magic = KV5M_CRED;

    code = krb5_cc_retrieve_cred(context, ccache, fields, &mcreds, ncreds);
    if (code != 0) {
        free(ncreds);
        ncreds = in_creds;
    } else {
        *out_creds = ncreds;
    }

    if ((code != KRB5_CC_NOTFOUND && code != KRB5_CC_NOT_KTYPE)
        || options & KRB5_GC_CACHED)
        goto cleanup;

    kdcopt = 0;
    if (options & KRB5_GC_CANONICALIZE)
        kdcopt |= KDC_OPT_CANONICALIZE;

    /* Actually make the S4U2Proxy request to the KDC */
    code = krb5_get_proxy_cred_from_kdc(context, ccache, ncreds, evidence_tkt,
                                        out_creds, &tgts, kdcopt);
    if (code != 0)
        goto cleanup;

    code = krb5_cc_store_cred(context, ccache, *out_creds);
    if (code != 0)
        goto cleanup;

    if (tgts != NULL) {
        int i = 0;
        krb5_error_code code2;

        while (tgts[i] != NULL) {
            code2 = krb5_cc_store_cred(context, ccache, tgts[i]);
            if (code2 != 0) {
                code = code2;
                break;
            }
            i++;
        }
        krb5_free_tgt_creds(context, tgts);
    }

cleanup:
    if (*out_creds != NULL && code != 0) {
        krb5_free_creds(context, *out_creds);
        *out_creds = NULL;
    }

    return code;
}