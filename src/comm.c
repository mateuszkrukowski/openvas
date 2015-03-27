 /* OpenVAS
  * $Id$
  * Description: Communication manager; it manages the NTP Protocol version 1.0 and 1.1.
  *
  * Authors: - Renaud Deraison <deraison@nessus.org> (Original pre-fork develoment)
  *          - Tim Brown <mailto:timb@openvas.org> (Initial fork)
  *          - Laban Mwangi <mailto:labanm@openvas.org> (Renaming work)
  *          - Tarik El-Yassem <mailto:tarik@openvas.org> (Headers section)
  *
  * Copyright:
  * Portions Copyright (C) 2006 Software in the Public Interest, Inc.
  * Based on work Copyright (C) 1998 - 2006 Tenable Network Security, Inc.
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License version 2,
  * as published by the Free Software Foundation
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program; if not, write to the Free Software
  * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
  */

#include <string.h> /* for strchr() */
#include <stdlib.h> /* for atoi() */

#include <stdarg.h>

#include <glib.h>

#include <openvas/nasl/nasl.h>
#include <openvas/misc/nvt_categories.h>/* for ACT_FIRST */
#include <openvas/misc/plugutils.h>
#include <openvas/misc/network.h>       /* for recv_line */
#include <openvas/misc/prefs.h>         /* for preferences_get() */

#include <openvas/base/nvticache.h>     /* for nvticache_t */

#include "comm.h"
#include "ntp.h"
#include "log.h"
#include "pluginscheduler.h"    /* for define LAUNCH_DISABLED */
#include "pluginload.h"    /* for current_loading_plugins */
#include "sighand.h"
#include "utils.h"

extern struct arglist *global_plugins;

/**
 * @brief Initializes the communication between the scanner (us) and the client.
 *
 * @return 0 if success, -1 if error.
 */
int
comm_init (int soc)
{
  char buf[1024];
  int n;

  /* We must read the version of the OTP the client
     wants us to use */
  n = recv_line (soc, buf, sizeof (buf) - 1);
  if (n <= 0)
    {
      log_write ("Failed reading client-requested OTP version.");
      return -1;
    }

  buf[sizeof (buf) - 1] = '\0';
  if (strncmp (buf, "< OTP/2.0 >", 11))
    {
      log_write ("Unknown client-requested OTP version: %s.", buf);
      return -1;
    }
  nsend (soc, "< OTP/2.0 >\n", 12, 0);
  return 0;
}

/**
 * @brief Informs the client that the scanner is still loading.
 *
 * @param[in]   soc Socket to send and receive from.
 *
 * @return 0 if success, -1 if error.
 */
int
comm_loading (int soc)
{
  int n;
  size_t len;
  char buf[256];
  n = recv_line (soc, buf, sizeof (buf) - 1);
  if (n <= 0)
    {
      log_write ("Failed reading client input.");
      return -1;
    }
  /* Always respond with SCANNER_LOADING. */
  g_snprintf (buf, sizeof (buf), "SCANNER_LOADING <|> %d <|> %d\n",
              current_loading_plugins (), total_loading_plugins ());
  len = strlen (buf);
  n = nsend (soc, buf, len, 0);
  if (n != len)
    return -1;
  while (n > 0)
    n = recv_line (soc, buf, sizeof (buf) - 1);

  return 0;
}

/**
 * Determines if the client is still connected.
 * @return 1 if the client is here, 0 if it's not.
 */
static int
is_client_present (int soc)
{
  fd_set rd;
  struct timeval tv;
  int m, e;

  stream_zero (&rd);
  m = stream_set (soc, &rd);
again:
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  errno = 0;
  e = select (m + 1, &rd, NULL, NULL, &tv);
  if (e < 0)
    {
      if (errno == EINTR)
        goto again;
      return 0;
    }

  if (e > 0 && !data_left (openvas_get_socket_from_connection (soc)))
    return 0;
  return 1;
}

/**
 * @brief This function must be called at the end of a session.
 */
void
comm_terminate (int soc)
{
  send_printf (soc, "SERVER <|> BYE <|> BYE <|> SERVER\n");
  while (is_client_present (soc))
    {
      char buffer[4096];
      int n;

      n = recv_line (soc, buffer, sizeof (buffer) - 1);
      if (n < 0 || *buffer == '\0')
        return;
    }
}

