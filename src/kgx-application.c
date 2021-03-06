/* kgx-window.c
 *
 * Copyright 2019 Zander Brown
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:kgx-application
 * @title: KgxApplication
 * @short_description: Application
 * 
 * The application, on the face of it nothing particularly interesting but
 * under the hood it contains a #GSource used to monitor the shells (and
 * there children) running in the open #KgxWindow s
 */

#define G_LOG_DOMAIN "Kgx"

#include <glib/gi18n.h>
#include <vte/vte.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "rgba.h"

#include "kgx-config.h"
#include "kgx-application.h"
#include "kgx-window.h"
#include "kgx-pages.h"

#define LOGO_COL_SIZE 28
#define LOGO_ROW_SIZE 14

G_DEFINE_TYPE (KgxApplication, kgx_application, GTK_TYPE_APPLICATION)

enum {
  PROP_0,
  PROP_THEME,
  PROP_FONT,
  PROP_FONT_SCALE,
  PROP_SCROLLBACK_LINES,
  LAST_PROP
};

static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void
kgx_application_set_theme (KgxApplication *self,
                           KgxTheme        theme)
{
  g_return_if_fail (KGX_IS_APPLICATION (self));

  self->theme = theme;

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_THEME]);
}


static void
kgx_application_set_scale (KgxApplication *self,
                           gdouble         scale)
{
  GAction *action;

  g_return_if_fail (KGX_IS_APPLICATION (self));

  self->scale = CLAMP (scale, 0.5, 2.0);

  action = g_action_map_lookup_action (G_ACTION_MAP (self), "zoom-out");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), self->scale > 0.5);
  action = g_action_map_lookup_action (G_ACTION_MAP (self), "zoom-normal");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), self->scale != 1.0);
  action = g_action_map_lookup_action (G_ACTION_MAP (self), "zoom-in");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), self->scale < 2.0);

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_FONT_SCALE]);
}


