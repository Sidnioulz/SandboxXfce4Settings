/*
 *  Copyright (c) 2016 Steve Dodier-Lazaro <sidi@xfce.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_MATH_H
#include <math.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <sys/types.h>
#include <signal.h>
#include <glib.h>
#include <xfconf/xfconf.h>
#include <garcon/garcon.h>
#include <libxfce4ui/libxfce4ui.h>

#include "debug.h"
#include "firejail-sandboxes.h"

/* No time to waste with a broken build chain */
#ifndef XFCE_FIREJAIL_ENABLE_NETWORK_KEY
#define XFCE_FIREJAIL_ENABLE_NETWORK_KEY     "X-XfceFirejailEnableNetwork"
#endif
#ifndef XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_KEY
#define XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_KEY "X-XfceFirejailBandwidthDownload"
#endif
#ifndef XFCE_FIREJAIL_BANDWIDTH_UPLOAD_KEY
#define XFCE_FIREJAIL_BANDWIDTH_UPLOAD_KEY   "X-XfceFirejailBandwidthUpload"
#endif
#ifndef XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_DEFAULT
#define XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_DEFAULT 200000
#endif
#ifndef XFCE_FIREJAIL_BANDWIDTH_UPLOAD_DEFAULT
#define XFCE_FIREJAIL_BANDWIDTH_UPLOAD_DEFAULT   200000
#endif

/* Type of Xfce sandbox being treated (guessed from environment) */
typedef enum {
    XFCE_SANDBOX_UNKNOWN = 0,
    XFCE_SANDBOX_DESKTOP = 1,
    XFCE_SANDBOX_WORKSPACE = 2
} XfceSandboxType;

typedef struct _XfceSandboxProcess {
    pid_t            pid;
    gchar           *name;
    XfceSandboxType  type;
    guint            ws_number;
    gchar           *desktop_path;
} XfceSandboxProcess;



static void                 xfce_sandbox_poller_dispose                   (GObject                *object);
static void                 xfce_sandbox_poller_finalize                  (GObject                *object);
static gboolean             xfce_sandbox_poller_initial_load              (XfceSandboxPoller      *poller,
                                                                           GError                **error);
static gboolean             xfce_sandbox_poller_add_entry_from_pid        (XfceSandboxPoller      *poller,
                                                                           pid_t                   pid,
                                                                           gboolean               *retry);
static gboolean             xfce_sandbox_poller_add_entry                 (XfceSandboxPoller      *poller,
                                                                           const gchar            *name,
                                                                           gboolean               *retry);
static gboolean             xfce_sandbox_poller_remove_entry_from_pid     (XfceSandboxPoller      *poller,
                                                                           pid_t                   pid);
static gboolean             xfce_sandbox_poller_remove_entry              (XfceSandboxPoller      *poller,
                                                                           const gchar            *name);
static void                 xfce_sandbox_poller_on_file_changed           (GFileMonitor           *monitor,
                                                                           GFile                  *file,
                                                                           GFile                  *other_file,
                                                                           GFileMonitorEvent       event_type,
                                                                           gpointer                user_data);
static void                 xfce_sandbox_poller_on_desktop_changed        (GFileMonitor           *monitor,
                                                                           GFile                  *file,
                                                                           GFile                  *other_file,
                                                                           GFileMonitorEvent       event_type,
                                                                           gpointer                user_data);
static void                 xfce_sandbox_poller_reload                    (XfceSandboxPoller      *poller);
static void                 xfce_sandbox_poller_unload                    (XfceSandboxPoller      *poller);
static void                 xfce_sandbox_poller_channel_property_changed  (XfconfChannel          *channel,
                                                                           const gchar            *property_name,
                                                                           const GValue           *value,
                                                                           XfceSandboxPoller      *helper);

static XfceSandboxProcess*  xfce_sandbox_process_new                      (pid_t                   pid,
                                                                           const gchar            *name,
                                                                           XfceSandboxType         type);
static void                 xfce_sandbox_process_free                     (XfceSandboxProcess     *proc);
static gboolean             xfce_sandbox_process_apply_workspace_prop     (XfceSandboxProcess     *proc,
                                                                           const gchar            *property_name);
static gboolean             xfce_sandbox_process_apply_desktop_props      (XfceSandboxProcess     *proc);
static gboolean             xfce_sandbox_process_start_watching           (XfceSandboxProcess     *proc);



struct _XfceSandboxPollerClass
{
    GObjectClass __parent__;
};

struct _XfceSandboxPoller
{
    GObject  __parent__;

    const gchar        *rundir_path;
    GHashTable         *sandboxes;
    XfconfChannel      *channel;
    GFileMonitor       *monitor;
    GFileMonitor       *app_monitor;
    GFileMonitor       *local_app_monitor;
    GFileMonitor       *home_app_monitor;
    guint               handler;
};