/**
 * @brief Sends a plugin info.
 */
void
send_plug_info (int soc, struct arglist *plugins)
{
  int j, ignored = 0;
  static const char *categories[] = { ACT_STRING_LIST_ALL };
#define CAT_MAX	(sizeof(categories) / sizeof(categories[0]))
  const char *name, *copyright, *summary, *version, *family;
  char *str;
  nvti_t *nvti = nvticache_get_by_oid_full (plugins->name);

  if (!nvti)
    {
      log_write ("NVTI not found for OID %s. Will not be sent.", plugins->name);
      return;
    }

  j = nvti_category (nvti);
  if (j >= CAT_MAX || j < ACT_FIRST)
    j = CAT_MAX - 1;

  version = nvti_version (nvti);
  if (!version)
    version = "?";

  if ((name = nvti_name (nvti)) == NULL)
    {
      log_write ("Inconsistent data (no name): %s - not applying this plugin",
                 nvti_oid (nvti));
      ignored = 1;
    }

  if ((copyright = nvti_copyright (nvti)) == NULL)
    {
      log_write
        ("Inconsistent data (no copyright): %s - not applying this plugin",
         name ? name : nvti_oid (nvti));
      ignored = 1;
    }

  if (nvti_tag (nvti) && strstr (nvti_tag (nvti), "summary="))
    summary = "NOSUMMARY";
  else
    summary = nvti_summary (nvti);
  if (summary == NULL)
    {
      log_write
        ("Inconsistent data (no summary): %s - not applying this plugin",
         name ? name : nvti_oid (nvti));
      ignored = 1;
    }

  if ((family = nvti_family (nvti)) == NULL)
    {
      log_write
        ("Inconsistent data (no family): %s - not applying this plugin",
         name ? name : nvti_oid (nvti));
      ignored = 1;
    }


  if (strchr (name, '\n') != NULL)
    {
      log_write ("%s: Newline in name\n", nvti_oid (nvti));
      ignored = 1;
    }

  if (strchr (copyright, '\n') != NULL)
    {
      log_write ("%s: Newline in copyright\n", nvti_oid (nvti));
      ignored = 1;
    }

  if (strchr (summary, '\n'))
    {
      log_write ("%s: Newline in summary\n", nvti_oid (nvti));
      ignored = 1;
    }

  if (!ignored)
    {
      char *cve_id, *bid, *xref, *tag;

      cve_id = nvti_cve (nvti);
      if (cve_id == NULL || strcmp (cve_id, "") == 0)
        cve_id = "NOCVE";

      bid = nvti_bid (nvti);
      if (bid == NULL || strcmp (bid, "") == 0)
        bid = "NOBID";

      xref = nvti_xref (nvti);
      if (xref == NULL || strcmp (xref, "") == 0)
        xref = "NOXREF";

      {
        char *index;
        tag = g_strdup (nvti_tag (nvti));
        index = tag;
        if (tag == NULL || strcmp (tag, "") == 0)
          tag = "NOTAG";
        else
          while (*index)
            {
              if (*index == '\n')
                *index = ';';
              index++;
            }
      }

      str = g_strdup_printf ("%s <|> %s <|> %s <|> "
                             "%s <|> %s <|> %s <|> "
                             "%s <|> %s <|> %s <|> %s <|> %s",
                             nvti_oid (nvti), name, categories[j],
                             copyright, summary, family,
                             version, cve_id, bid, xref, tag);
      if (tag != NULL && strcmp (tag, "NOTAG"))
        g_free (tag);
      send_printf (soc, "%s\n", str);
      g_free (str);
    }

  nvti_free (nvti);
}

/**
 * @brief Sends the plugin info for a single plugin.
 * @param soc Socket to use to send plugin info.
 * @param oid OID of the plugin to send.
 * @see send_plug_info
 */
void
plugin_send_infos (int soc, char *oid)
{
  struct arglist *plugins = global_plugins;

  if (!oid)
    return;
  if (!plugins)
    return;

  while (plugins)
    {
      struct arglist *args = plugins->value;
      if (args && !strcmp (oid, plugins->name))
        {
          send_plug_info (soc, plugins);
          return;
        }
      plugins = plugins->next;
    }
}