static void
kgx_application_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  KgxApplication *self = KGX_APPLICATION (object);

  switch (property_id) {
    case PROP_THEME:
      kgx_application_set_theme (self, g_value_get_enum (value));
      break;
    case PROP_FONT_SCALE:
      kgx_application_set_scale (self, g_value_get_double (value));
      break;
    case PROP_SCROLLBACK_LINES:
      self->scrollback_lines = g_value_get_int64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
kgx_application_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  KgxApplication *self = KGX_APPLICATION (object);

  switch (property_id) {
    case PROP_THEME:
      g_value_set_enum (value, self->theme);
      break;
    case PROP_FONT:
      g_value_take_boxed (value, kgx_application_get_font (self));
      break;
    case PROP_FONT_SCALE:
      g_value_set_double (value, self->scale);
      break;
    case PROP_SCROLLBACK_LINES:
      g_value_set_int64 (value, self->scrollback_lines);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_application_finalize (GObject *object)
{
  KgxApplication *self = KGX_APPLICATION (object);

  g_clear_object (&self->settings);
  g_clear_object (&self->desktop_interface);

  g_clear_pointer (&self->watching, g_tree_unref);
  g_clear_pointer (&self->children, g_tree_unref);
  g_clear_pointer (&self->pages, g_tree_unref);

  G_OBJECT_CLASS (kgx_application_parent_class)->finalize (object);
}


static void
kgx_application_activate (GApplication *app)
{
  GtkWindow *window;
  guint32 timestamp;

  timestamp = GDK_CURRENT_TIME;

  /* Get the current window or create one if necessary. */
  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL) {
    window = g_object_new (KGX_TYPE_WINDOW,
                           "application", app,
                           "close-on-zero", TRUE,
                           NULL);
  }

  gtk_window_present_with_time (window, timestamp);
}


static gboolean
handle_watch_iter (gpointer pid,
                   gpointer val,
                   gpointer user_data)
{
  KgxProcess *process = val;
  KgxApplication *self = user_data;
  GPid parent = kgx_process_get_parent (process);
  struct ProcessWatch *watch = NULL;

  watch = g_tree_lookup (self->watching, GINT_TO_POINTER (parent));

  // There are far more processes on the system than there are children
  // of watches, thus lookup are unlikly
  if (G_UNLIKELY (watch != NULL)) {
    if (!g_tree_lookup (self->children, pid)) {
      struct ProcessWatch *child_watch = g_new (struct ProcessWatch, 1);

      child_watch->process = g_rc_box_acquire (process);
      child_watch->page = g_object_ref (watch->page);

      g_debug ("Hello %i!", GPOINTER_TO_INT (pid));

      g_tree_insert (self->children, pid, child_watch);
    }

    kgx_tab_push_child (watch->page, process);
  }

  return FALSE;
}


struct RemoveDead {
  GTree     *plist;
  GPtrArray *dead;
};


static gboolean
remove_dead (gpointer pid,
             gpointer val,
             gpointer user_data)
{
  struct RemoveDead *data = user_data;
  struct ProcessWatch *watch = val;

  if (!g_tree_lookup (data->plist, pid)) {
    g_debug ("%i marked as dead", GPOINTER_TO_INT (pid));

    kgx_tab_pop_child (watch->page, watch->process);

    g_ptr_array_add (data->dead, pid);
  }

  return FALSE;
}


static gboolean
watch (gpointer data)
{
  KgxApplication *self = KGX_APPLICATION (data);
  g_autoptr (GTree) plist = NULL;
  struct RemoveDead dead;

  plist = kgx_process_get_list ();

  g_tree_foreach (plist, handle_watch_iter, self);

  dead.plist = plist;
  dead.dead = g_ptr_array_new_full (1, NULL);

  g_tree_foreach (self->children, remove_dead, &dead);

  // We can't modify self->chilren whilst walking it
  for (int i = 0; i < dead.dead->len; i++) {
    g_tree_remove (self->children, g_ptr_array_index (dead.dead, i));
  }

  g_ptr_array_unref (dead.dead);

  return G_SOURCE_CONTINUE;
}

static inline void
set_watcher (KgxApplication *self, gboolean focused)
{
  g_debug ("updated watcher focused? %s", focused ? "yes" : "no");

  if (self->timeout != 0) {
    g_source_remove (self->timeout);
  }

  // Slow down polling when nothing is focused
  self->timeout = g_timeout_add (focused ? 500 : 2000, watch, self);
  g_source_set_name_by_id (self->timeout, "[kgx] child watcher");
}


static void
kgx_application_startup (GApplication *app)
{
  KgxApplication *self = KGX_APPLICATION (app);
  GtkSettings    *gtk_settings;
  GtkCssProvider *provider;
  const char *const new_window_accels[] = { "<shift><primary>n", NULL };
  const char *const new_tab_accels[] = { "<shift><primary>t", NULL };
  const char *const close_tab_accels[] = { "<shift><primary>w", NULL };
  const char *const copy_accels[] = { "<shift><primary>c", NULL };
  const char *const paste_accels[] = { "<shift><primary>v", NULL };
  const char *const find_accels[] = { "<shift><primary>f", NULL };
  const char *const zoom_in_accels[] = { "<primary>plus", NULL };
  const char *const zoom_out_accels[] = { "<primary>minus", NULL };

  g_type_ensure (KGX_TYPE_TERMINAL);
  g_type_ensure (KGX_TYPE_PAGES);

  G_APPLICATION_CLASS (kgx_application_parent_class)->startup (app);

  hdy_init ();

  gtk_settings = gtk_settings_get_default ();

  g_object_set (G_OBJECT (gtk_settings),
                "gtk-application-prefer-dark-theme", TRUE,
                NULL);

  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "win.new-window", new_window_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "win.new-tab", new_tab_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "win.close-tab", close_tab_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "term.copy", copy_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "term.paste", paste_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "win.find", find_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "win.zoom-in", zoom_in_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "win.zoom-out", zoom_out_accels);

  self->settings = g_settings_new ("org.gnome.zbrown.KingsCross");
  g_settings_bind (self->settings, "theme", app, "theme", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "font-scale", app, "font-scale", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "scrollback-lines", app, "scrollback-lines", G_SETTINGS_BIND_DEFAULT);

  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (provider, RES_PATH "styles.css");
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (provider),
                                             /* Is this stupid? Yes
                                              * Does it fix vte using the wrong
                                              * priority for fallback styles? Yes*/
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

  set_watcher (KGX_APPLICATION (app), TRUE);
}

