/* kgx-pages.c
 *
 * Copyright 2019-2023 Zander Brown
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

#include "kgx-config.h"

#include <glib/gi18n.h>

#include "kgx-close-dialog.h"
#include "kgx-pages.h"
#include "kgx-tab.h"
#include "kgx-settings.h"
#include "kgx-marshals.h"


/**
 * KgxPages:
 *
 * The container of open #KgxTab (uses #AdwTabView internally)
 */
typedef struct _KgxPagesPrivate KgxPagesPrivate;
struct _KgxPagesPrivate {
  KgxSettings          *settings;

  GtkWidget            *view;
  char                 *title;
  GFile                *path;
  KgxTab               *active_page;
  gboolean              is_active;
  KgxStatus             page_status;
  gboolean              search_mode_enabled;
  gboolean              ringing;

  GtkWidget            *status_message;
  GtkWidget            *status_revealer;

  int                   last_cols;
  int                   last_rows;
  guint                 timeout;

  GSignalGroup         *active_page_signals;
  GBindingGroup        *active_page_binds;
  GSignalGroup         *settings_signals;

  GBinding             *is_active_bind;

  AdwTabPage           *action_page;
};


G_DEFINE_TYPE_WITH_PRIVATE (KgxPages, kgx_pages, ADW_TYPE_BIN)


enum {
  PROP_0,
  PROP_SETTINGS,
  PROP_TAB_VIEW,
  PROP_TAB_COUNT,
  PROP_TITLE,
  PROP_PATH,
  PROP_ACTIVE_PAGE,
  PROP_IS_ACTIVE,
  PROP_STATUS,
  PROP_SEARCH_MODE_ENABLED,
  PROP_RINGING,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


enum {
  ZOOM,
  CREATE_TEAROFF_HOST,
  MAYBE_CLOSE_WINDOW,
  N_SIGNALS
};
static guint signals[N_SIGNALS];


static void
kgx_pages_dispose (GObject *object)
{
  KgxPages *self = KGX_PAGES (object);
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);

  g_clear_object (&priv->settings);

  g_clear_handle_id (&priv->timeout, g_source_remove);

  g_clear_pointer (&priv->title, g_free);
  g_clear_object (&priv->path);

  g_clear_object (&priv->is_active_bind);
  g_clear_object (&priv->active_page);

  G_OBJECT_CLASS (kgx_pages_parent_class)->dispose (object);
}


static void
kgx_pages_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  KgxPages *self = KGX_PAGES (object);
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);

  switch (property_id) {
    case PROP_SETTINGS:
      g_value_set_object (value, priv->settings);
      break;
    case PROP_TAB_COUNT:
      g_value_set_uint (value, adw_tab_view_get_n_pages (ADW_TAB_VIEW (priv->view)));
      break;
    case PROP_TAB_VIEW:
      g_value_set_object (value, priv->view);
      break;
    case PROP_TITLE:
      g_value_set_string (value, priv->title);
      break;
    case PROP_PATH:
      g_value_set_object (value, priv->path);
      break;
    case PROP_ACTIVE_PAGE:
      g_value_set_object (value, priv->active_page);
      break;
    case PROP_IS_ACTIVE:
      g_value_set_boolean (value, priv->is_active);
      break;
    case PROP_STATUS:
      g_value_set_flags (value, priv->page_status);
      break;
    case PROP_SEARCH_MODE_ENABLED:
      g_value_set_boolean (value, priv->search_mode_enabled);
      break;
    case PROP_RINGING:
      g_value_set_boolean (value, priv->ringing);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_pages_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  KgxPages *self = KGX_PAGES (object);
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);

  switch (property_id) {
    case PROP_SETTINGS:
      g_set_object (&priv->settings, g_value_get_object (value));
      break;
    case PROP_TITLE:
      g_clear_pointer (&priv->title, g_free);
      priv->title = g_value_dup_string (value);
      break;
    case PROP_PATH:
      g_set_object (&priv->path, g_value_get_object (value));
      break;
    case PROP_ACTIVE_PAGE:
      if (priv->active_page) {
        g_object_set (priv->active_page, "is-active", FALSE, NULL);
      }
      g_clear_object (&priv->is_active_bind);
      g_set_object (&priv->active_page, g_value_get_object (value));
      if (priv->active_page) {
        priv->is_active_bind =
          g_object_bind_property (self, "is-active",
                                  priv->active_page, "is-active",
                                  G_BINDING_SYNC_CREATE);
      }
      break;
    case PROP_IS_ACTIVE:
      priv->is_active = g_value_get_boolean (value);
      break;
    case PROP_STATUS:
      priv->page_status = g_value_get_flags (value);
      break;
    case PROP_SEARCH_MODE_ENABLED:
      priv->search_mode_enabled = g_value_get_boolean (value);
      break;
    case PROP_RINGING:
      priv->ringing = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
size_timeout (gpointer data)
{
  KgxPages *self = data;
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);

  priv->timeout = 0;

  gtk_revealer_set_reveal_child (GTK_REVEALER (priv->status_revealer), FALSE);
}