G_DEFINE_TYPE (XfceSandboxPoller, xfce_sandbox_poller, G_TYPE_OBJECT);



static void
xfce_sandbox_poller_class_init (XfceSandboxPollerClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->dispose = xfce_sandbox_poller_dispose;
    gobject_class->finalize = xfce_sandbox_poller_finalize;
}



static void
xfce_sandbox_poller_init (XfceSandboxPoller *poller)
{
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Initialisation of sandbox poller starting.");

    poller->rundir_path = EXECHELP_RUN_DIR;
    poller->sandboxes   = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) xfce_sandbox_process_free);
    poller->channel = xfconf_channel_get ("xfwm4");

    /* monitor channel changes */
    poller->handler = g_signal_connect (G_OBJECT (poller->channel),
                                        "property-changed",
                                        G_CALLBACK (xfce_sandbox_poller_channel_property_changed),
                                        poller);

    /* load all settings */
    xfce_sandbox_poller_reload (poller);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Finished initialising the sandbox poller.");
}



static void
xfce_sandbox_poller_dispose (GObject *object)
{
    XfceSandboxPoller *poller = XFCE_SANDBOX_POLLER (object);

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Disposing of the sandbox poller.");
    if (poller->handler > 0)
    {
        g_signal_handler_disconnect (G_OBJECT (poller->channel), poller->handler);
        poller->handler = 0;
    }

    g_signal_handlers_disconnect_by_func (poller->monitor, xfce_sandbox_poller_on_file_changed, poller);
    g_signal_handlers_disconnect_by_func (poller->app_monitor, xfce_sandbox_poller_on_desktop_changed, poller);
    g_signal_handlers_disconnect_by_func (poller->local_app_monitor, xfce_sandbox_poller_on_desktop_changed, poller);
    g_signal_handlers_disconnect_by_func (poller->home_app_monitor, xfce_sandbox_poller_on_desktop_changed, poller);

    xfce_sandbox_poller_unload (poller);

    (*G_OBJECT_CLASS (xfce_sandbox_poller_parent_class)->dispose) (object);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Finished disposing of the sandbox poller.");
}



static void
xfce_sandbox_poller_finalize (GObject *object)
{
    XfceSandboxPoller *poller = XFCE_SANDBOX_POLLER (object);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Finalizing the sandbox poller.");

    g_hash_table_destroy (poller->sandboxes);
    g_object_unref (poller->channel);
    
    g_object_unref (poller->monitor);
    g_object_unref (poller->app_monitor);
    g_object_unref (poller->local_app_monitor);
    g_object_unref (poller->home_app_monitor);

    g_hash_table_destroy (poller->sandboxes);

    (*G_OBJECT_CLASS (xfce_sandbox_poller_parent_class)->finalize) (object);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Finished finalizing the sandbox poller.");
}



typedef struct _XfceSandboxPropChangedData {
    XfceSandboxPoller  *poller;
    guint               ws_number;
    const gchar        *property_name;
    XfceSandboxProcess *found;
} XfceSandboxPropChangedData;



static void
xfce_sandbox_poller_channel_property_update (gpointer key,
                                             gpointer value,
                                             gpointer user_data)
{
    XfceSandboxPropChangedData *data    = (XfceSandboxPropChangedData*) user_data;
    XfceSandboxProcess         *proc    = (XfceSandboxProcess*) value;

    if (data->found)
      return;

    if (proc->type == XFCE_SANDBOX_WORKSPACE && data->ws_number == proc->ws_number)
      data->found = proc;
}
           
           

static void
xfce_sandbox_poller_channel_property_changed (XfconfChannel     *channel,
                                              const gchar       *property_name,
                                              const GValue      *value,
                                              XfceSandboxPoller *poller)
{
    XfceSandboxPropChangedData *data;
    gint                        ws_number;

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "An Xfconf property ('%s') has changed.", property_name);
    g_return_if_fail (XFCE_IS_SANDBOX_POLLER (poller));

    if (g_str_has_prefix (property_name, "/security/workspace_"))
    {
      gchar *end = NULL;
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "The changed Xfconf property ('%s') is related to Workspace security settings.", property_name);

      ws_number = g_ascii_strtoll (property_name + 20, &end, 10);
      if (!end || *end != '/')
      {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Malformed Xfconf property, cannot be processed (the settings editor expected something of the form \"/security/workspace_<id>/<property>\").");
        g_warning ("Malformed Xfconf property, cannot be processed (the settings editor expected something of the form \"/security/workspace_<id>/<property>\").\n");
        return;
      }
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "The changed Xfconf property ('%s') is related to Workspace security settings for workspace %d.", property_name, ws_number);

      data = g_malloc (sizeof (XfceSandboxPropChangedData));
      data->poller = poller;
      data->ws_number = ws_number;
      data->property_name = property_name;
      data->found = NULL;

      /* find a sandbox for the firejail domain whose property changed */
      g_hash_table_foreach (poller->sandboxes, xfce_sandbox_poller_channel_property_update, data);

      /* if a sandbox instance was found, apply the change */
      if (data->found)
      {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Found a running sandbox instance to which this Xfconf property applies: %d. Applying the property now.", data->found->pid);
        xfce_sandbox_process_apply_workspace_prop (data->found, data->property_name);
      }
      else
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Found no running sandbox instance to which this Xfconf property applies, doing nothing.");

      g_free (data);
    }
}