static int
kgx_application_command_line (GApplication            *app,
                              GApplicationCommandLine *cli)
{
  KgxApplication *self = KGX_APPLICATION (app);
  GVariantDict *options = NULL;
  const char *working_dir = NULL;
  const char *title = NULL;
  const char *command = NULL;
  const char *const *shell = NULL;
  gint64 scrollback;
  GtkWidget *window;
  g_autofree char *abs_path = NULL;

  options = g_application_command_line_get_options_dict (cli);

  g_variant_dict_lookup (options, "working-directory", "^&ay", &working_dir);
  g_variant_dict_lookup (options, "title", "&s", &title);
  g_variant_dict_lookup (options, "command", "^&ay", &command);

  if (g_variant_dict_lookup (options, "set-shell", "^as", &shell) && shell) {
    g_settings_set_strv (self->settings, "shell", shell);

    return 0;
  }

  if (g_variant_dict_lookup (options, "set-scrollback", "x", &scrollback)) {
    g_settings_set_int64 (self->settings, "scrollback-lines", scrollback);

    return 0;
  }

  if (working_dir != NULL) {
    abs_path = g_canonicalize_filename (working_dir, NULL);
  }

  window = g_object_new (KGX_TYPE_WINDOW,
                         "application", app,
                         "close-on-zero", command == NULL,
                         "initial-work-dir", abs_path,
  #if IS_GENERIC
                         "title", title ? title : _("Terminal"),
  #else
                         "title", title ? title : _("King’s Cross"),
  #endif
                         "command", command,
                         NULL);
  gtk_widget_show (window);

  return 0;
}

static void
print_center (char *msg, int ign, short width)
{
  int half_msg = 0;
  int half_screen = 0;

  half_msg = strlen (msg) / 2;
  half_screen = width / 2;

  g_print ("%*s\n",
           half_screen + half_msg,
           msg);
}

static void
print_logo (short width)
{
  g_autoptr (GFile) logo = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) logo_lines = NULL;
  g_autofree char *logo_text = NULL;
  int i = 0;
  int half_screen = width / 2;

  logo = g_file_new_for_uri ("resource:/" RES_PATH "logo.txt");

  g_file_load_contents (logo, NULL, &logo_text, NULL, NULL, &error);

  if (error) {
    g_error ("Wat? %s", error->message);
  }

  logo_lines = g_strsplit (logo_text, "\n", -1);

  while (logo_lines[i]) {
    g_print ("%*s%s\n",
             half_screen - (LOGO_COL_SIZE / 2),
             "",
             logo_lines[i]);

    i++;
  }
}

static int
kgx_application_handle_local_options (GApplication *app,
                                      GVariantDict *options)
{
  gboolean version = FALSE;
  gboolean about = FALSE;

  if (g_variant_dict_lookup (options, "version", "b", &version)) {
    if (version) {
      // Translators: The leading # is intentional, the initial %s is the
      // version of King's Cross itself, the latter format is the VTE version
      g_print (_("# King’s Cross %s using VTE %u.%u.%u %s\n"),
               PACKAGE_VERSION,
               vte_get_major_version (),
               vte_get_minor_version (),
               vte_get_micro_version (),
               vte_get_features ());
      return 0;
    }
  }

  if (g_variant_dict_lookup (options, "about", "b", &about)) {
    if (about) {
      g_autofree char *copyright = g_strdup_printf (_("© %s Zander Brown"),
                                                    "2019-2021");
      struct winsize w;
      int padding = 0;

      ioctl (STDOUT_FILENO, TIOCGWINSZ, &w);

      padding = ((w.ws_row -1) - (LOGO_ROW_SIZE + 5)) / 2;

      for (int i = 0; i < padding; i++) {
        g_print ("\n");
      }

      print_logo (w.ws_col);
      print_center (_("King’s Cross"), -1, w.ws_col);
      print_center (PACKAGE_VERSION, -1, w.ws_col);
      print_center (_("Terminal Emulator"), -1, w.ws_col);
      print_center (copyright, -1, w.ws_col);
      print_center (_("GPL 3.0 or later"), -1, w.ws_col);

      for (int i = 0; i < padding; i++) {
        g_print ("\n");
      }

      return 0;
    }
  }

  return G_APPLICATION_CLASS (kgx_application_parent_class)->handle_local_options (app, options);
}