static void
size_changed (KgxTab   *tab,
              guint     rows,
              guint     cols,
              KgxPages *self)
{
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);
  g_autofree char *label = NULL;

  if (gtk_widget_in_destruction (GTK_WIDGET (self)))
    return;

  if (cols == priv->last_cols && rows == priv->last_rows) {
    return;
  }

  priv->last_cols = cols;
  priv->last_rows = rows;

  if (gtk_window_is_maximized (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))))) {
    // Don't show when maximised as it isn't very interesting
    return;
  }

  g_clear_handle_id (&priv->timeout, g_source_remove);
  priv->timeout = g_timeout_add_once (800, size_timeout, self);
  g_source_set_name_by_id (priv->timeout, "[kgx] resize label timeout");

  label = g_strdup_printf ("%i × %i", cols, rows);

  gtk_label_set_label (GTK_LABEL (priv->status_message), label);
  gtk_revealer_set_reveal_child (GTK_REVEALER (priv->status_revealer), TRUE);
}


static void
died (KgxTab         *page,
      GtkMessageType  type,
      const char     *message,
      gboolean        success,
      KgxPages       *self)
{
  KgxPagesPrivate *priv;
  AdwTabPage *tab_page;
  gboolean close_on_quit;
  int tab_count;

  priv = kgx_pages_get_instance_private (self);
  tab_page = adw_tab_view_get_page (ADW_TAB_VIEW (priv->view), GTK_WIDGET (page));

  g_object_get (page, "close-on-quit", &close_on_quit, NULL);

  if (!(close_on_quit && success)) {
    return;
  }

  g_object_get (self, "tab-count", &tab_count, NULL);

  if (tab_count < 1) {
    return;
  }

  adw_tab_view_close_page (ADW_TAB_VIEW (priv->view), tab_page);
}


static void
zoom (KgxTab   *tab,
      KgxZoom   dir,
      KgxPages *self)
{
  g_signal_emit (self, signals[ZOOM], 0, dir);
}


static void
page_attached (AdwTabView *view,
               AdwTabPage *page,
               int         position,
               KgxPages   *self)
{
  KgxTab *tab;

  g_return_if_fail (ADW_IS_TAB_PAGE (page));

  tab = KGX_TAB (adw_tab_page_get_child (page));

  g_object_connect (tab,
                    "signal::died", G_CALLBACK (died), self,
                    "signal::zoom", G_CALLBACK (zoom), self,
                    NULL);
}


static void
page_detached (AdwTabView *view,
               AdwTabPage *page,
               int         position,
               KgxPages   *self)
{
  KgxTab *tab;

  g_return_if_fail (ADW_IS_TAB_PAGE (page));

  tab = KGX_TAB (adw_tab_page_get_child (page));

  g_object_disconnect (tab,
                       "any-signal::died", G_CALLBACK (died), self,
                       "any-signal::zoom", G_CALLBACK (zoom), self,
                       NULL);

  if (adw_tab_view_get_n_pages (ADW_TAB_VIEW (view)) == 0) {
    g_signal_emit (self, signals[MAYBE_CLOSE_WINDOW], 0);
  }
}