/**
 * @brief Sends the list of plugins that the scanner could load to the client,
 * @brief using the OTP format (calls send_plug_info for each).
 * @param socket    Socket to use for sending list of plugins.
 * @see send_plug_info
 */
void
comm_send_pluginlist (int soc)
{
  struct arglist *plugins = global_plugins;

  send_printf (soc, "SERVER <|> PLUGIN_LIST <|>\n");
  while (plugins && plugins->next)
    {
      send_plug_info (soc, plugins);
      plugins = plugins->next;
    }
  send_printf (soc, "<|> SERVER\n");
}

/**
 * @brief Sends the preferences of the scanner.
 * @param soc Socket to use for sending.
 */
void
comm_send_preferences (int soc)
{
  struct arglist *prefs = preferences_get ();

  /* We have to be backward compatible with the NTP/1.0 */
  send_printf (soc, "SERVER <|> PREFERENCES <|>\n");

  while (prefs && prefs->next)
    {
      if (prefs->type == ARG_STRING && !is_scanner_only_pref (prefs->name))
        send_printf (soc, "%s <|> %s\n", prefs->name,
                     (const char *) prefs->value);
      prefs = prefs->next;
    }
  send_printf (soc, "<|> SERVER\n");
}


/**
 * @brief This function waits for the attack order of the client.
 * Meanwhile, it processes all the messages the client could send.
 */
int
comm_wait_order (struct arglist *globals)
{
  int soc = arg_get_value_int (globals, "global_socket");

  for (;;)
    {
      static char str[2048];
      int n;

      n = recv_line (soc, str, sizeof (str) - 1);
      if (n < 0)
        {
          log_write ("Client closed the communication");
          return -1;
        }
      if (str[0] == '\0')
        if (!is_client_present (soc))
          {
            log_write ("Client not present");
            return -1;
          }

      n = ntp_parse_input (globals, str);
      if (n == 0)
        return 0;
      else if (n == -1)
        {
          log_write ("Client input parsing error: %s", str);
          return -1;
        }
    }
}

/*-------------------------------------------------------------------------------*/

/** @TODO Consolidate sorting mechanisms spread over the openvas project.
 *        Rename function (qsort_oid?). */

/**
 * Q-Sort comparison function.
 * @param a An arglist** to compare against b.
 * @param b An arglist** to compare against a.
 */
static int
qsort_cmp (const void *a, const void *b)
{
  struct arglist **plugin_a = (struct arglist **) a;
  struct arglist **plugin_b = (struct arglist **) b;

  return (strcmp ((*plugin_a)->name, (*plugin_b)->name));
}

/**
 * Retrieves a plugin defined by its OID from a range within a sorted plugin
 * array.
 * Recursively defined, uses divide and conquer approach.
 */
static struct arglist *
_get_plug_by_oid (struct arglist **array, char *oid, int start, int end,
                  int rend)
{
  int mid;
  char *plugin_oid;

  if (start >= rend)
    return NULL;

  if (start == end)
    {
      plugin_oid = array[start]->name;
      if (strcmp (plugin_oid, oid) == 0)
        return array[start];
      else
        return NULL;
    }

  mid = (start + end) / 2;
  plugin_oid = array[mid]->name;
  if (strcmp (plugin_oid, oid) > 0)
    return _get_plug_by_oid (array, oid, start, mid, rend);
  else if (strcmp (plugin_oid, oid) < 0)
    return _get_plug_by_oid (array, oid, mid + 1, end, rend);

  return array[mid];
}

/**
 * @brief Retrieves a plugin defined by its OID from a plugin arrray.
 */
static struct arglist *
get_plug_by_oid (struct arglist **array, char *oid, int num_plugins)
{
  return _get_plug_by_oid (array, oid, 0, num_plugins, num_plugins);
}

/*-------------------------------------------------------------------------------*/


/**
 * Enable the plugins which have been selected by the user, or all if
 * list == NULL or list == "-1;";
 * @param list A user (client) defined semicolon delimited list, of plugin(oids)
 *             that shall be enabled. If NULL or "-1;" all plugins are enabled!
 */