static void
kgx_application_class_init (KgxApplicationClass *klass)
{
  GObjectClass      *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class    = G_APPLICATION_CLASS (klass);

  object_class->set_property = kgx_application_set_property;
  object_class->get_property = kgx_application_get_property;
  object_class->finalize = kgx_application_finalize;

  app_class->activate = kgx_application_activate;
  app_class->startup = kgx_application_startup;
  app_class->command_line = kgx_application_command_line;
  app_class->handle_local_options = kgx_application_handle_local_options;

  /**
   * KgxApplication:theme:
   * 
   * The palette to use, one of the values of #KgxTheme
   * 
   * Officially only "night" exists, "hacker" is just a little fun
   * 
   * Bound to /org/gnome/zbrown/KingsCross/theme so changes persist
   * 
   * Stability: Private
   */
  pspecs[PROP_THEME] =
    g_param_spec_enum ("theme", "Theme", "Terminal theme",
                       KGX_TYPE_THEME, KGX_THEME_NIGHT,
                       G_PARAM_READWRITE);

  pspecs[PROP_FONT] =
    g_param_spec_boxed ("font", "Font", "Monospace font",
                         PANGO_TYPE_FONT_DESCRIPTION,
                         G_PARAM_READABLE);

  pspecs[PROP_FONT_SCALE] =
    g_param_spec_double ("font-scale", "Font scale", "Font scaling",
                         0.5, 2.0, 1.0,
                         G_PARAM_READWRITE);

  /**
   * KgxApplication:scrollback-lines:
   * 
   * How many lines of scrollback #KgxTerminal should keep
   * 
   * Bound to /org/gnome/zbrown/KingsCross/scrollback-lines so changes persist
   * 
   * Stability: Private
   * 
   * Since: 0.5.0
   */
  pspecs[PROP_SCROLLBACK_LINES] =
    g_param_spec_int64 ("scrollback-lines", "Scrollback Lines", "Size of the scrollback",
                        G_MININT64, G_MAXINT64, 512,
                        G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);
}


static void
clear_watch (struct ProcessWatch *watch)
{
  g_return_if_fail (watch != NULL);

  g_clear_pointer (&watch->process, kgx_process_unref);
  g_clear_object (&watch->page);

  g_clear_pointer (&watch, g_free);
}


static void
font_changed (GSettings      *settings,
              const char     *key,
              KgxApplication *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_FONT]);
}


static GOptionEntry entries[] = {
  {
    "version",
    0,
    0,
    G_OPTION_ARG_NONE,
    NULL,
    NULL,
    NULL
  },
  {
    "about",
    0,
    0,
    G_OPTION_ARG_NONE,
    NULL,
    NULL,
    NULL
  },
  {
    "command",
    'e',
    0,
    G_OPTION_ARG_FILENAME,
    NULL,
    N_("Execute the argument to this option inside the terminal"),
    NULL
  },
  {
    "working-directory",
    0,
    0,
    G_OPTION_ARG_FILENAME,
    NULL,
    N_("Set the working directory"),
    // Translators: Placeholder of for a given directory
    N_("DIRNAME")
  },
  {
    "wait",
    0,
    0,
    G_OPTION_ARG_NONE,
    NULL,
    N_("Wait until the child exits (TODO)"),
    NULL
  },
  {
    "title",
    'T',
    0,
    G_OPTION_ARG_STRING,
    NULL,
    N_("Set the initial window title"),
    NULL
  },
  {
    "set-shell",
    0,
    0,
    G_OPTION_ARG_STRING_ARRAY,
    NULL,
    N_("ADVANCED: Set the shell to launch"),
    NULL
  },
  {
    "set-scrollback",
    0,
    0,
    G_OPTION_ARG_INT64,
    NULL,
    N_("ADVANCED: Set the scrollback length"),
    NULL
  },
  { NULL }
};