static void
read_sandbox_env (const gchar *env_path, pid_t client_pid, XfceSandboxType *type, gchar **name)
{
    FILE            *fp;
    char            *buffer = NULL;
    char            *value;
    size_t           n = 0;
    ssize_t          linelen = 0;

    g_return_if_fail (type && name);
    *type = XFCE_SANDBOX_UNKNOWN;
    *name = NULL;

    fp = fopen(env_path, "rb");
    if (!fp)
    {
      g_warning ("Cannot open environment file '%s' to find out the sandbox type of process %d: %s", env_path, client_pid, strerror (errno));
      return;
    }

    buffer = NULL;
    n = 0;
    linelen = 0;
    errno = 0;
    while ((linelen = getline(&buffer, &n, fp)) != -1)
    {
      if (buffer[linelen-1] == '\n')
        buffer[linelen-1] = '\0';

      value = strchr(buffer, '=');
      if (value)
      {
        *value = 0;
        value += 1;

        /* always apply the workspace type if found */
//          xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Sandbox %d: %s = %s\n", client_pid, buffer, value);
        if (g_strcmp0 (buffer, "FIREJAIL_SANDBOX_WORKSPACE") == 0)
        {
//          xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Sandbox %d is a workspace sandbox: %s\n", client_pid, value);
          *type = XFCE_SANDBOX_WORKSPACE;
        }
        else if (g_strcmp0 (buffer, "FIREJAIL_SANDBOX_NAME") == 0)
        {
          *name = g_strdup (value);

          /* only apply this one in absence of other types */
          if (*type == XFCE_SANDBOX_UNKNOWN)
          {
//            xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Sandbox %d is a desktop? sandbox: %s\n", client_pid, value);
            *type = XFCE_SANDBOX_DESKTOP;
          }
        }
      }

      free(buffer);
      buffer = NULL;
      n = 0;
    }

    fclose(fp);

//    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Sandbox %d is a sandbox of name %s and type %d\n", client_pid, *name, *type);
    return;
}



typedef struct _XfceSandboxAddEntryIdleData {
    XfceSandboxPoller  *poller;
    gchar              *name;
    gint                try_counter;
} XfceSandboxAddEntryIdleData;




static gboolean
xfce_sandbox_poller_add_entry_idle (gpointer user_data)
{
    XfceSandboxAddEntryIdleData *data = (XfceSandboxAddEntryIdleData *) user_data;
    gboolean                     retry = FALSE;
    gboolean                     overran = FALSE;

    //FIXME possible inconsistencies, must keep guint of handler somewhere for proper cleanup
    g_return_val_if_fail (data, FALSE);
    g_return_val_if_fail (XFCE_IS_SANDBOX_POLLER (data->poller), FALSE);

    if (data->try_counter++ == 10)
      overran = TRUE;
    else
      xfce_sandbox_poller_add_entry (data->poller, data->name, &retry);

    if (!retry || overran)
    {
      g_free (data->name);
      g_free (data);
      return FALSE;
    }
    else
      return TRUE;
}



