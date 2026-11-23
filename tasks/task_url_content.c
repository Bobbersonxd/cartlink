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

/* "Load from URL" task: runs url_fetch_to_cache off the main thread,
 * asks for a login when the server answers 401, then hands the cached
 * file to the menu's normal content loading. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#ifdef HAVE_NETWORKING

#include <boolean.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <retro_miscellaneous.h>
#include <features/features_cpu.h>
#include <queues/task_queue.h>
#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif

#include "../configuration.h"
#include "../msg_hash.h"
#include "../paths.h"
#include "../retroarch.h"
#include "../runloop.h"
#include "../verbosity.h"
#include "../network/url_content.h"
#include "../network/url_fetch.h"
#include "../network/url_logins.h"
#include "../network/url_servers.h"
#include "../network/url_catalog.h"
#include "../network/url_archive_org.h"
#ifdef HAVE_MENU
#include "../menu/menu_driver.h"
#include "../menu/menu_input.h"
#include "../menu/menu_cbs.h"
#endif

#include "tasks_internal.h"

#define URL_TASK_URL_MAX   4096
#define URL_TASK_HOST_MAX  256
#define URL_TASK_CRED_MAX  256
#define URL_TASK_POLL_USEC 50000

/* Lifetime: the fetch thread only ever touches this struct (never the
 * task or the task queue). It is shared by two owners, counted in
 * refs under lock: the task (dropped in url_task_cleanup) and the
 * fetch thread while it runs (dropped as its very last step). The
 * last owner frees it. Normally the handler joins the thread before
 * the task finishes, so cleanup frees it. If RetroArch quits during a
 * download, task_queue_deinit drops the unfinished task without
 * calling cleanup, so the task's reference is never dropped and the
 * struct (and its lock) stay valid for the still-running thread until
 * the process exits. If cleanup ever runs while the thread is still
 * going, the thread is detached and frees the struct itself. */
typedef struct url_task_data
{
#ifdef HAVE_THREADS
   sthread_t *thread;
   slock_t *lock;
#endif
   enum url_fetch_result result;
   int refs;                  /* owners, under lock */
   int pct;                   /* written by the fetch thread, under lock */
   int shown_pct;             /* task thread only */
   bool started;
   bool done;
   bool has_login;
   bool anyway;               /* "Download anyway (unsafe)" was chosen */
   char url[URL_TASK_URL_MAX];
   char host[URL_TASK_HOST_MAX];
   char login_site[URL_TASK_HOST_MAX + 16]; /* https site a login went to */
   char cache_dir[PATH_MAX_LENGTH];
   char user[URL_TASK_CRED_MAX];
   char pass[URL_TASK_CRED_MAX];
   char out_path[PATH_MAX_LENGTH];
   char challenge[1024];
   char err[512];
   struct url_catalog catalog; /* a game list (URL_FETCH_CATALOG) */
} url_task_data_t;

/* Main thread only. busy is true while a download task runs; url,
 * host and user carry a login prompt across its two dialogs. */
static struct
{
   bool busy;
   char url[URL_TASK_URL_MAX];
   char host[URL_TASK_HOST_MAX];
   char user[URL_TASK_CRED_MAX];
} url_task_st;

static bool url_task_push(const char *url,
      const char *user, const char *pass, bool anyway);

static void url_task_notify(const char *msg, enum message_queue_category cat)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 240, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, cat);
}

static void url_task_data_free(url_task_data_t *d)
{
   url_content_wipe(d->user, sizeof(d->user));
   url_content_wipe(d->pass, sizeof(d->pass));
   url_catalog_free(&d->catalog);
#ifdef HAVE_THREADS
   slock_free(d->lock);
#endif
   free(d);
}

/* Drops one owner's reference; the last one frees d. */
static void url_task_data_release(url_task_data_t *d)
{
   int left;
#ifdef HAVE_THREADS
   slock_lock(d->lock);
#endif
   left = --d->refs;
#ifdef HAVE_THREADS
   slock_unlock(d->lock);
#endif
   if (left <= 0)
      url_task_data_free(d);
}

/* Called on the fetch thread: only stores the percentage in d. The
 * task handler hands it to the task queue. */
static void url_task_progress_cb(size_t done, size_t total, void *ud)
{
   url_task_data_t *d = (url_task_data_t*)ud;
   int pct;

   if (!total)
      return;
   if (done >= total)
      pct = 100;
   else
      pct = (int)(((double)done * 100.0) / (double)total);
#ifdef HAVE_THREADS
   slock_lock(d->lock);
#endif
   d->pct = pct;
#ifdef HAVE_THREADS
   slock_unlock(d->lock);
#endif
}