static void
new_window_activated (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       data)
{
  KgxApplication *self = KGX_APPLICATION (data);
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (window != NULL)
    {
      g_action_group_activate_action (G_ACTION_GROUP (window), "new-window", NULL);
    }
}


static void
focus_activated (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       data)
{
  KgxApplication *self = KGX_APPLICATION (data);
  GtkWidget *window;
  KgxPages *pages;
  KgxTab *page;

  page = kgx_application_lookup_page (self, g_variant_get_uint32 (parameter));
  pages = kgx_tab_get_pages (page);
  kgx_pages_focus_page (pages, page);
  window = gtk_widget_get_toplevel (GTK_WIDGET (pages));

  gtk_window_present_with_time (GTK_WINDOW (window), GDK_CURRENT_TIME);
}


static void
zoom_out_activated (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       data)
{
  KgxApplication *self = KGX_APPLICATION (data);

  if (self->scale < 0.1) {
    return;
  }

  kgx_application_set_scale (self, self->scale - 0.1);
}


static void
zoom_normal_activated (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       data)
{
  KgxApplication *self = KGX_APPLICATION (data);

  kgx_application_set_scale (self, 1.0);
}


static void
zoom_in_activated (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
  KgxApplication *self = KGX_APPLICATION (data);

  kgx_application_set_scale (self, self->scale + 0.1);
}


static GActionEntry app_entries[] =
{
  { "new-window", new_window_activated, NULL, NULL, NULL },
  { "focus-page", focus_activated, "u", NULL, NULL },
  { "zoom-out", zoom_out_activated, NULL, NULL, NULL },
  { "zoom-normal", zoom_normal_activated, NULL, NULL, NULL },
  { "zoom-in", zoom_in_activated, NULL, NULL, NULL },
};


static void
kgx_application_init (KgxApplication *self)
{
  g_application_add_main_option_entries (G_APPLICATION (self), entries);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   app_entries,
                                   G_N_ELEMENTS (app_entries),
                                   self);

  self->desktop_interface = g_settings_new (DESKTOP_INTERFACE_SETTINGS_SCHEMA);

  g_signal_connect (self->desktop_interface,
                    "changed::" MONOSPACE_FONT_KEY_NAME,
                    G_CALLBACK (font_changed),
                    self);

  self->watching = g_tree_new_full (kgx_pid_cmp,
                                    NULL,
                                    NULL,
                                    (GDestroyNotify) clear_watch);
  self->children = g_tree_new_full (kgx_pid_cmp,
                                    NULL,
                                    NULL,
                                    (GDestroyNotify) clear_watch);
  self->pages = g_tree_new_full (kgx_pid_cmp,
                                 NULL,
                                 NULL,
                                 (GDestroyNotify) g_object_unref);

  self->active = 0;
  self->timeout = 0;
}


/**
 * kgx_application_add_watch:
 * @self: the #KgxApplication
 * @pid: the shell process to watch
 * @page: the #KgxTab the shell is running in
 * 
 * Registers a new shell process with the pid watcher
 * 
 * Since: 0.3.0
 */
void
kgx_application_add_watch (KgxApplication *self,
                           GPid            pid,
                           KgxTab        *page)
{
  struct ProcessWatch *watch;

  g_return_if_fail (KGX_IS_APPLICATION (self));
  g_return_if_fail (KGX_IS_TAB (page));

  watch = g_new0 (struct ProcessWatch, 1);
  watch->process = kgx_process_new (pid);
  watch->page = g_object_ref (page);

  g_debug ("Started watching %i", pid);

  g_return_if_fail (KGX_IS_TAB (watch->page));

  g_tree_insert (self->watching, GINT_TO_POINTER (pid), watch);
}

/**
 * kgx_application_remove_watch:
 * @self: the #KgxApplication
 * @pid: the shell process to stop watch watching
 * 
 * unregisters the shell with #GPid pid
 */