static gboolean
xfce_sandbox_poller_add_entry_from_pid (XfceSandboxPoller *poller,
                                        pid_t              pid,
                                        gboolean          *retry)
{
    XfceSandboxProcess *proc;
    XfceSandboxType     type;
    gchar              *path;
    gchar              *sandbox_name;

    g_return_val_if_fail (XFCE_IS_SANDBOX_POLLER (poller), FALSE);

    if (kill (pid, 0))
    {
      if (errno == ESRCH)
      {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Sandbox directory %d corresponds to an app that already exited, ignoring.", pid);
        return FALSE;
      }
    }

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Adding sandbox '%d'.", pid);
    /* ignore entries we already have */
    if (g_hash_table_contains (poller->sandboxes, GUINT_TO_POINTER (pid)))
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Sandbox '%d' already added, ignoring.", pid);
      return FALSE;
    }

    /* ignore entries that don't seem to have the environment file we need */
    path = g_strdup_printf ("%s/%d/%s", poller->rundir_path, pid, DOMAIN_ENV_FILE);
    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Missing environment file at '%s' for sandbox '%d', cannot manage its settings.", path, pid);
      g_warning ("Missing environment file at '%s' for sandbox '%d', cannot manage its settings.\n", path, pid);

      g_free (path);

      /* This file is not immediately available on sandbox startup, give it a second and retry */
      if (retry)
        *retry = TRUE;

      return FALSE;
    }

    read_sandbox_env (path, pid, &type, &sandbox_name);
    if (type == XFCE_SANDBOX_UNKNOWN)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Could not determine type of sandbox for sandbox '%d', cannot manage its settings.", pid);
      g_warning ("Could not determine type of sandbox for sandbox '%d', cannot manage its settings.\n", pid);

      /* This file is not immediately available on sandbox startup, give it a second and retry */
      if (retry)
        *retry = TRUE;

      g_free (path);
      return FALSE;
    }

    /* create a process struct and add it to our list of monitored processes */
    proc = xfce_sandbox_process_new (pid, sandbox_name, type);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Adding proc object for sandbox '%d', named '%s' and of type %s.", pid, sandbox_name, type == XFCE_SANDBOX_WORKSPACE? "Workspace":"Desktop app");
    g_free (sandbox_name);

    if (xfce_sandbox_process_start_watching (proc))
      g_hash_table_insert (poller->sandboxes, GUINT_TO_POINTER (pid), proc);
    else
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Proc object for sandbox '%d' cannot be watched, not adding proc object to list of managed sandboxes.", pid);

    g_free (path);
    return TRUE;
}



static gboolean
xfce_sandbox_poller_add_entry (XfceSandboxPoller *poller,
                               const gchar       *name,
                               gboolean          *retry)
{
    pid_t               pid;

    g_return_val_if_fail (XFCE_IS_SANDBOX_POLLER (poller), FALSE);
    g_return_val_if_fail (name != NULL, FALSE);

    /* ignore the "self" entry */
    if (g_strcmp0 ("self", name) == 0)
      return FALSE;

    pid = g_ascii_strtoll (name, NULL, 10);
    if (!pid)
      return FALSE;
    else
      return xfce_sandbox_poller_add_entry_from_pid (poller, pid, retry);
}



static gboolean
xfce_sandbox_poller_remove_entry_from_pid (XfceSandboxPoller *poller,
                                           pid_t              pid)
{
    g_return_val_if_fail (XFCE_IS_SANDBOX_POLLER (poller), FALSE);

    /* ignore entries we already have */
    if (g_hash_table_contains (poller->sandboxes, GUINT_TO_POINTER (pid)))
      return g_hash_table_remove (poller->sandboxes, GUINT_TO_POINTER (pid));

    return FALSE;
}



static gboolean
xfce_sandbox_poller_remove_entry (XfceSandboxPoller *poller,
                                  const gchar       *name)
{
    pid_t               pid;

    g_return_val_if_fail (XFCE_IS_SANDBOX_POLLER (poller), FALSE);
    g_return_val_if_fail (name != NULL, FALSE);

    /* ignore the "self" entry */
    if (g_strcmp0 ("self", name) == 0)
      return FALSE;

    pid = g_ascii_strtoll (name, NULL, 10);
    if (!pid)
      return FALSE;
    else
      return xfce_sandbox_poller_remove_entry_from_pid (poller, pid);
}



static gboolean
xfce_sandbox_poller_initial_load (XfceSandboxPoller *poller, GError **error)
{
    GDir        *dir       = NULL;
    gboolean     succeeded;
    const gchar *next;

    g_return_val_if_fail (XFCE_IS_SANDBOX_POLLER (poller), FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    dir = g_dir_open (poller->rundir_path, 0, error);
    if (error && *error)
    {
      g_prefix_error (error, "Failed to open runtime directory: ");
      return FALSE;
    }

#if HAVE_ERRNO_H
    errno = 0;
#endif
    succeeded = TRUE;
    while ((next = g_dir_read_name (dir)) != NULL)
    {
      if (!xfce_sandbox_poller_add_entry (poller, next, NULL) && g_ascii_strtoll (next, NULL, 10))
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to add entry %s", next);

#if HAVE_ERRNO_H
      errno = 0;
#endif
    }

#if HAVE_ERRNO_H
    if (errno != 0)
    {
      if (error)
        *error = g_error_new (G_IO_ERROR, g_io_error_from_errno (errno), "Failed to read a runtime directory entry: ");
      succeeded = FALSE;
    }
#endif

    g_dir_close (dir);

    return succeeded;
}



static void
xfce_sandbox_poller_on_file_changed (GFileMonitor     *monitor,
                                     GFile            *file,
                                     GFile            *other_file,
                                     GFileMonitorEvent event_type,
                                     gpointer          user_data)
{
    XfceSandboxPoller *poller = (XfceSandboxPoller *) user_data;
    gchar             *filename;

    g_return_if_fail (XFCE_IS_SANDBOX_POLLER (poller));

    filename = g_file_get_basename (file);

    if (event_type == G_FILE_MONITOR_EVENT_CREATED)
    {
        XfceSandboxAddEntryIdleData *data = g_malloc (sizeof (XfceSandboxAddEntryIdleData));

        data->poller = poller;
        data->name = g_strdup (filename);
        data->try_counter = 0;

        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' created, adding it in a few moments", filename);
        g_timeout_add_seconds (1, xfce_sandbox_poller_add_entry_idle, data);
    }
    else if (event_type == G_FILE_MONITOR_EVENT_DELETED)
    {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' deleted", filename);
        xfce_sandbox_poller_remove_entry (poller, filename);
    }
    else if (event_type == G_FILE_MONITOR_EVENT_PRE_UNMOUNT)
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' about to be unmounted", filename);
    else if (event_type == G_FILE_MONITOR_EVENT_UNMOUNTED)
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' unmounted", filename);
    else if (event_type == G_FILE_MONITOR_EVENT_MOVED)
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' moved", filename);
    else if (event_type == G_FILE_MONITOR_EVENT_MOVED_IN)
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' moved in", filename);
    else if (event_type == G_FILE_MONITOR_EVENT_MOVED_OUT)
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' moved out", filename);
    else if (event_type == G_FILE_MONITOR_EVENT_RENAMED)
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "File '%s' renamed", filename);

    g_free (filename);
}