/* The download itself. Touches only d. */
static void url_task_fetch(url_task_data_t *d)
{
   d->result = url_fetch_to_cache_ex(d->url, d->cache_dir,
         d->has_login ? d->user : NULL,
         d->has_login ? d->pass : NULL,
         d->out_path, sizeof(d->out_path),
         d->challenge, sizeof(d->challenge),
         d->err, sizeof(d->err),
         url_task_progress_cb, d, &d->catalog, d->anyway,
         d->login_site, sizeof(d->login_site));
#ifdef HAVE_THREADS
   slock_lock(d->lock);
#endif
   d->done = true;
#ifdef HAVE_THREADS
   slock_unlock(d->lock);
#endif
}

#ifdef HAVE_THREADS
/* Fetch thread entry: the download, then drop the thread's reference
 * (the last thing this thread does with d). */
static void url_task_fetch_thread(void *ud)
{
   url_task_data_t *d = (url_task_data_t*)ud;
   url_task_fetch(d);
   url_task_data_release(d);
}
#endif

/* The message for a file the safety checks refused (and deleted). */
static enum msg_hash_enums url_task_blocked_msg(enum url_fetch_result res)
{
   switch (res)
   {
      case URL_FETCH_BLOCKED_PROGRAM:
         return MSG_URL_SAFETY_PROGRAM;
      case URL_FETCH_BLOCKED_TOO_BIG:
         return MSG_URL_SAFETY_TOO_BIG;
      case URL_FETCH_BLOCKED_FINGERPRINT:
         return MSG_URL_FINGERPRINT_MISMATCH;
      default:
         return MSG_URL_SAFETY_ARCHIVE;
   }
}

/* Replaces the task title with a final message, so the normal task
 * notification shows it. Error tasks show as failed. */
static void url_task_finish_msg(retro_task_t *task, const char *msg,
      bool is_error)
{
   task_free_title(task);
   task_set_title(task, strdup(msg));
   if (is_error)
      task_set_error(task, strdup(msg));
}

