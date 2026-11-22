/*  RetroArch - A frontend for libretro.
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

static void ua_wipe(void *p, size_t len)
{
   volatile unsigned char *b = (volatile unsigned char*)p;
   while (len--)
      *b++ = 0;
}

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

/* POSTs form to url. On a 200 reply, *data / *len hold the body
 * (caller frees). Returns false with err set otherwise. */
static bool ua_post(const char *url, const char *form, size_t form_len,
      uint8_t **data, size_t *len, char *err, size_t elen)
{
   struct http_connection_t *conn;
   struct http_t *http;
   retro_time_t last_change;
   size_t last_done = (size_t)-1;
   bool stalled     = false;
   int status;

   *data = NULL;
   *len  = 0;

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

   status = net_http_status(http);
   if (!stalled && !net_http_error(http) && status == 200)
      *data = net_http_data(http, len, true);
   net_http_delete(http);

   if (*data)
      return true;

   if (stalled || status < 0)
      ua_set_err(err, elen, "could not reach archive.org");
   else if (status != 200)
   {
      char msg[64];
      snprintf(msg, sizeof(msg), "server answered %d", status);
      ua_set_err(err, elen, msg);
   }
   else
      ua_set_err(err, elen, "empty reply");
   return false;
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
            form, (size_t)form_len, &data, &len, err, elen))
      goto end;

   if (!url_content_parse_archive_login((const char*)data, len,
            access, alen, secret, slen, err, elen))
      goto end;

   if (!url_logins_set_archive_keys(access, secret))
   {
      ua_wipe(access, alen);
      ua_wipe(secret, slen);
      ua_set_err(err, elen, "could not save the keys");
      goto end;
   }
   ok = true;

end:
   if (data)
   {
      ua_wipe(data, len);
      free(data);
   }
   ua_wipe(pass_enc, sizeof(pass_enc));
   ua_wipe(form, sizeof(form));
   return ok;
}

#endif /* HAVE_NETWORKING */