static void
xfce_sandbox_poller_reload (XfceSandboxPoller *poller)
{
    GError *error = NULL;
    GFile  *file;
    gchar  *home_path;

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Reloading all sandboxes.");
    g_return_if_fail (XFCE_IS_SANDBOX_POLLER (poller));

    /* clean up existing state first */
    if (poller->monitor)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... suspending existing file monitor");
      g_object_unref (poller->monitor);
      poller->monitor = NULL;
    }
    if (poller->app_monitor)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... suspending existing AppInfo monitor");
      g_object_unref (poller->app_monitor);
      poller->app_monitor = NULL;
    }
    if (poller->local_app_monitor)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... suspending existing local AppInfo monitor");
      g_object_unref (poller->local_app_monitor);
      poller->local_app_monitor = NULL;
    }

    if (g_hash_table_size (poller->sandboxes) != 0)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... removing existing sandboxes from hash table.");
      g_hash_table_remove_all (poller->sandboxes);
    }

    /* load existing sandboxes' parameters */
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... applying settings for existing sandboxes.");
    xfce_sandbox_poller_initial_load (poller, &error);
    if (error)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to load and apply settings for existing Firejail sandboxes. Reason: %s", error->message);
      g_warning ("Failed to load and apply settings for existing Firejail sandboxes. Reason: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }

    /* watch for future sandboxes */
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... loading new file monitor.");
    file = g_file_new_for_path (poller->rundir_path);
    poller->monitor = g_file_monitor_directory (file, G_FILE_MONITOR_WATCH_MOUNTS, NULL, &error);
    if (error)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to load file monitor for Firejail's runtime directory. Will not automatically apply settings to future sandboxes. Reason: %s", error->message);
      g_warning ("Failed to load file monitor for Firejail's runtime directory. Will not automatically apply settings to future sandboxes. Reason: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
    else
    {
      g_signal_connect (G_OBJECT (poller->monitor), "changed", G_CALLBACK (xfce_sandbox_poller_on_file_changed), poller);
    }

    /* watch for changes to sandboxed Desktop apps */
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... loading new AppInfo monitor.");
    file = g_file_new_for_path ("/usr/share/applications");
    poller->app_monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, &error);
    if (error)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to load AppInfo monitor. May not automatically update settings to Desktop app sandboxes. Reason: %s", error->message);
      g_warning ("Failed to load AppInfo monitor. May not automatically update settings to Desktop app sandboxes. Reason: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
    else
    {
      g_signal_connect (G_OBJECT (poller->app_monitor), "changed", G_CALLBACK (xfce_sandbox_poller_on_desktop_changed), poller);
    }

    /* watch for changes to sandboxed Desktop apps */
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... loading new AppInfo monitor.");
    file = g_file_new_for_path ("/usr/local/share/applications");
    poller->local_app_monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, &error);
    if (error)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to load AppInfo monitor. May not automatically update settings to Desktop app sandboxes. Reason: %s", error->message);
      g_warning ("Failed to load AppInfo monitor. May not automatically update settings to Desktop app sandboxes. Reason: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
    else
    {
      g_signal_connect (G_OBJECT (poller->local_app_monitor), "changed", G_CALLBACK (xfce_sandbox_poller_on_desktop_changed), poller);
    }

    /* watch for changes to sandboxed Desktop apps */
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "... loading new AppInfo monitor.");
    home_path = g_strdup_printf ("%s/applications", g_get_user_data_dir ());
    file = g_file_new_for_path (home_path);
    g_free (home_path);
    poller->home_app_monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, &error);
    if (error)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to load AppInfo monitor. May not automatically update settings to Desktop app sandboxes. Reason: %s", error->message);
      g_warning ("Failed to load AppInfo monitor. May not automatically update settings to Desktop app sandboxes. Reason: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
    else
    {
      g_signal_connect (G_OBJECT (poller->home_app_monitor), "changed", G_CALLBACK (xfce_sandbox_poller_on_desktop_changed), poller);
    }

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Finished reloading.");
}