static void url_task_handler(retro_task_t *task)
{
   url_task_data_t *d = (url_task_data_t*)task->task_data;
   char msg[512];

   if (!d->started)
   {
      d->started = true;
#ifdef HAVE_THREADS
      if (d->lock)
      {
         slock_lock(d->lock);
         d->refs++;             /* the thread's reference */
         slock_unlock(d->lock);
         if (!(d->thread = sthread_create(url_task_fetch_thread, d)))
         {
            slock_lock(d->lock);
            d->refs--;
            slock_unlock(d->lock);
         }
      }
      if (d->thread)
      {
         task->when = cpu_features_get_time_usec() + URL_TASK_POLL_USEC;
         return;
      }
#endif
      url_task_fetch(d);
   }

#ifdef HAVE_THREADS
   if (d->thread)
   {
      bool done;
      int pct;
      slock_lock(d->lock);
      done = d->done;
      pct  = d->pct;
      slock_unlock(d->lock);
      /* Progress goes to the task queue from here, never from the
       * fetch thread. */
      if (pct != d->shown_pct)
      {
         d->shown_pct = pct;
         task_set_progress(task, (int8_t)pct);
      }
      if (!done)
      {
         task->when = cpu_features_get_time_usec() + URL_TASK_POLL_USEC;
         return;
      }
      sthread_join(d->thread);
      d->thread = NULL;
   }
#endif

   switch (d->result)
   {
      case URL_FETCH_OK:
      case URL_FETCH_CACHED: /* loading the game shows its own notice */
         break;
      case URL_FETCH_CATALOG: /* the callback opens it in the menu */
         {
            /* Replace "Downloading <host>", which would otherwise stay
             * up over the list that just opened. */
            char title[256];
            url_catalog_title(title, sizeof(title),
                  string_is_empty(d->catalog.base)
                  ? d->url : d->catalog.base);
            snprintf(msg, sizeof(msg),
                  msg_hash_to_str(MSG_URL_CATALOG_OPENED), title);
            url_task_finish_msg(task, msg, false);
         }
         break;
      case URL_FETCH_NEED_LOGIN:
         if (d->has_login)
         {
            snprintf(msg, sizeof(msg),
                  msg_hash_to_str(MSG_URL_CONTENT_LOGIN_FAILED), d->host);
            url_task_finish_msg(task, msg, true);
         }
         else
         {
            snprintf(msg, sizeof(msg),
                  msg_hash_to_str(MSG_URL_CONTENT_LOGIN_NEEDED), d->host);
            url_task_finish_msg(task, msg, false);
         }
         break;
      case URL_FETCH_NOT_A_FILE:
         url_task_finish_msg(task,
               msg_hash_to_str(MSG_URL_CONTENT_NOT_A_FILE), true);
         break;
      case URL_FETCH_BLOCKED_PROGRAM:
      case URL_FETCH_BLOCKED_ARCHIVE:
      case URL_FETCH_BLOCKED_TOO_BIG:
      case URL_FETCH_BLOCKED_FINGERPRINT:
         url_task_finish_msg(task,
               msg_hash_to_str(url_task_blocked_msg(d->result)), true);
         break;
      case URL_FETCH_CERT_INVALID: /* the callback offers the choice */
         url_task_finish_msg(task,
               msg_hash_to_str(MSG_URL_CERT_INVALID), false);
         break;
      case URL_FETCH_LOGIN_HTTP: /* never a login prompt over http */
         url_task_finish_msg(task,
               msg_hash_to_str(MSG_URL_LOGIN_HTTP), true);
         break;
      case URL_FETCH_TOO_BIG:
      case URL_FETCH_FAILED:
      default:
         url_task_finish_msg(task, string_is_empty(d->err)
               ? msg_hash_to_str(MSG_URL_CONTENT_FAILED) : d->err, true);
         break;
   }

   task->when = 0;
   task_set_progress(task, 100);
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void url_task_load(const char *path)
{
#ifdef HAVE_MENU
   char dir[PATH_MAX_LENGTH];

   fill_pathname_basedir(dir, path, sizeof(dir));
   if (action_ok_url_content_load_file(dir, path_basename(path)) != 0)
      RARCH_ERR("[URL] Could not load \"%s\".\n", path);
#else
   RARCH_ERR("[URL] No menu to load \"%s\" with.\n", path);
#endif
}

#ifdef HAVE_MENU
static void url_task_pass_cb(void *userdata, const char *line)
{
   if (!line)
   {
      menu_input_dialog_end();
      url_content_wipe(url_task_st.user, sizeof(url_task_st.user));
      return;
   }

   url_task_push(url_task_st.url, url_task_st.user, line, false);

   /* The line is the dialog's own heap buffer; clear the password
    * before the input code frees it. */
   url_content_wipe((char*)line, strlen(line));
   url_content_wipe(url_task_st.user, sizeof(url_task_st.user));
   menu_input_dialog_end();
}

static bool url_task_dialog_label(const char *label,
      input_keyboard_line_complete_t cb)
{
   menu_input_ctx_line_t line;
   struct menu_state *menu_st = menu_state_get_ptr();

   if (!(menu_st->flags & MENU_ST_FLAG_ALIVE))
      retroarch_menu_running();

   line.label         = label;
   line.label_setting = "no_setting";
   line.type          = 0;
   line.idx           = 0;
   line.cb            = cb;
   return menu_input_dialog_start(&line);
}

static bool url_task_dialog(enum msg_hash_enums fmt,
      input_keyboard_line_complete_t cb)
{
   char label[512];
   snprintf(label, sizeof(label), msg_hash_to_str(fmt), url_task_st.host);
   return url_task_dialog_label(label, cb);
}

/* Runs on_done on the next task queue pass (see below). */
static void url_task_next_pass(retro_task_callback_t on_done);

/* A dialog cannot be opened from inside another dialog's callback:
 * the input code frees the line right after the callback returns.
 * This empty task opens the password dialog on the next queue pass. */
static void url_task_open_pass_handler(retro_task_t *task)
{
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void url_task_open_pass_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   url_task_dialog(MSG_URL_CONTENT_PASSWORD_FOR, url_task_pass_cb);
}

static void url_task_next_pass(retro_task_callback_t on_done)
{
   retro_task_t *task;

   if (!(task = task_init()))
      return;
   task->handler  = url_task_open_pass_handler;
   task->callback = on_done;
   if (!task_queue_push(task))
      free(task);
}

static void url_task_user_cb(void *userdata, const char *line)
{
   if (string_is_empty(line))
   {
      menu_input_dialog_end();
      return;
   }

   strlcpy(url_task_st.user, line, sizeof(url_task_st.user));
   menu_input_dialog_end();

   url_task_next_pass(url_task_open_pass_callback);
}
#endif

/* ---------- game lists (one per open catalog menu level) ---------- */

#define URL_CATALOG_LEVELS 32

/* Main thread only. Each open catalog menu level owns one entry; it is
 * freed when the menu pops that level (task_url_catalog_prune from
 * menu_driver.c) or when a new level is stored at its depth. */
static struct
{
   size_t depth;
   struct url_catalog cat;
} url_catalog_levels[URL_CATALOG_LEVELS];
static size_t url_catalog_level_count;

void task_url_catalog_prune(size_t depth)
{
   size_t i = 0;

   while (i < url_catalog_level_count)
   {
      if (url_catalog_levels[i].depth > depth)
      {
         url_catalog_free(&url_catalog_levels[i].cat);
         url_catalog_levels[i] =
               url_catalog_levels[--url_catalog_level_count];
         memset(&url_catalog_levels[url_catalog_level_count], 0,
               sizeof(url_catalog_levels[0]));
      }
      else
         i++;
   }
}

const struct url_catalog *task_url_catalog_get(size_t depth,
      const char *url)
{
   size_t i;

   if (!url)
      return NULL;
   for (i = 0; i < url_catalog_level_count; i++)
      if (     url_catalog_levels[i].depth == depth
            && string_is_equal(url_catalog_levels[i].cat.base, url))
         return &url_catalog_levels[i].cat;
   return NULL;
}

#ifdef HAVE_MENU
/* Takes cat over (cat is zeroed) for the level at depth, replacing any
 * list at that depth or deeper. False (cat freed) if there is no room. */
static bool url_catalog_level_store(size_t depth, struct url_catalog *cat)
{
   task_url_catalog_prune(depth - 1);
   if (url_catalog_level_count >= URL_CATALOG_LEVELS)
   {
      url_catalog_free(cat);
      return false;
   }
   url_catalog_levels[url_catalog_level_count].depth = depth;
   url_catalog_levels[url_catalog_level_count].cat   = *cat;
   url_catalog_level_count++;
   memset(cat, 0, sizeof(*cat));
   return true;
}

/* A link answered with a game list: remember the server and open the
 * list as a new menu level on top of the current one. */
static void url_task_open_catalog(url_task_data_t *d)
{
   char root[URL_CATALOG_URL_LEN];
   char url[URL_CATALOG_URL_LEN];
   struct menu_state *menu_st = menu_state_get_ptr();
   file_list_t *stack;
   size_t depth;

   if (string_is_empty(d->catalog.base))
      strlcpy(d->catalog.base, d->url, sizeof(d->catalog.base));
   strlcpy(url, d->catalog.base, sizeof(url));

   url_servers_root(root, sizeof(root), url);
   if (!string_is_empty(root) && !url_servers_add(root))
      RARCH_WARN("[URL] Could not save the server %s.\n", root);

   if (!(menu_st->flags & MENU_ST_FLAG_ALIVE))
      retroarch_menu_running();
   if (!menu_st->entries.list
         || !(stack = MENU_LIST_GET(menu_st->entries.list, 0)))
   {
      url_catalog_free(&d->catalog);
      return;
   }
   depth = stack->size + 1; /* the level about to be pushed */

   if (d->catalog.truncated)
      url_task_notify(msg_hash_to_str(MSG_URL_CATALOG_TRUNCATED),
            MESSAGE_QUEUE_CATEGORY_INFO);

   RARCH_LOG("[URL] Game list with %u item(s).\n",
         (unsigned)d->catalog.count);
   if (!url_catalog_level_store(depth, &d->catalog))
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CONTENT_FAILED),
            MESSAGE_QUEUE_CATEGORY_ERROR);
      return;
   }
   if (action_ok_url_catalog_push(url) != 0)
      task_url_catalog_prune(depth - 1);
}
#endif

