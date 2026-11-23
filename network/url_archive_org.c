/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 Cartlink contributors
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_NETWORKING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>
#include <retro_timers.h>
#include <features/features_cpu.h>
#include <net/net_http.h>

#include "url_content.h"
#include "url_logins.h"
#include "url_archive_org.h"

/* Overridable only so a test build can point the menu flow at a local
 * fake server; release builds always use archive.org. */
#ifndef UA_LOGIN_URL
#define UA_LOGIN_URL   "https://archive.org/services/xauthn/?op=login"
#endif
#define UA_FIELD_LEN   256
#define UA_FORM_LEN    (UA_FIELD_LEN * 2 + 32)
#define UA_MAX_BODY    ((size_t)1024 * 1024)
#define UA_STALL_USEC  30000000   /* give up after 30 s without data */

static char ua_test_url[1024] = {0};

static void ua_set_err(char *err, size_t elen, const char *msg)
{
   if (err && elen)
      strlcpy(err, msg, elen);
}

void url_archive_org_set_test_url(const char *url)
{
   if (url)
      strlcpy(ua_test_url, url, sizeof(ua_test_url));
   else
      ua_test_url[0] = '\0';
}

/* POSTs form to url (https: only with a verified certificate). On a
 * reply, *status is its HTTP status and *data / *len hold the body
 * whatever the status (caller frees; NULL if empty). Returns false
 * with err set if there is no usable reply. */
static bool ua_post(const char *url, const char *form, size_t form_len,
      int *status_out, uint8_t **data, size_t *len, char *err, size_t elen)
{
   struct http_connection_t *conn;
   struct http_t *http;
   retro_time_t last_change;
   size_t last_done = (size_t)-1;
   bool stalled     = false;
   int tls_error;
   int status;

   *data       = NULL;
   *len        = 0;
   *status_out = -1;

   if (!(conn = net_http_connection_new(url, "POST", NULL)))
   {
      ua_set_err(err, elen, "bad sign-in address");
      return false;
   }
   if (     !net_http_connection_iterate(conn)
         || !net_http_connection_done(conn))
   {
      net_http_connection_free(conn);
      ua_set_err(err, elen, "bad sign-in address");
      return false;
   }
   net_http_connection_set_content(conn,
         "application/x-www-form-urlencoded", form_len, form);
   net_http_connection_set_user_agent(conn, "RetroArch");
   /* Never re-send the password to wherever a redirect points. */
   net_http_connection_set_follow_redirects(conn, false);
   net_http_connection_set_max_body(conn, UA_MAX_BODY);
   /* The password only goes to a server whose certificate checks out. */
   net_http_connection_set_verify_tls(conn, true);

   http = net_http_new(conn);
   /* net_http wipes its copies of the body when it frees them. */
   net_http_connection_free(conn);
   if (!http)
   {
      ua_set_err(err, elen, "could not reach archive.org");
      return false;
   }

   last_change = cpu_features_get_time_usec();
   for (;;)
   {
      size_t done  = 0;
      size_t total = 0;

      if (net_http_update(http, &done, &total))
         break;
      if (done != last_done)
      {
         last_done   = done;
         last_change = cpu_features_get_time_usec();
      }
      else if (cpu_features_get_time_usec() - last_change > UA_STALL_USEC)
      {
         stalled = true;
         break;
      }
      else
         retro_sleep(1);
   }

   status    = net_http_status(http);
   tls_error = net_http_tls_error(http);
   /* net_http_error is also true for any non-2xx status; a transport
    * error leaves the status at -1. */
   if (!stalled && status > 0)
      *data = net_http_data(http, len, true);
   net_http_delete(http);

   if (tls_error)
   {
      ua_set_err(err, elen, tls_error == NET_HTTP_TLS_NO_CA
            ? "can't check archive.org's security certificate, so the password was not sent"
            : "archive.org's security certificate is not valid, so the password was not sent");
      return false;
   }
   if (stalled || status < 0)
   {
      ua_set_err(err, elen, "could not reach archive.org");
      return false;
   }
   *status_out = status;
   if (*data && !*len)
   {
      free(*data);
      *data = NULL;
   }
   return true;
}

bool url_archive_org_login(const char *email, const char *password,
      char *access, size_t alen, char *secret, size_t slen,
      char *err, size_t elen)
{
   char email_enc[UA_FIELD_LEN * 3];
   char pass_enc[UA_FIELD_LEN * 3];
   char form[UA_FORM_LEN * 3];
   uint8_t *data = NULL;
   size_t len    = 0;
   int status    = -1;
   int form_len;
   bool ok       = false;

   if (access && alen)
      access[0] = '\0';
   if (secret && slen)
      secret[0] = '\0';
   ua_set_err(err, elen, "");

   if (!access || !alen || !secret || !slen)
   {
      ua_set_err(err, elen, "no room for the keys");
      return false;
   }
   if (!email || !*email || !password || !*password)
   {
      ua_set_err(err, elen, "email and password are needed");
      return false;
   }

   if (     !url_content_form_encode(email_enc, sizeof(email_enc), email)
         || !url_content_form_encode(pass_enc, sizeof(pass_enc), password))
   {
      ua_set_err(err, elen, "email or password too long");
      goto end;
   }
   form_len = snprintf(form, sizeof(form), "email=%s&password=%s",
         email_enc, pass_enc);
   if (form_len < 0 || (size_t)form_len >= sizeof(form))
   {
      ua_set_err(err, elen, "email or password too long");
      goto end;
   }

   if (!ua_post(*ua_test_url ? ua_test_url : UA_LOGIN_URL,
            form, (size_t)form_len, &status, &data, &len, err, elen))
      goto end;

   /* archive.org answers a wrong password with an error status and a
    * JSON reason: read the reply whatever the status. */
   if (!data)
   {
      char msg[64];
      if (status == 200)
         strlcpy(msg, "empty reply", sizeof(msg));
      else
         snprintf(msg, sizeof(msg), "server answered %d", status);
      ua_set_err(err, elen, msg);
      goto end;
   }
   if (!url_content_parse_archive_login((const char*)data, len,
            access, alen, secret, slen, err, elen))
   {
      /* A reply that is not the login JSON: the status says more. */
      if (     status != 200 && err && elen
            && string_is_equal(err, "unexpected reply"))
         snprintf(err, elen, "server answered %d", status);
      goto end;
   }
   if (status != 200)
   {
      char msg[64];
      /* Keys in an error reply are not trusted. */
      url_content_wipe(access, alen);
      url_content_wipe(secret, slen);
      snprintf(msg, sizeof(msg), "server answered %d", status);
      ua_set_err(err, elen, msg);
      goto end;
   }

   if (!url_logins_set_archive_keys(access, secret))
   {
      url_content_wipe(access, alen);
      url_content_wipe(secret, slen);
      ua_set_err(err, elen, "could not save the keys");
      goto end;
   }
   ok = true;

end:
   if (data)
   {
      url_content_wipe(data, len);
      free(data);
   }
   url_content_wipe(pass_enc, sizeof(pass_enc));
   url_content_wipe(form, sizeof(form));
   return ok;
}

#endif /* HAVE_NETWORKING */