static void
xfce_sandbox_poller_unload (XfceSandboxPoller *poller)
{
    g_return_if_fail (XFCE_IS_SANDBOX_POLLER (poller));

    g_hash_table_remove_all (poller->sandboxes);
}



static XfceSandboxProcess *
xfce_sandbox_process_new (pid_t            pid,
                          const gchar     *name,
                          XfceSandboxType  type)
{
    XfceSandboxProcess *proc;

    g_return_val_if_fail (pid > 0, NULL);
    g_return_val_if_fail (name != NULL, NULL);

    proc = g_malloc (sizeof (XfceSandboxProcess));
    g_return_val_if_fail (proc != NULL, NULL);

    proc->pid  = pid;
    proc->name = g_strdup (name);
    proc->type = type;
    if (type == XFCE_SANDBOX_WORKSPACE)
      proc->ws_number = xfce_workspace_get_workspace_id_from_name (name);
    else
      proc->ws_number = 0;

    proc->desktop_path = NULL;

    return proc;
}



static void
xfce_sandbox_process_free (XfceSandboxProcess *proc)
{
    g_return_if_fail (proc != NULL);

    if (proc->desktop_path)
      g_free (proc->desktop_path);

    g_free (proc->name);
    g_free (proc);
}



static gboolean
xfce_sandbox_process_apply_workspace_prop (XfceSandboxProcess *proc,
                                           const gchar *property_name)
{
    gchar     *key;
    gchar    **argv = NULL;
    gchar    **envp = NULL;
    gchar     *new_path = NULL;
    guint      n;
    guint      n_envp;
    gboolean   succeeded;
    GError    *error = NULL;
    GPid       pid;

    g_return_val_if_fail (proc != NULL, FALSE);
    g_return_val_if_fail (property_name != NULL, FALSE);

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Applying property '%s' to workspace %d", property_name, proc->ws_number);

    key = strrchr (property_name, '/');
    g_return_val_if_fail (key != NULL, FALSE);

    key++;

    /* find out if the modified property is managed by xfsettingsd, if so, build a command */
    if (g_strcmp0 (key, "bandwidth_download") == 0 || g_strcmp0 (key, "bandwidth_upload") == 0)
    {
      /* Exit if there is no Internet connection in the sandbox */
      if (!xfce_workspace_fine_tuned_network (proc->ws_number) || !xfce_workspace_enable_network (proc->ws_number))
      {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Network property, but Workspace %d does not have bandwidth control, nothing to do", proc->ws_number);
        return TRUE;
      }

      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Network property, about to execute Firejail");
      argv = g_malloc0 (sizeof (char *) * 7);
      argv[0] = g_strdup ("firejail");
      argv[1] = g_strdup_printf ("--bandwidth=%s", proc->name);
      argv[2] = g_strdup ("set");
      argv[3] = g_strdup ("auto");
      argv[4] = g_strdup_printf ("%d", xfce_workspace_download_speed (proc->ws_number));
      argv[5] = g_strdup_printf ("%d", xfce_workspace_upload_speed (proc->ws_number));
      argv[6] = NULL;
    }
    else if (g_strcmp0 (key, "proxy_ip") == 0)
    {
      g_info ("Proxy IP setting is not yet supported for Firejail sandboxes. Cannot update proxy IP for workspace '%s'", proc->name);
    } 
    else if (g_strcmp0 (key, "proxy_port") == 0)
    {
      g_info ("Proxy port setting is not yet supported for Firejail sandboxes. Cannot update proxy port for workspace '%s'", proc->name);    
    }

    /* if there's no command to execute, just leave now */
    if (!argv)
      return TRUE;

    /* clean up path in the environment so we're confident the sandbox properly set up */
    for (n = 0; environ && environ[n] != NULL; ++n);
    envp = g_new0 (gchar *, n + 2);
    for (n_envp = n = 0; environ[n] != NULL; ++n)
    {
      if (strncmp (environ[n], "DESKTOP_STARTUP_ID", 18) != 0
          && strncmp (environ[n], "PATH", 4) != 0)
        envp[n_envp++] = environ[n];
    }
    envp[n_envp++] = new_path = g_strdup ("PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/sbin");

    for (n = 0; argv[n]; n++)
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "argv[%d] -> %s", n, argv[n]);
    succeeded = g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH_FROM_ENVP, NULL, NULL, &pid, &error);
    
    if (!succeeded)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to execute firejail to update property '%s': %s", property_name, error->message);
      g_warning ("Failed to execute firejail to update property '%s': %s\n", property_name, error->message);
      g_error_free (error);
      error = NULL;
    }
    else
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Firejail successfully executed, property '%s' is now updated", property_name);

    g_strfreev (argv);
    g_free (new_path);
    g_free (envp);

    return succeeded;
}