/* After a login worked: a typed one is saved for the https site it
 * went to (verified certificate, never plain http); a saved one from
 * an older build is moved to that site's key. Nothing if no login was
 * sent (login_site empty). */
static void url_task_keep_login(url_task_data_t *d)
{
   if (string_is_empty(d->login_site))
      return;
   if (d->has_login)
   {
      if (!url_logins_set(d->login_site, d->user, d->pass))
         RARCH_WARN("[URL] Could not save the login for %s.\n",
               d->login_site);
   }
   else if (!url_logins_migrate(d->login_site))
      RARCH_WARN("[URL] Could not update the saved login for %s.\n",
            d->login_site);
}

static void url_task_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   url_task_data_t *d = (url_task_data_t*)task_data;

   url_task_st.busy = false;
   if (!d)
      return;

   switch (d->result)
   {
      case URL_FETCH_OK:
      case URL_FETCH_CACHED:
         /* Saved (or replaced) only once the login has worked. */
         url_task_keep_login(d);
         RARCH_LOG("[URL] %s \"%s\".\n",
               d->result == URL_FETCH_CACHED ? "Cache hit" : "Downloaded",
               d->out_path);
         url_task_load(d->out_path);
         break;
      case URL_FETCH_CATALOG:
         /* A login typed for a list is kept exactly as for a file. */
         url_task_keep_login(d);
#ifdef HAVE_MENU
         url_task_open_catalog(d);
#else
         RARCH_ERR("[URL] %s\n", msg_hash_to_str(MSG_URL_CATALOG_CLI));
#endif
         break;
      case URL_FETCH_NEED_LOGIN:
         /* One prompt per answer from the server: a wrong login comes
          * back here once and asks again, it never retries alone. */
#ifdef HAVE_MENU
         strlcpy(url_task_st.url, d->url, sizeof(url_task_st.url));
         strlcpy(url_task_st.host, d->host, sizeof(url_task_st.host));
         url_task_dialog(MSG_URL_CONTENT_USERNAME_FOR, url_task_user_cb);
#endif
         break;
      case URL_FETCH_CERT_INVALID:
         /* Only ever for a download without a login or a fingerprint
          * (url_fetch_to_cache_ex): the user may take it unchecked,
          * once, or cancel. Never decided here. */
         RARCH_WARN("[URL] %s\n", d->err);
#ifdef HAVE_MENU
         if (!d->anyway && !d->has_login)
         {
            struct menu_state *menu_st = menu_state_get_ptr();
            if (!(menu_st->flags & MENU_ST_FLAG_ALIVE))
               retroarch_menu_running();
            action_ok_url_cert_choice_push(d->url);
         }
#endif
         break;
      default:
         RARCH_ERR("[URL] %s\n", d->err);
         break;
   }
}