void
comm_setup_plugins (char *list)
{
  int num_plugins = 0;
  struct arglist *plugins = global_plugins;
  struct arglist *p = plugins;
  struct arglist **array;
  char *t;
  char *oid;
  int i;
  int enable = LAUNCH_DISABLED;

  if (p == NULL)
    return;
  if (list == NULL)
    list = "-1;";

  if (atoi (list) == -1)
    enable = LAUNCH_RUN;
  /* Disable every plugin */
  while (p->next != NULL)
    {
      num_plugins++;
      plug_set_launch (p->value, enable);
      p = p->next;
    }

  if (num_plugins == 0 || enable != 0)
    return;

  /* Store the plugins in an array for quick access */
  p = plugins;
  i = 0;
  array = g_malloc0 (num_plugins * sizeof (struct arglist **));
  while (p->next != NULL)
    {
      array[i++] = p;
      p = p->next;
    }

  qsort (array, num_plugins, sizeof (struct arglist *), qsort_cmp);

  t = list;
  oid = strtok (t, ";");

  /* Read the list provided by the user and enable the plugins accordingly */
  while (oid != NULL)
    {
      p = get_plug_by_oid (array, oid, num_plugins);
      if (p != NULL)
        plug_set_launch (p->value, LAUNCH_RUN);
#ifdef DEBUG
      else
        log_write ("PLUGIN ID %s NOT FOUND!!!", oid);
#endif
      oid = strtok (NULL, ";");
    }

  g_free (array);
}


/**
 * @brief Determine the version of the NVT feed.
 * @param[out] feed_version Buffer to contain feed_version.
 * @param[in]  feed_size    Size of feed_version buffer.
 *
 * @return Feed version. Free on caller.
 */
static int
nvt_feed_version (char *feed_version, int feed_size)
{
  FILE *foutput;
  gchar *command, *info_file;
  info_file = g_build_filename (OPENVAS_NVT_DIR, "plugin_feed_info.inc", NULL);
  command = g_strdup_printf ("grep PLUGIN_SET %s | sed -e 's/[^0-9]//g'",
                             info_file);

  foutput = popen (command, "r");
  if (fgets (feed_version, feed_size, foutput) == NULL)
    {
      pclose (foutput);
      g_free (info_file);
      g_free (command);
      return 1;
    }

  feed_version[strlen (feed_version) - 1] = '\0';
  pclose (foutput);
  g_free (info_file);
  g_free (command);
  return 0;
}

/**
 * @brief Determine whether a buffer contains a valid feed version.
 *
 * @param[in] feed_version Buffer containing feed_version.
 *
 * @return 1 is valid feed_version, 0 otherwise.
 */
static int
is_valid_feed_version (const char *feed_version)
{
  if (feed_version == NULL)
    return 0;

  while (*feed_version)
    if (!g_ascii_isdigit (*feed_version++))
      return 0;
  return 1;
}

/**
 * @brief Send the OTP NVT_INFO message and then handle any COMPLETE_LIST
 * and PLUGIN_INFO commands.
 */
void
comm_send_nvt_info (int soc)
{
  char buf[2048];
  gchar *feed_version;
  int feed_size = 32;

  feed_version = g_malloc0 (feed_size);
  nvt_feed_version (feed_version, feed_size);

  send_printf (soc, "SERVER <|> NVT_INFO <|> %s <|> SERVER\n",
               is_valid_feed_version (feed_version)
                ? feed_version : "NOVERSION");

  g_free (feed_version);

  for (;;)
    {
      bzero (buf, sizeof (buf));
      recv_line (soc, buf, sizeof (buf) - 1);
      if (strstr (buf, "COMPLETE_LIST"))
        comm_send_pluginlist (soc);
      else if (strstr (buf, "PLUGIN_INFO"))
        {
          char *t = strstr (buf, " <|> ");
          char *s;
          if (!t)
            continue;
          t = strstr (t + 5, " <|> ");
          if (!t)
            continue;
          s = t + 5;
          t = strchr (s, ' ');
          if (!t)
            continue;
          t[0] = '\0';
          plugin_send_infos (soc, s);
        }
      else
        break;
    }
}