static AdwTabView *
create_window (AdwTabView *view,
               KgxPages   *self)
{
  KgxPages *new_pages;
  KgxPagesPrivate *priv;

  g_signal_emit (self, signals[CREATE_TEAROFF_HOST], 0, &new_pages);

  priv = kgx_pages_get_instance_private (new_pages);

  return ADW_TAB_VIEW (priv->view);
}


typedef struct {
  AdwTabView *view;
  AdwTabPage *page;
} CloseData;


static void
close_data_free (gpointer data)
{
  CloseData *self = data;

  if (!self) {
    return;
  }

  g_clear_object (&self->view);
  g_clear_weak_pointer (&self->page);

  g_free (self);
}


G_DEFINE_AUTOPTR_CLEANUP_FUNC (CloseData, close_data_free)


static void
close_response (AdwMessageDialog *dialogue,
                const char       *response,
                gpointer          user_data)
{
  CloseData *data = user_data;

  if (G_UNLIKELY (!data->page)) {
    return;
  }

  adw_tab_view_close_page_finish (ADW_TAB_VIEW (data->view),
                                  data->page,
                                  !g_strcmp0 (response, "close"));

  gtk_window_destroy (GTK_WINDOW (dialogue));
}


static gboolean
close_page (AdwTabView *view,
            AdwTabPage *page,
            KgxPages   *self)
{
  g_autoptr (CloseData) data = g_new0 (CloseData, 1);
  g_autoptr (GPtrArray) children = NULL;
  GtkWidget *dlg;
  GtkRoot *root;

  /* slightly paranoid */
  g_set_object (&data->view, view);
  g_set_weak_pointer (&data->page, page);

  children = kgx_tab_get_children (KGX_TAB (adw_tab_page_get_child (page)));

  if (children->len < 1) {
    return GDK_EVENT_PROPAGATE; /* Aka no, I don’t want to block closing */
  }

  root = gtk_widget_get_root (GTK_WIDGET (self));

  dlg = kgx_close_dialog_new (KGX_CONTEXT_TAB, children);

  gtk_window_set_transient_for (GTK_WINDOW (dlg), GTK_WINDOW (root));

  g_signal_connect_data (dlg,
                         "response", G_CALLBACK (close_response),
                         g_steal_pointer (&data), (GClosureNotify) close_data_free,
                         0);

  gtk_window_present (GTK_WINDOW (dlg));

  return GDK_EVENT_STOP; // Block the close
}


static void
setup_menu (AdwTabView *view,
            AdwTabPage *page,
            KgxPages   *self)
{
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);

  priv->action_page = page;
}


static gboolean
title_to_title (GBinding     *binding,
                const GValue *from_value,
                GValue       *to_value,
                gpointer      user_data)
{
  KgxTab *tab = KGX_TAB (user_data);
  g_autofree char *title = g_value_dup_string (from_value);

  if (G_UNLIKELY (!title)) {
    /* Translators: %i is, from the users perspective, a random number.
     * this string will only be seen when the running program has
     * failed to set a title, and exists purely to avoid blank tabs
     */
    g_autofree char *placeholder = g_strdup_printf (_("Tab %i"),
                                                    kgx_tab_get_id (tab));

    g_set_str (&title, placeholder);
  }

  g_value_take_string (to_value, g_steal_pointer (&title));

  return TRUE;
}


static gboolean
status_to_icon (GBinding     *binding,
                const GValue *from_value,
                GValue       *to_value,
                gpointer      user_data)
{
  KgxStatus status = g_value_get_flags (from_value);

  if (status & KGX_REMOTE)
    g_value_take_object (to_value, g_themed_icon_new ("status-remote-symbolic"));
  else if (status & KGX_PRIVILEGED)
    g_value_take_object (to_value, g_themed_icon_new ("status-privileged-symbolic"));
  else
    g_value_set_object (to_value, NULL);

  return TRUE;
}


static gboolean
path_to_keyword (GBinding     *binding,
                 const GValue *from_value,
                 GValue       *to_value,
                 gpointer      user_data)
{
  GFile *path = g_value_get_object (from_value);

  if (path) {
    g_value_take_string (to_value, g_file_get_path (path));
  } else {
    g_value_set_static_string (to_value, "");
  }

  return TRUE;
}