static void url_task_cleanup(retro_task_t *task)
{
   url_task_data_t *d = (url_task_data_t*)task->task_data;

   if (!d)
      return;
   task->task_data = NULL;
#ifdef HAVE_THREADS
   /* Not joined yet (should not happen: the handler joins before it
    * finishes): let the thread run on and free d itself. */
   if (d->thread)
   {
      sthread_detach(d->thread);
      d->thread = NULL;
   }
#endif
   url_task_data_release(d);
}

static bool url_task_cache_dir(char *out, size_t len)
{
   settings_t *settings = config_get_ptr();
   const char *base     = settings->paths.directory_core_assets;
   char cfg_dir[PATH_MAX_LENGTH];

   if (string_is_empty(base))
   {
      const char *cfg = path_get(RARCH_PATH_CONFIG);
      if (string_is_empty(cfg))
         return false;
      fill_pathname_basedir(cfg_dir, cfg, sizeof(cfg_dir));
      base = cfg_dir;
   }
   fill_pathname_join_special(out, base, "url_cache", len);
   return true;
}

static bool url_task_push(const char *url,
      const char *user, const char *pass, bool anyway)
{
   char link[URL_TASK_URL_MAX];
   char fixed[URL_TASK_URL_MAX];
   char name[PATH_MAX_LENGTH];
   char title[PATH_MAX_LENGTH];
   const char *shown;
   url_task_data_t *d;
   retro_task_t *task;

   if (string_is_empty(url))
      return false;

   if (url_task_st.busy)
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CONTENT_BUSY),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return false;
   }

   if (!(d = (url_task_data_t*)calloc(1, sizeof(*d))))
      return false;

   if (!url_task_cache_dir(d->cache_dir, sizeof(d->cache_dir)))
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CONTENT_FAILED),
            MESSAGE_QUEUE_CATEGORY_ERROR);
      free(d);
      return false;
   }

   strlcpy(d->url, url, sizeof(d->url));
   d->anyway = anyway;
   /* Same host url_fetch uses for saved logins (the link without its
    * fragment; a broken #sha256= is reported by the fetch). */
   if (!url_content_split_fingerprint(link, sizeof(link), NULL, NULL, url))
      strlcpy(link, url, sizeof(link));
   url_content_fix_link(fixed, sizeof(fixed), link);
   url_content_host(d->host, sizeof(d->host), fixed);
   d->refs      = 1;   /* the task's reference */
   d->pct       = -1;
   d->shown_pct = -1;
   if (!string_is_empty(user))
   {
      d->has_login = true;
      strlcpy(d->user, user, sizeof(d->user));
      if (pass)
         strlcpy(d->pass, pass, sizeof(d->pass));
   }