void
kgx_application_remove_watch (KgxApplication *self,
                              GPid            pid)
{
  g_return_if_fail (KGX_IS_APPLICATION (self));

  if (G_LIKELY (g_tree_lookup (self->watching, GINT_TO_POINTER (pid)))) {
    g_tree_remove (self->watching, GINT_TO_POINTER (pid));
    g_debug ("Stopped watching %i", pid);
  } else {
    g_warning ("Unknown process %i", pid);
  }
}


/**
 * kgx_application_get_font:
 * @self: the #KgxApplication
 *
 * Creates a #PangoFontDescription for the system monospace font.
 *
 * Returns: (transfer full): a new #PangoFontDescription
 */
PangoFontDescription *
kgx_application_get_font (KgxApplication *self)
{
  // Taken from gnome-terminal
  g_autofree char *font = NULL;

  g_return_val_if_fail (KGX_IS_APPLICATION (self), NULL);

  font = g_settings_get_string (self->desktop_interface,
                                MONOSPACE_FONT_KEY_NAME);

  return pango_font_description_from_string (font);
}


/**
 * kgx_application_get_shell:
 * @self: the #KgxApplication
 *
 * Figure out what shell to launch for #KgxSimpleTabs, this is generally
 * whatever vte decides is the users default shell. Alternatively it may
 * be a custom command or if all else fails: /bin/sh
 *
 * Returns: (transfer full): the #GStrv shell vector
 * 
 * Since: 0.5.0
 */
GStrv
kgx_application_get_shell (KgxApplication *self)
{
  g_autofree char *user = vte_get_user_shell ();
  g_auto (GStrv) argv  =  NULL;
  g_auto (GStrv) custom  =  NULL;
  g_autoptr (GError) error = NULL;

  g_return_val_if_fail (KGX_IS_APPLICATION (self), NULL);

  g_shell_parse_argv (user, NULL, &argv, &error);
  if (error) {
    g_warning ("Failed to parse “%s” as a command", user);
    argv = g_new0 (char *, 2);
    argv[0] = g_strdup ("/bin/sh");
    argv[1] = NULL;
  }

  custom = g_settings_get_strv (self->settings, "shell");

  if (g_strv_length (custom) > 0) {
    return g_steal_pointer (&custom);
  } else {
    return g_steal_pointer (&argv);
  }
}


/**
 * kgx_application_push_active:
 * @self: the #KgxApplication
 *
 * Increase the active window count
 */
void
kgx_application_push_active (KgxApplication *self)
{
  g_return_if_fail (KGX_IS_APPLICATION (self));

  self->active++;

  g_debug ("push_active");

  if (G_LIKELY (self->active > 0)) {
    set_watcher (self, TRUE);
  } else {
    set_watcher (self, FALSE);
  }
}


/**
 * kgx_application_pop_active:
 * @self: the #KgxApplication
 *
 * Decrease the active window count
 */
void
kgx_application_pop_active (KgxApplication *self)
{
  g_return_if_fail (KGX_IS_APPLICATION (self));

  self->active--;

  g_debug ("pop_active");

  if (G_LIKELY (self->active < 1)) {
    set_watcher (self, FALSE);
  } else {
    set_watcher (self, TRUE);
  }
}


/**
 * kgx_application_add_page:
 * @self: the instance to look for @id in
 * @page: the page to add
 * 
 * Register a new #KgxTab with @self
 */
void
kgx_application_add_page (KgxApplication *self,
                          KgxTab        *page)
{
  guint id = 0;

  g_return_if_fail (KGX_IS_APPLICATION (self));
  g_return_if_fail (KGX_IS_TAB (page));

  id = kgx_tab_get_id (page);

  g_tree_insert (self->pages, GINT_TO_POINTER (id), g_object_ref (page));
}


/**
 * kgx_application_lookup_page:
 * @self: the instance to look for @id in
 * @id: the page id to look for
 * 
 * Try and find a #KgxTab with @id in @self
 * 
 * Returns: (transfer none) (nullable): the found #KgxTab or %NULL
 */
KgxTab *
kgx_application_lookup_page (KgxApplication *self,
                             guint           id)
{
  g_return_val_if_fail (KGX_IS_APPLICATION (self), NULL);

  return g_tree_lookup (self->pages, GUINT_TO_POINTER (id));
}