static gboolean
xfce_sandbox_process_apply_desktop_props (XfceSandboxProcess *proc)
{
    GKeyFile  *key_file;
    gint       dl, ul;
    gchar    **argv = NULL;
    gchar    **envp = NULL;
    gchar     *new_path = NULL;
    guint      n;
    guint      n_envp;
    gboolean   succeeded;
    GError    *error = NULL;
    GPid       pid;

    g_return_val_if_fail (proc != NULL, FALSE);
    g_return_val_if_fail (proc->desktop_path != NULL, FALSE);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Applying settings for desktop app %s", proc->name);
    
    key_file = g_key_file_new ();
    g_key_file_load_from_file (key_file, proc->desktop_path, G_KEY_FILE_NONE, &error);
    if (error)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Could not load Desktop app %s 's .desktop file: %s. Not managing sandbox %d's settings.", proc->name, error->message, proc->pid);
      g_warning ("Could not load Desktop app %s 's .desktop file: %s. Not managing sandbox %d's settings.\n", proc->name, error->message, proc->pid);
      g_error_free (error);
      error = NULL;
      return FALSE;
    }

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Applying bandwidth limits to desktop app %s", proc->name);

    /* Exit if there is no Internet connection in the sandbox */
    if (g_key_file_has_key (key_file, G_KEY_FILE_DESKTOP_GROUP, XFCE_FIREJAIL_ENABLE_NETWORK_KEY, NULL) &&
        !g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, XFCE_FIREJAIL_ENABLE_NETWORK_KEY, NULL))
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Desktop app %s does not have an Internet connection, nothing to do", proc->name);
      g_key_file_free (key_file);
      return TRUE;
    }

    dl = g_key_file_has_key (key_file, G_KEY_FILE_DESKTOP_GROUP, XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_KEY, NULL)?
                           g_key_file_get_integer (key_file, G_KEY_FILE_DESKTOP_GROUP, XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_KEY, NULL) : XFCE_FIREJAIL_BANDWIDTH_DOWNLOAD_DEFAULT;
    ul = g_key_file_has_key (key_file, G_KEY_FILE_DESKTOP_GROUP, XFCE_FIREJAIL_BANDWIDTH_UPLOAD_KEY, NULL)?
                           g_key_file_get_integer (key_file, G_KEY_FILE_DESKTOP_GROUP, XFCE_FIREJAIL_BANDWIDTH_UPLOAD_KEY, NULL) : XFCE_FIREJAIL_BANDWIDTH_UPLOAD_DEFAULT;

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Network property, about to execute Firejail");
    argv = g_malloc0 (sizeof (char *) * 7);
    argv[0] = g_strdup ("firejail");
    argv[1] = g_strdup_printf ("--bandwidth=%s", proc->name);
    argv[2] = g_strdup ("set");
    argv[3] = g_strdup ("auto");
    argv[4] = g_strdup_printf ("%d", dl);
    argv[5] = g_strdup_printf ("%d", ul);
    argv[6] = NULL;

    /* clean up path in the environment so we're confident the sandbox properly set up */
    for (n = 0; environ && environ[n] != NULL; ++n);
    envp = g_new0 (gchar *, n + 2);
    for (n_envp = n = 0; environ[n] != NULL; ++n)
    {
      if (strncmp (environ[n], "DESKTOP_STARTUP_ID", 18) != 0
          && strncmp (environ[n], "PATH", 4) != 0)
        envp[n_envp++] = environ[n];
    }
    envp[n_envp++] = new_path = g_strdup ("PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/sbin");

    for (n = 0; argv[n]; n++)
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "argv[%d] -> %s", n, argv[n]);
    succeeded = g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH_FROM_ENVP, NULL, NULL, &pid, &error);
    
    if (!succeeded)
    {
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Failed to execute firejail to update bandwidth limits: %s", error->message);
      g_warning ("Failed to execute firejail to update bandwidth limits: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
    else
      xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Firejail successfully executed, bandwidth limits now updated");

    g_strfreev (argv);
    g_free (new_path);
    g_free (envp);
    g_key_file_free (key_file);

    return succeeded;
}



static gchar *
find_desktop_path_from_name (XfceSandboxProcess *proc)
{
    GList *infos;
    GList *iter;
    gchar *path = NULL;

    g_return_val_if_fail (proc != NULL, NULL);

    infos = g_app_info_get_all ();

    for (iter = infos; path == NULL && iter != NULL; iter = iter->next)
    {
      GAppInfo    *info = iter->data;
      const gchar *info_name = g_app_info_get_name (info);

      if (g_strcmp0 (info_name, proc->name) == 0 && G_IS_DESKTOP_APP_INFO (info))
      {
        path = g_strdup (g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (info)));
        if (path == NULL)
        {
          xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Found a Desktop App Info that matches sandbox %d's name %s, but it did not have a filename.", proc->pid, proc->name);
          g_warning ("Found a Desktop App Info that matches sandbox %d's name %s, but it did not have a filename.\n", proc->pid, proc->name);
        }
      }
    }

    g_list_free_full (infos, g_object_unref);

    return path;
}