#ifdef HAVE_THREADS
   d->lock = slock_new();
#endif

   if (!(task = task_init()))
   {
      url_content_wipe(d->pass, sizeof(d->pass));
#ifdef HAVE_THREADS
      slock_free(d->lock);
#endif
      free(d);
      return false;
   }

   /* Shown name: the cache name without its 17-char hash prefix. */
   url_content_cache_name(name, sizeof(name), fixed, NULL);
   shown = (strlen(name) > 17) ? name + 17 : name;
   if (string_is_equal(shown, "download.bin") && !string_is_empty(d->host))
      shown = d->host;
   snprintf(title, sizeof(title),
         msg_hash_to_str(MSG_URL_CONTENT_DOWNLOADING), shown);

   task->handler   = url_task_handler;
   task->callback  = url_task_callback;
   task->cleanup   = url_task_cleanup;
   task->task_data = d;
   task->title     = strdup(title);
   task->progress  = -1;

   url_task_st.busy = true;
   if (!task_queue_push(task))
   {
      /* Not queued, so the queue will not call cleanup. */
      url_task_st.busy = false;
      url_task_cleanup(task);
      free(task->title);
      free(task);
      return false;
   }
   return true;
}

bool task_push_url_content_load(const char *url)
{
   return url_task_push(url, NULL, NULL, false);
}

bool task_push_url_content_load_anyway(const char *url)
{
   return url_task_push(url, NULL, NULL, true);
}

void task_url_content_clear_cache(void)
{
   char cache_dir[PATH_MAX_LENGTH];
   unsigned removed = 0;

   /* A running download may be writing into the folder. */
   if (url_task_st.busy)
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CONTENT_BUSY),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   if (     !url_task_cache_dir(cache_dir, sizeof(cache_dir))
         || !url_fetch_clear_cache(cache_dir, &removed))
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CACHE_CLEAR_FAILED),
            MESSAGE_QUEUE_CATEGORY_ERROR);
      return;
   }
   RARCH_LOG("[URL] Cache cleared: %u file(s) removed.\n", removed);
   url_task_notify(msg_hash_to_str(MSG_URL_CACHE_CLEARED),
         MESSAGE_QUEUE_CATEGORY_INFO);
}

/* ---------- command line ---------- */

/* Logs every 10% (sizes known) so a big download shows it is alive. */
static void url_cli_progress_cb(size_t done, size_t total, void *ud)
{
   int *last = (int*)ud;
   int pct;

   if (!total)
      return;
   if (done >= total)
      pct = 100;
   else
      pct = (int)(((double)done * 100.0) / (double)total);
   pct -= pct % 10;
   if (pct != *last)
   {
      *last = pct;
      RARCH_LOG("[URL] Downloading: %d%% of %u KiB.\n", pct,
            (unsigned)(total / 1024));
   }
}

/* Errors must reach the user even without --verbose. */
static void url_cli_error(const char *msg)
{
   RARCH_ERR("[URL] %s\n", msg);
   if (!verbosity_is_enabled() || is_logging_to_file())
      fprintf(stderr, "%s\n", msg);
}

