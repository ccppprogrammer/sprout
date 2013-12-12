/**
 * @file authentication.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

extern "C" {
#include <pjsip.h>
#include <pjlib-util.h>
#include <pjlib.h>
}

// Common STL includes.
#include <cassert>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <queue>
#include <string>
#include <boost/algorithm/string/predicate.hpp>
#include <json/reader.h>

#include "log.h"
#include "stack.h"
#include "sasevent.h"
#include "pjutils.h"
#include "constants.h"
#include "analyticslogger.h"
#include "hssconnection.h"
#include "authentication.h"
#include "avstore.h"


//
// mod_auth authenticates SIP requests.  It must be inserted into the
// stack below the transaction layer.
//
static pj_bool_t authenticate_rx_request(pjsip_rx_data *rdata);

pjsip_module mod_auth =
{
  NULL, NULL,                         // prev, next
  pj_str("mod-auth"),                 // Name
  -1,                                 // Id
  PJSIP_MOD_PRIORITY_TSX_LAYER-1,     // Priority
  NULL,                               // load()
  NULL,                               // start()
  NULL,                               // stop()
  NULL,                               // unload()
  &authenticate_rx_request,           // on_rx_request()
  NULL,                               // on_rx_response()
  NULL,                               // on_tx_request()
  NULL,                               // on_tx_response()
  NULL,                               // on_tsx_state()
};


// Connection to the HSS service for retrieving subscriber credentials.
static HSSConnection* hss;


// AV store used to store Authentication Vectors while waiting for the
// client to respond to a challenge.
static AvStore* av_store;


// Analytics logger.
static AnalyticsLogger* analytics;


// PJSIP structure for control server authentication functions.
pjsip_auth_srv auth_srv;



pj_status_t user_lookup(pj_pool_t *pool,
                        const pjsip_auth_lookup_cred_param *param,
                        pjsip_cred_info *cred_info)
{
  const pj_str_t* acc_name = &param->acc_name;
  const pj_str_t* realm = &param->realm;
  const pjsip_rx_data* rdata = param->rdata;

  pj_status_t status = PJSIP_EAUTHACCNOTFOUND;

  // Get the impi and the nonce.  There must be an authorization header otherwise
  // PJSIP wouldn't have called this method.
  std::string impi = PJUtils::pj_str_to_string(acc_name);
  pjsip_authorization_hdr* auth_hdr = (pjsip_authorization_hdr*)
           pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_AUTHORIZATION, NULL);
  std::string nonce = PJUtils::pj_str_to_string(&auth_hdr->credential.digest.nonce);

  // Get the Authentication Vector from the store.
  Json::Value* av = av_store->get_av(impi, nonce);

  if (av != NULL)
  {
    pj_strdup(pool, &cred_info->realm, realm);
    pj_cstr(&cred_info->scheme, "digest");
    pj_strdup(pool, &cred_info->username, acc_name);
    if (av->isMember("aka"))
    {
      // AKA authentication, so response is plain-text password.
      cred_info->data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
      pj_strdup2(pool, &cred_info->data, (*av)["aka"]["response"].asCString());
      status = PJ_SUCCESS;
    }
    else if (av->isMember("digest"))
    {
      // Digest authentication, so ha1 field is hashed password.
      cred_info->data_type = PJSIP_CRED_DATA_DIGEST;
      pj_strdup2(pool, &cred_info->data, (*av)["digest"]["ha1"].asCString());
      status = PJ_SUCCESS;
    }
    delete av;
  }

  return status;
}


void create_challenge(pjsip_authorization_hdr* auth_hdr,
                      pjsip_rx_data* rdata,
                      pjsip_tx_data* tdata)
{
  // Get the public and private identities from the request.
  std::string impi;
  std::string impu;
  std::string nonce;
  std::string autn;

  pjsip_uri* to_uri = (pjsip_uri*)pjsip_uri_get_uri(PJSIP_MSG_TO_HDR(rdata->msg_info.msg)->uri);
  impu = PJUtils::public_id_from_uri(to_uri);
  if ((auth_hdr != NULL) &&
      (auth_hdr->credential.digest.username.slen != 0))
  {
    // private user identity is supplied in the Authorization header so use it.
    impi = PJUtils::pj_str_to_string(&auth_hdr->credential.digest.username);
    LOG_DEBUG("Private identity from authorization header = %s", impi.c_str());
  }
  else
  {
    // private user identity not supplied, so construct a default from the
    // public user identity by stripping the sip: prefix.
    impi = PJUtils::default_private_id_from_uri(to_uri);
    LOG_DEBUG("Private identity defaulted from public identity = %s", impi.c_str());
  }

  if (auth_hdr != NULL)
  {
    // Check for an AUTN parameter indicating a resync is required.
    pjsip_param* p = auth_hdr->credential.digest.other_param.next;
    while ((p != NULL) && (p != &auth_hdr->credential.digest.other_param))
    {
      if (pj_stricmp(&p->name, &STR_AUTN) == 0)
      {
        autn = PJUtils::pj_str_to_string(&p->value);
      }
      p = p->next;
    }
  }

  // Get the Authentication Vector from the HSS.
  Json::Value* av = hss->get_auth_vector(impi, impu, autn, get_trail(rdata));

  if (av != NULL)
  {
    // Retrieved a valid authentication vector, so generate the challenge.
    LOG_DEBUG("Valid AV - generate challenge");
    char buf[16];
    pj_str_t random;
    random.ptr = buf;
    random.slen = sizeof(buf);

    LOG_DEBUG("Create WWW-Authenticate header");
    pjsip_www_authenticate_hdr* hdr = pjsip_www_authenticate_hdr_create(tdata->pool);
    LOG_DEBUG("Created");

    hdr->scheme = STR_DIGEST;
    LOG_DEBUG("Add realm");
    pj_strdup(tdata->pool, &hdr->challenge.digest.realm, &auth_srv.realm);

    if (av->isMember("aka"))
    {
      // AKA authentication.
      LOG_DEBUG("Add AKA information");
      Json::Value* aka = &(*av)["aka"];
      hdr->challenge.digest.algorithm = STR_AKAV1_MD5;
      nonce = (*aka)["challenge"].asString();
      pj_strdup2(tdata->pool, &hdr->challenge.digest.nonce, nonce.c_str());
      pj_create_random_string(buf, sizeof(buf));
      pj_strdup(tdata->pool, &hdr->challenge.digest.opaque, &random);
      hdr->challenge.digest.qop = STR_AUTH;
      hdr->challenge.digest.stale = PJ_FALSE;

      // Add the cryptography key parameter.
      pjsip_param* ck_param = (pjsip_param*)pj_pool_alloc(tdata->pool, sizeof(pjsip_param));
      ck_param->name = STR_CK;
      pj_strdup2(tdata->pool, &ck_param->value, (*aka)["cryptkey"].asCString());
      pj_list_insert_before(&hdr->challenge.digest.other_param, ck_param);

      // Add the integrity key parameter.
      pjsip_param* ik_param = (pjsip_param*)pj_pool_alloc(tdata->pool, sizeof(pjsip_param));
      ik_param->name = STR_IK;
      pj_strdup2(tdata->pool, &ik_param->value, (*aka)["integritykey"].asCString());
      pj_list_insert_before(&hdr->challenge.digest.other_param, ik_param);
    }
    else
    {
      // Digest authentication.
      LOG_DEBUG("Add Digest information");
      Json::Value* digest = &(*av)["digest"];
      hdr->challenge.digest.algorithm = STR_MD5;
      pj_create_random_string(buf, sizeof(buf));
      nonce.assign(buf, sizeof(buf));
      pj_strdup(tdata->pool, &hdr->challenge.digest.nonce, &random);
      pj_create_random_string(buf, sizeof(buf));
      pj_strdup(tdata->pool, &hdr->challenge.digest.opaque, &random);
      pj_strdup2(tdata->pool, &hdr->challenge.digest.qop, (*digest)["qop"].asCString());
      hdr->challenge.digest.stale = PJ_FALSE;
    }

    // Add the header to the message.
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)hdr);

    // Write the authentication vector (as a JSON string) into the AV store.
    LOG_DEBUG("Write AV to store");
    av_store->set_av(impi, nonce, av);

    delete av;
  }
  else
  {
    LOG_DEBUG("Failed to get Authentication vector");
    tdata->msg->line.status.code = PJSIP_SC_FORBIDDEN;
  }

  return;
}


pj_bool_t authenticate_rx_request(pjsip_rx_data* rdata)
{
  pj_status_t status;

  if (rdata->msg_info.msg->line.req.method.id != PJSIP_REGISTER_METHOD)
  {
    // Non-REGISTER request, so don't do authentication as it must have come
    // from an authenticated or trusted source.
    return PJ_FALSE;
  }

  // Check to see if the request has already been integrity protected?
  pjsip_authorization_hdr* auth_hdr = (pjsip_authorization_hdr*)
           pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_AUTHORIZATION, NULL);

  if (auth_hdr != NULL)
  {
    LOG_DEBUG("Authorization header in request");
    pjsip_param* integrity =
           pjsip_param_find(&auth_hdr->credential.digest.other_param,
                            &STR_INTEGRITY_PROTECTED);

    if ((integrity != NULL) &&
        ((pj_stricmp(&integrity->value, &STR_YES) == 0) ||
         (pj_stricmp(&integrity->value, &STR_TLS_YES) == 0) ||
         (pj_stricmp(&integrity->value, &STR_IP_ASSOC_YES) == 0)))
    {
      // Request is already integrity protected, so let it through.
      LOG_INFO("Request integrity protected by edge proxy");
      return PJ_FALSE;
    }
  }

  int sc = PJSIP_SC_UNAUTHORIZED;
  status = PJSIP_EAUTHNOAUTH;

  if ((auth_hdr != NULL) &&
      (auth_hdr->credential.digest.response.slen != 0))
  {
    // Request contains a response to a previous challenge, so pass it to
    // the authentication module to verify.
    LOG_DEBUG("Verify authentication information in request");
    status = pjsip_auth_srv_verify(&auth_srv, rdata, &sc);
    if (status == PJ_SUCCESS)
    {
      // The authentication information in the request was verified, so let
      // the message through.
      LOG_DEBUG("Request authenticated successfully");
      return PJ_FALSE;
    }
  }

  // The message either has insufficient authentication information, or
  // has failed authentication.  In either case, the message will be
  // absorbed and responded to by the authentication module, so we need to
  // add SAS markers so the trail will become searchable.
  SAS::TrailId trail = get_trail(rdata);
  SAS::Marker start_marker(trail, MARKER_ID_START, 1u);
  SAS::report_marker(start_marker);
  if (rdata->msg_info.from)
  {
    SAS::Marker calling_dn(trail, MARKER_ID_CALLING_DN, 1u);
    pjsip_sip_uri* calling_uri = (pjsip_sip_uri*)pjsip_uri_get_uri(rdata->msg_info.from->uri);
    calling_dn.add_var_param(calling_uri->user.slen, calling_uri->user.ptr);
    SAS::report_marker(calling_dn);
  }

  if (rdata->msg_info.to)
  {
    SAS::Marker called_dn(trail, MARKER_ID_CALLED_DN, 1u);
    pjsip_sip_uri* called_uri = (pjsip_sip_uri*)pjsip_uri_get_uri(rdata->msg_info.to->uri);
    called_dn.add_var_param(called_uri->user.slen, called_uri->user.ptr);
    SAS::report_marker(called_dn);
  }

  if (rdata->msg_info.cid)
  {
    SAS::Marker cid(trail, MARKER_ID_SIP_CALL_ID, 1u);
    cid.add_var_param(rdata->msg_info.cid->id.slen, rdata->msg_info.cid->id.ptr);
    SAS::report_marker(cid, SAS::Marker::Scope::Trace);
  }

  // Add a SAS end marker
  SAS::Marker end_marker(trail, MARKER_ID_END, 1u);
  SAS::report_marker(end_marker);

  if (rdata->msg_info.msg->line.req.method.id == PJSIP_ACK_METHOD)
  {
    // Discard unauthenticated ACK request since we can't reject or challenge it.
    LOG_VERBOSE("Discard unauthenticated ACK request");
  }
  else if (rdata->msg_info.msg->line.req.method.id == PJSIP_CANCEL_METHOD)
  {
    // Reject an unauthenticated CANCEL as it cannot be challenged (see RFC3261
    // section 22.1).
    LOG_VERBOSE("Reject unauthenticated CANCEL request");
    PJUtils::respond_stateless(stack_data.endpt,
                               rdata,
                               PJSIP_SC_FORBIDDEN,
                               NULL,
                               NULL,
                               NULL);
  }
  else if (status == PJSIP_EAUTHNOAUTH)
  {
    // No authorization information in request, or stale, so must issue challenge
    LOG_DEBUG("No authentication information in request, so reject with challenge");
    pjsip_tx_data* tdata;
    status = PJUtils::create_response(stack_data.endpt, rdata, sc, NULL, &tdata);
    if (status != PJ_SUCCESS)
    {
      LOG_ERROR("Error building challenge response, %s",
                PJUtils::pj_status_to_string(status).c_str());
      PJUtils::respond_stateless(stack_data.endpt,
                                 rdata,
                                 PJSIP_SC_INTERNAL_SERVER_ERROR,
                                 NULL,
                                 NULL,
                                 NULL);
      return PJ_TRUE;
    }

    create_challenge(auth_hdr, rdata, tdata);
    status = pjsip_endpt_send_response2(stack_data.endpt, rdata, tdata, NULL, NULL);
  }
  else
  {
    // Authentication failed.
    LOG_ERROR("Authentication failed, %s",
              PJUtils::pj_status_to_string(status).c_str());
    if (analytics != NULL)
    {
      analytics->auth_failure(PJUtils::pj_str_to_string(&auth_hdr->credential.digest.username),
                              PJUtils::aor_from_uri((pjsip_sip_uri*)pjsip_uri_get_uri(PJSIP_MSG_TO_HDR(rdata->msg_info.msg)->uri)));
    }

    // @TODO - need more diagnostics here so we can identify and flag
    // attacks.

    // Reject the request.
    PJUtils::respond_stateless(stack_data.endpt,
                               rdata,
                               sc,
                               NULL,
                               NULL,
                               NULL);
  }

  return PJ_TRUE;
}


pj_status_t init_authentication(const std::string& realm_name,
                                AvStore* avstore,
                                HSSConnection* hss_connection,
                                AnalyticsLogger* analytics_logger)
{
  pj_status_t status;

  av_store = avstore;
  hss = hss_connection;
  analytics = analytics_logger;

  // Register the authentication module.  This needs to be in the stack
  // before the transaction layer.
  status = pjsip_endpt_register_module(stack_data.endpt, &mod_auth);

  // Initialize the authorization server.
  pj_str_t realm = (realm_name != "") ? pj_strdup3(stack_data.pool, realm_name.c_str()) : stack_data.local_host;
  LOG_STATUS("Initializing authentication server for realm %.*s", realm.slen, realm.ptr);
  pjsip_auth_srv_init_param params;
  params.realm = &realm;
  params.lookup2 = user_lookup;
  params.options = 0;
  status = pjsip_auth_srv_init2(stack_data.pool, &auth_srv, &params);

  return status;
}


void destroy_authentication()
{
  pjsip_endpt_unregister_module(stack_data.endpt, &mod_auth);
}