static gboolean
ringing_to_icon (GBinding     *binding,
                 const GValue *from_value,
                 GValue       *to_value,
                 gpointer      user_data)
{
  if (g_value_get_boolean (from_value)) {
    g_value_take_object (to_value, g_themed_icon_new ("bell-outline"));
  } else {
    g_value_set_object (to_value, NULL);
  }

  return TRUE;
}


static gboolean
object_accumulator (GSignalInvocationHint *ihint,
                    GValue                *return_value,
                    const GValue          *signal_value,
                    gpointer               data)
{
  GObject *object = g_value_get_object (signal_value);

  g_value_set_object (return_value, object);

  return !object;
}


static void
kgx_pages_class_init (KgxPagesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = kgx_pages_dispose;
  object_class->get_property = kgx_pages_get_property;
  object_class->set_property = kgx_pages_set_property;

  pspecs[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         KGX_TYPE_SETTINGS,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * KgxPages:tab-view:
   *
   * The #AdwTabView
   *
   * Stability: Private
   */
  pspecs[PROP_TAB_VIEW] =
    g_param_spec_object ("tab-view", NULL, NULL,
                         ADW_TYPE_TAB_VIEW,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * KgxPages:tab-count:
   *
   * The number of open pages
   *
   * Stability: Private
   */
  pspecs[PROP_TAB_COUNT] =
    g_param_spec_uint ("tab-count", NULL, NULL,
                       0,
                       G_MAXUINT32,
                       0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * KgxPages:title:
   *
   * The #KgxTab:tab-title of the current #KgxTab
   *
   * Note the writability of this property in an implementation detail, DO NOT
   * set this property
   *
   * Stability: Private
   */
  pspecs[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * KgxPages:path:
   *
   * The #KgxTab:tab-path of the current #KgxTab
   *
   * Note the writability of this property in an implementation detail, DO NOT
   * set this property
   *
   * Stability: Private
   */
  pspecs[PROP_PATH] =
    g_param_spec_object ("path", NULL, NULL,
                         G_TYPE_FILE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * KgxPages:active-page:
   *
   * The #KgxTab in #KgxPages:tab-view's current #AdwTabPage
   *
   * Note the writability of this property in an implementation detail, DO NOT
   * set this property
   */
  pspecs[PROP_ACTIVE_PAGE] =
    g_param_spec_object ("active-page", NULL, NULL,
                         KGX_TYPE_TAB,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_IS_ACTIVE] =
    g_param_spec_boolean ("is-active", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_STATUS] =
    g_param_spec_flags ("status", NULL, NULL,
                        KGX_TYPE_STATUS,
                        KGX_NONE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_SEARCH_MODE_ENABLED] =
    g_param_spec_boolean ("search-mode-enabled", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_RINGING] =
    g_param_spec_boolean ("ringing", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);


  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  signals[ZOOM] = g_signal_new ("zoom",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                0, NULL, NULL,
                                kgx_marshals_VOID__ENUM,
                                G_TYPE_NONE,
                                1,
                                KGX_TYPE_ZOOM);
  g_signal_set_va_marshaller (signals[ZOOM],
                              G_TYPE_FROM_CLASS (klass),
                              kgx_marshals_VOID__ENUMv);

  signals[CREATE_TEAROFF_HOST] = g_signal_new ("create-tearoff-host",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               object_accumulator, NULL,
                                               kgx_marshals_OBJECT__VOID,
                                               KGX_TYPE_PAGES,
                                               0);
  g_signal_set_va_marshaller (signals[CREATE_TEAROFF_HOST],
                              G_TYPE_FROM_CLASS (klass),
                              kgx_marshals_OBJECT__VOIDv);

  signals[MAYBE_CLOSE_WINDOW] = g_signal_new ("maybe-close-window",
                                              G_TYPE_FROM_CLASS (klass),
                                              G_SIGNAL_RUN_LAST,
                                              0,
                                              NULL, NULL,
                                              kgx_marshals_VOID__VOID,
                                              G_TYPE_NONE,
                                              0);
  g_signal_set_va_marshaller (signals[MAYBE_CLOSE_WINDOW],
                              G_TYPE_FROM_CLASS (klass),
                              kgx_marshals_VOID__VOIDv);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               KGX_APPLICATION_PATH "kgx-pages.ui");

  gtk_widget_class_bind_template_child_private (widget_class, KgxPages, view);
  gtk_widget_class_bind_template_child_private (widget_class, KgxPages, status_message);
  gtk_widget_class_bind_template_child_private (widget_class, KgxPages, status_revealer);
  gtk_widget_class_bind_template_child_private (widget_class, KgxPages, active_page_signals);
  gtk_widget_class_bind_template_child_private (widget_class, KgxPages, active_page_binds);
  gtk_widget_class_bind_template_child_private (widget_class, KgxPages, settings_signals);

  gtk_widget_class_bind_template_callback (widget_class, page_attached);
  gtk_widget_class_bind_template_callback (widget_class, page_detached);
  gtk_widget_class_bind_template_callback (widget_class, create_window);
  gtk_widget_class_bind_template_callback (widget_class, close_page);
  gtk_widget_class_bind_template_callback (widget_class, setup_menu);

  gtk_widget_class_set_css_name (widget_class, "pages");
}


static void
kgx_pages_init (KgxPages *self)
{
  KgxPagesPrivate *priv = kgx_pages_get_instance_private (self);
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_group_connect (priv->active_page_signals,
                          "size-changed", G_CALLBACK (size_changed),
                          self);

  g_signal_group_connect_swapped (priv->settings_signals, "notify::theme",
                                  G_CALLBACK (adw_tab_view_invalidate_thumbnails),
                                  priv->view);

  g_signal_connect_object (style_manager,
                           "notify::dark",
                           G_CALLBACK (adw_tab_view_invalidate_thumbnails),
                           priv->view,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (style_manager,
                           "notify::high-contrast",
                           G_CALLBACK (adw_tab_view_invalidate_thumbnails),
                           priv->view,
                           G_CONNECT_SWAPPED);

  g_binding_group_bind (priv->active_page_binds, "search-mode-enabled",
                        self, "search-mode-enabled",
                        G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  adw_tab_view_remove_shortcuts (ADW_TAB_VIEW (priv->view),
                                 ADW_TAB_VIEW_SHORTCUT_CONTROL_HOME |
                                 ADW_TAB_VIEW_SHORTCUT_CONTROL_END |
                                 ADW_TAB_VIEW_SHORTCUT_CONTROL_SHIFT_HOME |
                                 ADW_TAB_VIEW_SHORTCUT_CONTROL_SHIFT_END);
}


void
kgx_pages_add_page (KgxPages *self,
                    KgxTab   *tab)
{
  KgxPagesPrivate *priv;
  AdwTabPage *page;

  g_return_if_fail (KGX_IS_PAGES (self));

  priv = kgx_pages_get_instance_private (self);

  kgx_tab_set_initial_title (tab, priv->title, priv->path);

  page = adw_tab_view_add_page (ADW_TAB_VIEW (priv->view), GTK_WIDGET (tab), NULL);
  g_object_bind_property_full (tab, "tab-title",
                               page, "title",
                               G_BINDING_SYNC_CREATE,
                               title_to_title, NULL,
                               tab, NULL);
  g_object_bind_property (tab, "tab-tooltip", page, "tooltip", G_BINDING_SYNC_CREATE);
  g_object_bind_property (tab, "needs-attention", page, "needs-attention", G_BINDING_SYNC_CREATE);
  g_object_bind_property_full (tab, "tab-status", page, "icon", G_BINDING_SYNC_CREATE,
                               status_to_icon, NULL, NULL, NULL);
  g_object_bind_property_full (tab, "tab-path", page, "keyword", G_BINDING_SYNC_CREATE,
                               path_to_keyword, NULL, NULL, NULL);
  g_object_bind_property_full (tab, "ringing", page, "indicator-icon", G_BINDING_SYNC_CREATE,
                               ringing_to_icon, NULL, NULL, NULL);
}


/**
 * kgx_pages_focus_page:
 * @self: the #KgxPages
 * @page: the #KgxTab to focus
 *
 * Switch to a page
 */
void
kgx_pages_focus_page (KgxPages *self,
                      KgxTab  *page)
{
  KgxPagesPrivate *priv;
  AdwTabPage *index;

  g_return_if_fail (KGX_IS_PAGES (self));
  g_return_if_fail (KGX_IS_TAB (page));

  priv = kgx_pages_get_instance_private (self);

  index = adw_tab_view_get_page (ADW_TAB_VIEW (priv->view),
                                 GTK_WIDGET (page));

  g_return_if_fail (index != NULL);

  adw_tab_view_set_selected_page (ADW_TAB_VIEW (priv->view), index);

  gtk_widget_grab_focus (GTK_WIDGET (page));
}


/**
 * kgx_pages_current_status:
 * @self: the #KgxPages
 *
 * Get the #KgxStatus of the current #KgxTab
 */
KgxStatus
kgx_pages_current_status (KgxPages *self)
{
  KgxPagesPrivate *priv;

  g_return_val_if_fail (KGX_IS_PAGES (self), KGX_NONE);

  priv = kgx_pages_get_instance_private (self);

  return priv->page_status;
}


gboolean
kgx_pages_is_ringing (KgxPages *self)
{
  KgxPagesPrivate *priv;

  g_return_val_if_fail (KGX_IS_PAGES (self), FALSE);

  priv = kgx_pages_get_instance_private (self);

  return priv->ringing;
}


/**
 * kgx_pages_count:
 * @self: the #KgxPages
 *
 * Returns: number of #KgxTab s in @self
 */
int
kgx_pages_count (KgxPages *self)
{
  KgxPagesPrivate *priv;

  g_return_val_if_fail (KGX_IS_PAGES (self), KGX_NONE);

  priv = kgx_pages_get_instance_private (self);

  return adw_tab_view_get_n_pages (ADW_TAB_VIEW (priv->view));
}


/**
 * kgx_pages_get_children:
 * @self: the #KgxPages
 *
 * Call kgx_tab_get_children on all #KgxTab s in @self building a
 * combined list
 *
 * Returns: (element-type Kgx.Process) (transfer full): the list of #KgxProcess
 */
GPtrArray *
kgx_pages_get_children (KgxPages *self)
{
  KgxPagesPrivate *priv;
  GPtrArray *children;
  guint n;

  g_return_val_if_fail (KGX_IS_PAGES (self), NULL);

  priv = kgx_pages_get_instance_private (self);

  children = g_ptr_array_new_full (10, (GDestroyNotify) kgx_process_unref);

  n = adw_tab_view_get_n_pages (ADW_TAB_VIEW (priv->view));

  for (guint i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page (ADW_TAB_VIEW (priv->view), i);
    g_autoptr (GPtrArray) page_children = NULL;

    page_children = kgx_tab_get_children (KGX_TAB (adw_tab_page_get_child (page)));

    g_ptr_array_extend_and_steal (children, g_steal_pointer (&page_children));
  }

  return children;
}


void
kgx_pages_close_page (KgxPages *self)
{
  KgxPagesPrivate *priv;
  AdwTabPage *page;

  g_return_if_fail (KGX_IS_PAGES (self));

  priv = kgx_pages_get_instance_private (self);
  page = priv->action_page;

  if (!page)
    page = adw_tab_view_get_selected_page (ADW_TAB_VIEW (priv->view));

  if (!page)
    return;

  adw_tab_view_close_page (ADW_TAB_VIEW (priv->view), page);
}


void
kgx_pages_detach_page (KgxPages *self)
{
  KgxPagesPrivate *priv;
  AdwTabPage *page;
  AdwTabView *new_view;

  g_return_if_fail (KGX_IS_PAGES (self));

  priv = kgx_pages_get_instance_private (self);
  page = priv->action_page;

  if (!page)
    page = adw_tab_view_get_selected_page (ADW_TAB_VIEW (priv->view));

  if (!page)
    return;

  new_view = create_window (ADW_TAB_VIEW (priv->view), self);
  adw_tab_view_transfer_page (ADW_TAB_VIEW (priv->view), page, new_view, 0);
}


AdwTabPage *
kgx_pages_get_selected_page (KgxPages  *self)
{
  KgxPagesPrivate *priv;

  g_return_val_if_fail (KGX_IS_PAGES (self), NULL);

  priv = kgx_pages_get_instance_private (self);

  return adw_tab_view_get_selected_page (ADW_TAB_VIEW (priv->view));
}