bool task_url_content_fetch_cli(const char *url,
      char *out_path, size_t len)
{
   char fixed[URL_TASK_URL_MAX];
   char host[URL_TASK_HOST_MAX];
   char site[URL_TASK_HOST_MAX + 16];
   char cache_dir[PATH_MAX_LENGTH];
   char err[512];
   char msg[1024];
   int last_pct = -1;
   enum url_fetch_result res;

   if (out_path && len)
      out_path[0] = '\0';
   if (string_is_empty(url) || !out_path || !len)
      return false;

   {
      char link[URL_TASK_URL_MAX];
      if (!url_content_split_fingerprint(link, sizeof(link), NULL, NULL, url))
         strlcpy(link, url, sizeof(link));
      url_content_fix_link(fixed, sizeof(fixed), link);
   }
   url_content_host(host, sizeof(host), fixed);

   if (!url_task_cache_dir(cache_dir, sizeof(cache_dir)))
   {
      url_cli_error(msg_hash_to_str(MSG_URL_CONTENT_FAILED));
      return false;
   }

   /* Only the host is logged: a link may carry a token or a login. */
   RARCH_LOG("[URL] Fetching from %s into \"%s\".\n", host, cache_dir);
   err[0] = '\0';
   site[0] = '\0';
   res    = url_fetch_to_cache_ex(url, cache_dir, NULL, NULL,
         out_path, len, NULL, 0, err, sizeof(err),
         url_cli_progress_cb, &last_pct, NULL, false, site, sizeof(site));
   /* A saved login from an older build that just worked: moved to its
    * site's key. */
   if (     (res == URL_FETCH_OK || res == URL_FETCH_CATALOG)
         && site[0] && !url_logins_migrate(site))
      RARCH_WARN("[URL] Could not update the saved login for %s.\n", site);

   switch (res)
   {
      case URL_FETCH_OK:
      case URL_FETCH_CACHED:
         RARCH_LOG("[URL] %s \"%s\".\n",
               res == URL_FETCH_CACHED ? "Cache hit" : "Downloaded",
               out_path);
         return true;
      case URL_FETCH_NEED_LOGIN:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_URL_CONTENT_CLI_LOGIN_NEEDED), host);
         url_cli_error(msg);
         break;
      case URL_FETCH_NOT_A_FILE:
         url_cli_error(msg_hash_to_str(MSG_URL_CONTENT_NOT_A_FILE));
         break;
      case URL_FETCH_BLOCKED_PROGRAM:
      case URL_FETCH_BLOCKED_ARCHIVE:
      case URL_FETCH_BLOCKED_TOO_BIG:
      case URL_FETCH_BLOCKED_FINGERPRINT:
         url_cli_error(msg_hash_to_str(url_task_blocked_msg(res)));
         break;
      case URL_FETCH_CERT_INVALID: /* no "Download anyway" here */
         url_cli_error(msg_hash_to_str(MSG_URL_CERT_INVALID));
         break;
      case URL_FETCH_LOGIN_HTTP:
         url_cli_error(msg_hash_to_str(MSG_URL_LOGIN_HTTP));
         break;
      case URL_FETCH_CATALOG: /* game lists need the menu */
         url_cli_error(msg_hash_to_str(MSG_URL_CATALOG_CLI));
         break;
      case URL_FETCH_TOO_BIG:
      case URL_FETCH_FAILED:
      default:
         url_cli_error(string_is_empty(err)
               ? msg_hash_to_str(MSG_URL_CONTENT_FAILED) : err);
         break;
   }
   out_path[0] = '\0';
   return false;
}

/* ---------- archive.org sign-in ---------- */

#ifdef HAVE_MENU

#define URL_ARCHIVE_KEY_MAX 256

typedef struct url_archive_task_data
{
#ifdef HAVE_THREADS
   sthread_t *thread;
   slock_t *lock;
#endif
   bool started;
   bool done;
   bool ok;
   char email[URL_TASK_CRED_MAX];
   char pass[URL_TASK_CRED_MAX];
   char access[URL_ARCHIVE_KEY_MAX];
   char secret[URL_ARCHIVE_KEY_MAX];
   char err[256];
} url_archive_task_data_t;

/* Main thread only: one sign-in at a time; email carries the first
 * dialog's answer to the second. */
static struct
{
   bool busy;
   char email[URL_TASK_CRED_MAX];
} url_archive_st;

/* Called on the sign-in thread (or the task thread without threads). */
static void url_archive_task_run(void *ud)
{
   url_archive_task_data_t *d = (url_archive_task_data_t*)ud;
   bool ok = url_archive_org_login(d->email, d->pass,
         d->access, sizeof(d->access), d->secret, sizeof(d->secret),
         d->err, sizeof(d->err));

   /* Not needed any more: the keys are saved, the password never is. */
   url_content_wipe(d->pass, sizeof(d->pass));
   url_content_wipe(d->access, sizeof(d->access));
   url_content_wipe(d->secret, sizeof(d->secret));

#ifdef HAVE_THREADS
   slock_lock(d->lock);
#endif
   d->ok   = ok;
   d->done = true;
#ifdef HAVE_THREADS
   slock_unlock(d->lock);
#endif
}