static void
xfce_sandbox_poller_on_desktop_changed (GFileMonitor     *monitor,
                                        GFile            *file,
                                        GFile            *other_file,
                                        GFileMonitorEvent event_type,
                                        gpointer          user_data)
{
    XfceSandboxPoller  *poller = (XfceSandboxPoller *) user_data;
    GDesktopAppInfo    *appinfo = NULL;
    gchar              *new_path = NULL;
    const gchar        *name = NULL;
    GHashTableIter      iter;
    gpointer            key, value;
    pid_t               pending = 0;
    XfceSandboxProcess *proc;

    if (!(event_type == G_FILE_MONITOR_EVENT_CHANGED ||
          event_type == G_FILE_MONITOR_EVENT_DELETED ||
          event_type == G_FILE_MONITOR_EVENT_CREATED))
        return;

    /* Verify the file is an appinfo */
    new_path = g_file_get_path (file);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "A .desktop file changed: %s.", new_path);

    appinfo = g_desktop_app_info_new_from_filename (new_path);
    if (!appinfo)
    {
        g_free (new_path);
        return;
    }

    /* Find out if a sandbox with the same name exists */
    name = g_app_info_get_name (G_APP_INFO (appinfo));
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "The .desktop file changed corresponds to Desktop app '%s'.", name);

    g_hash_table_iter_init (&iter, poller->sandboxes);
    while (g_hash_table_iter_next (&iter, &key, &value) && !pending)
    {
        proc = (XfceSandboxProcess *) value;
        if (g_strcmp0 (proc->name, name) == 0)
            pending = GPOINTER_TO_UINT (key);
    }

    g_object_unref (appinfo);

    if (!pending)
    {
        g_free (new_path);
        return;
    }

    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "A matching process (PID %d) was found for Desktop app '%s'.", pending, proc->name);

    /* Just gotta re-read the same file */
    if (g_strcmp0 (proc->desktop_path, new_path) == 0 && (event_type == G_FILE_MONITOR_EVENT_CHANGED || event_type == G_FILE_MONITOR_EVENT_CREATED))
    {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Reloading settings for process %d...", pending);
        xfce_sandbox_process_apply_desktop_props (proc);
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Done reloading settings for process %d...", pending);
    }

    /* Depending on the GAppinfo path traversal logic, the created file might
       supersede the existing one for our sandbox, or another file might replace
       the deleted file. Just re-add the sandbox */
    if (event_type != G_FILE_MONITOR_EVENT_CHANGED)
    {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Reloading process %d entirely...", pending);
        if (xfce_sandbox_poller_remove_entry_from_pid (poller, pending))
            xfce_sandbox_poller_add_entry_from_pid (poller, pending, NULL);
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Done reloading process %d entirely...", pending);
    }

    g_free (new_path);
}



static gboolean
xfce_sandbox_process_start_watching (XfceSandboxProcess *proc)
{
    g_return_val_if_fail (proc != NULL, FALSE);
    xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Starting to watch sandbox '%s'", proc->name);

    if (proc->type == XFCE_SANDBOX_WORKSPACE)
    {
      gchar *bandwidth;

      bandwidth = g_strdup_printf ("/security/workspace_%d/bandwidth_upload", proc->ws_number);
      xfce_sandbox_process_apply_workspace_prop (proc, bandwidth);
      g_free (bandwidth);

      return TRUE;
    }
    else if (proc->type == XFCE_SANDBOX_DESKTOP)
    {
      gchar    *desktop_path;

      desktop_path = find_desktop_path_from_name (proc);
      if (!desktop_path)
      {
        xfsettings_dbg (XFSD_DEBUG_FIREJAIL, "Could not find the Desktop app corresponding to sandbox name %s. Not managing sandbox %d's settings.", proc->name, proc->pid);
        g_warning ("Could not find the Desktop app corresponding to sandbox name %s. Not managing sandbox %d's settings.\n", proc->name, proc->pid);
        return FALSE;
      }

      proc->desktop_path = desktop_path;

      return xfce_sandbox_process_apply_desktop_props (proc);
    }

    return FALSE;
}