static void url_archive_task_handler(retro_task_t *task)
{
   url_archive_task_data_t *d = (url_archive_task_data_t*)task->task_data;
   char msg[512];

   if (!d->started)
   {
      d->started = true;
#ifdef HAVE_THREADS
      if (d->lock)
         d->thread = sthread_create(url_archive_task_run, d);
      if (d->thread)
      {
         task->when = cpu_features_get_time_usec() + URL_TASK_POLL_USEC;
         return;
      }
#endif
      url_archive_task_run(d);
   }

#ifdef HAVE_THREADS
   if (d->thread)
   {
      bool done;
      slock_lock(d->lock);
      done = d->done;
      slock_unlock(d->lock);
      if (!done)
      {
         task->when = cpu_features_get_time_usec() + URL_TASK_POLL_USEC;
         return;
      }
      sthread_join(d->thread);
      d->thread = NULL;
   }
#endif

   if (d->ok)
   {
      RARCH_LOG("[URL] Signed in to archive.org, keys saved.\n");
      url_task_finish_msg(task,
            msg_hash_to_str(MSG_URL_ARCHIVE_SIGNED_IN), false);
   }
   else
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_URL_ARCHIVE_SIGN_IN_FAILED), d->err);
      RARCH_WARN("[URL] %s\n", msg);
      url_task_finish_msg(task, msg, true);
   }

   task->when = 0;
   task_set_progress(task, 100);
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void url_archive_task_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   url_archive_st.busy = false;
}

static void url_archive_task_cleanup(retro_task_t *task)
{
   url_archive_task_data_t *d = (url_archive_task_data_t*)task->task_data;

   if (!d)
      return;
   url_content_wipe(d->pass, sizeof(d->pass));
   url_content_wipe(d->access, sizeof(d->access));
   url_content_wipe(d->secret, sizeof(d->secret));
#ifdef HAVE_THREADS
   slock_free(d->lock);
#endif
   free(d);
   task->task_data = NULL;
}

static bool url_archive_task_push(const char *email, const char *pass)
{
   url_archive_task_data_t *d;
   retro_task_t *task;

   if (string_is_empty(email) || string_is_empty(pass))
      return false;
   if (url_archive_st.busy)
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CONTENT_BUSY),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return false;
   }
   if (!(d = (url_archive_task_data_t*)calloc(1, sizeof(*d))))
      return false;

   strlcpy(d->email, email, sizeof(d->email));
   strlcpy(d->pass, pass, sizeof(d->pass));
#ifdef HAVE_THREADS
   d->lock = slock_new();
#endif

   if (!(task = task_init()))
   {
      url_content_wipe(d->pass, sizeof(d->pass));
#ifdef HAVE_THREADS
      slock_free(d->lock);
#endif
      free(d);
      return false;
   }

   task->handler   = url_archive_task_handler;
   task->callback  = url_archive_task_callback;
   task->cleanup   = url_archive_task_cleanup;
   task->task_data = d;
   task->title     = strdup(msg_hash_to_str(MSG_URL_ARCHIVE_SIGNING_IN));
   task->progress  = -1;

   url_archive_st.busy = true;
   if (!task_queue_push(task))
   {
      url_archive_st.busy = false;
      url_archive_task_cleanup(task);
      free(task->title);
      free(task);
      return false;
   }
   return true;
}

static void url_archive_pass_cb(void *userdata, const char *line)
{
   if (!string_is_empty(line))
   {
      url_archive_task_push(url_archive_st.email, line);
      /* The dialog's own buffer: clear it before the input code frees
       * it. */
      url_content_wipe((char*)line, strlen(line));
   }
   url_content_wipe(url_archive_st.email, sizeof(url_archive_st.email));
   menu_input_dialog_end();
}

static void url_archive_open_pass_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   url_task_dialog_label(msg_hash_to_str(MSG_URL_ARCHIVE_PASSWORD_PROMPT),
         url_archive_pass_cb);
}

static void url_archive_email_cb(void *userdata, const char *line)
{
   if (string_is_empty(line))
   {
      menu_input_dialog_end();
      return;
   }
   strlcpy(url_archive_st.email, line, sizeof(url_archive_st.email));
   menu_input_dialog_end();
   url_task_next_pass(url_archive_open_pass_callback);
}
#endif

void task_url_archive_login_prompt(void)
{
#ifdef HAVE_MENU
   if (url_archive_st.busy)
   {
      url_task_notify(msg_hash_to_str(MSG_URL_CONTENT_BUSY),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   url_task_dialog_label(msg_hash_to_str(MSG_URL_ARCHIVE_EMAIL_PROMPT),
         url_archive_email_cb);
#endif
}

#endif /* HAVE_NETWORKING */
