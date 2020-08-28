/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *   @author Rob Bradford
 *   @author Travis Reitter <travis.reitter@maemo.org>
 *   @author Marco Barisione <marco.barisione@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/channel.h>
#include <telepathy-glib/contact.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/util.h>
#include <rtcom-telepathy-glib/extensions.h>

#include "e-book-backend-tp-contact.h"
#include "e-book-backend-tp-cl.h"
#include "e-book-backend-tp-log.h"

G_DEFINE_TYPE (EBookBackendTpCl, e_book_backend_tp_cl, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_BOOK_BACKEND_TP_CL, EBookBackendTpClPrivate))


typedef struct _EBookBackendTpClContactList EBookBackendTpClContactList;

struct _EBookBackendTpClContactList
{
  TpHandle handle;
  TpChannel *channel;
};

typedef struct _EBookBackendTpClPrivate EBookBackendTpClPrivate;

struct _EBookBackendTpClPrivate
{
  const gchar *account_name;
  McAccount *account;
  TpConnection *conn;
  EBookBackendTpClStatus status;
  EBookBackendTpClContactList *contact_list_channels[CL_LAST_LIST];
  /* maps TpHandle -> (EBookBackendTpContact*) */
  GHashTable *contacts_hash;
};

enum
{
  STATUS_CHANGED = 0,
  CONTACTS_ADDED,
  CONTACTS_REMOVED,
  FLAGS_CHANGED,
  ALIASES_CHANGED,
  PRESENCES_CHANGED,
  AVATAR_TOKENS_CHANGED,
  AVATAR_DATA_CHANGED,
  CAPABILITIES_CHANGED,
  CONTACT_INFO_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void e_book_backend_tp_cl_set_status (EBookBackendTpCl *tpcl, 
    EBookBackendTpClStatus status);
static void connection_status_changed_cb (McAccount *account,
    TpConnectionStatus status, TpConnectionStatusReason reason,
    gpointer userdata);

static void contact_info_changed_cb (TpConnection *conn, guint handle, 
    const GPtrArray *handle_contactinfo, gpointer userdata, GObject *weak_object);
static void get_contact_info_for_members_cb (TpConnection *conn, 
    GHashTable *out_contactinfo, const GError *error, gpointer user_data, 
    GObject *weak_object);

GQuark
e_book_backend_tp_cl_error (void)
{
  return g_quark_from_static_string ("e-book-backend-tp-cl-error-quark");
}

static void
e_book_backend_tp_cl_get_property (GObject *object, guint property_id,
                              GValue *value, GParamSpec *pspec)
{
  switch (property_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
e_book_backend_tp_cl_set_property (GObject *object, guint property_id,
                              const GValue *value, GParamSpec *pspec)
{
  switch (property_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
free_channels_and_connection (EBookBackendTpCl *tpcl)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  gint i;

  for (i = 0; i < CL_LAST_LIST; i++)
  {
    EBookBackendTpClContactList *list = priv->contact_list_channels[i];
    priv->contact_list_channels[i] = NULL;
    if (list) {
      g_object_unref (list->channel);
      g_free (list);
    }
  }

  if (priv->conn) {
    g_object_unref (priv->conn);
    priv->conn = NULL;
  }
}

static void
e_book_backend_tp_cl_dispose (GObject *object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (object);
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);

  if (priv->account)
  {
    g_object_unref (priv->account);
    priv->account = NULL;
  }

  free_channels_and_connection (tpcl);

  if (G_OBJECT_CLASS (e_book_backend_tp_cl_parent_class)->dispose)
    G_OBJECT_CLASS (e_book_backend_tp_cl_parent_class)->dispose (object);
}

static void
e_book_backend_tp_cl_finalize (GObject *object)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (object);

  g_hash_table_unref (priv->contacts_hash);

  if (priv->account)
    g_signal_handlers_disconnect_by_func (priv->account,
        G_CALLBACK (connection_status_changed_cb), object);
  
  if (G_OBJECT_CLASS (e_book_backend_tp_cl_parent_class)->finalize)
    G_OBJECT_CLASS (e_book_backend_tp_cl_parent_class)->finalize (object);
}

static void
e_book_backend_tp_cl_class_init (EBookBackendTpClClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (EBookBackendTpClPrivate));

  object_class->get_property = e_book_backend_tp_cl_get_property;
  object_class->set_property = e_book_backend_tp_cl_set_property;
  object_class->dispose = e_book_backend_tp_cl_dispose;
  object_class->finalize = e_book_backend_tp_cl_finalize;

  /* This should be g_cclosure_marshal_VOID__ENUM but we don't have the
   * generated GType for the enums.
   * TODO: change this to be an enum. */
  signals[STATUS_CHANGED] = g_signal_new ("status-changed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, status_changed),
        NULL, NULL,
        g_cclosure_marshal_VOID__INT,
        G_TYPE_NONE, 
        1, G_TYPE_INT);

  signals[CONTACTS_ADDED] = g_signal_new ("contacts-added",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, contacts_added),
        NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 
        1, G_TYPE_POINTER);

  signals[CONTACTS_REMOVED] = g_signal_new ("contacts-removed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, contacts_removed),
        NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 
        1, G_TYPE_POINTER);

  signals[FLAGS_CHANGED] = g_signal_new ("flags-changed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, flags_changed),
        NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 
        1, G_TYPE_POINTER);

  signals[ALIASES_CHANGED] = g_signal_new ("aliases-changed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, aliases_changed),
        NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 
        1, G_TYPE_POINTER);

  signals[PRESENCES_CHANGED] = g_signal_new ("presences-changed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, presences_changed),
        NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 
        1, G_TYPE_POINTER);

  signals[AVATAR_TOKENS_CHANGED] = g_signal_new ("avatar-tokens-changed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (EBookBackendTpClClass, avatar_tokens_changed),
      NULL, NULL,
      g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE,
      1, G_TYPE_POINTER);

  signals[AVATAR_DATA_CHANGED] = g_signal_new ("avatar-data-changed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (EBookBackendTpClClass, avatar_data_changed),
        NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 
        1, G_TYPE_POINTER);

  signals[CAPABILITIES_CHANGED] = g_signal_new ("capabilities-changed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (EBookBackendTpClClass, capabilities_changed),
      NULL, NULL,
      g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE,
      1, G_TYPE_POINTER);
  
  signals[CONTACT_INFO_CHANGED] = g_signal_new ("contact-info-changed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (EBookBackendTpClClass, contact_info_changed),
      NULL, NULL,
      g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE,
      1, G_TYPE_POINTER);
  
  /* FIXME: For ContactInfo interface */
  rtcom_tp_cli_init ();
}

static void
e_book_backend_tp_cl_init (EBookBackendTpCl *self)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (self);

  priv->contacts_hash = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify)e_book_backend_tp_contact_unref);
}

EBookBackendTpCl *
e_book_backend_tp_cl_new (void)
{
  return g_object_new (E_TYPE_BOOK_BACKEND_TP_CL, NULL);
}

static gboolean
verify_is_connected (EBookBackendTpCl *self, GError **error)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (self);

  if (priv->conn)
    return TRUE;
  else
  {
    WARNING ("Disconnected while executing operation");
    g_set_error (error, E_BOOK_BACKEND_TP_CL_ERROR,
        E_BOOK_BACKEND_TP_CL_ERROR_FAILED,
        "disconnected while executing operation");
    return FALSE;
  }
}

static const gchar *
contact_list_id_to_string (EBookBackendTpContactListId list_id)
{
  switch (list_id)
  {
    case CL_SUBSCRIBE:
      return "subscribe";
    case CL_SUBSCRIBE_LOCAL_PENDING:
      return "subscribe:local_pending";
    case CL_SUBSCRIBE_REMOTE_PENDING:
      return "subscribe:remote_pending";

    case CL_PUBLISH:
      return "publish";
    case CL_PUBLISH_LOCAL_PENDING:
      return "publish:local_pending";
    case CL_PUBLISH_REMOTE_PENDING:
      return "publish:remote_pending";
      
    case CL_ALLOW:
      return "allow";
    case CL_ALLOW_LOCAL_PENDING:
      return "allow:local_pending";
    case CL_ALLOW_REMOTE_PENDING:
      return "allow:remote_pending";

    case CL_DENY:
      return "deny";
    case CL_DENY_LOCAL_PENDING:
      return "deny:local_pending";
    case CL_DENY_REMOTE_PENDING:
      return "deny:remote_pending";

    case CL_STORED:
      return "stored";
    case CL_STORED_LOCAL_PENDING:
      return "stored:local_pending";
    case CL_STORED_REMOTE_PENDING:
      return "stored:remote_pending";

    default:
      return "unknown";
  }
}

typedef struct
{
  EBookBackendTpCl *tpcl;
  GArray *contacts_to_add;
  GArray *contacts_to_update;
  GArray *contacts_to_remove;
  GArray *handles_to_inspect;
} ChannelMembersChangedClosure;


static const gchar *
presence_code_to_string (TpConnectionPresenceType type)
{
  switch (type)
  {
    case TP_CONNECTION_PRESENCE_TYPE_UNSET:
      return "unset";
    case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
      return "offline";
    case TP_CONNECTION_PRESENCE_TYPE_AVAILABLE:
      return "available";
    case TP_CONNECTION_PRESENCE_TYPE_AWAY:
      return "away";
    case TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY:
      return "xa";
    case TP_CONNECTION_PRESENCE_TYPE_HIDDEN:
      return "hidden";
    case TP_CONNECTION_PRESENCE_TYPE_BUSY:
      return "busy";
    case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
      return "unknown";
    case TP_CONNECTION_PRESENCE_TYPE_ERROR:
      return "error";
    default:
      return "error";
  }
}

/* Update the contact details from the passed TpContacts but *without*
 * emitting any signal */
static GArray *
update_contact_details (EBookBackendTpCl *tpcl, guint n_tp_contacts,
    TpContact * const *tp_contacts)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GArray *contacts;
  guint i;
  TpContact *tp_contact;
  TpHandle handle;
  EBookBackendTpContact *contact;
  const gchar *avatar_token;

  DEBUG ("contacts retrieved");

  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  for (i = 0; i < n_tp_contacts; i++)
  {
    tp_contact = tp_contacts[i];
    handle = tp_contact_get_handle (tp_contact);
    contact = g_hash_table_lookup (priv->contacts_hash,
        GUINT_TO_POINTER (handle));

    if (!contact)
    {
      /* This can happen if the contact is gone but the inspection
       * of the handle succeeded */
      WARNING ("failed looking up contact with handle %d to "
          "assign it contact information", handle);
      continue;
    }

    e_book_backend_tp_contact_update_name  (contact,
        tp_contact_get_identifier (tp_contact));

    g_free (contact->alias);
    g_free (contact->status);
    g_free (contact->status_message);

    contact->alias = g_strdup (tp_contact_get_alias (tp_contact));

    contact->generic_status = presence_code_to_string (
        tp_contact_get_presence_type (tp_contact));
    contact->status = g_strdup (tp_contact_get_presence_status (tp_contact));
    contact->status_message = g_strdup (tp_contact_get_presence_message (
          tp_contact));

    avatar_token = tp_contact_get_avatar_token (tp_contact);
    if (tp_strdiff (avatar_token, contact->avatar_token))
    {
      g_free (contact->avatar_token);
      contact->avatar_token = g_strdup (avatar_token);
    }

    g_array_append_val (contacts, contact);

    DEBUG ("got name: %s; alias: %s; status: %s; avatar_token: %s",
        contact->name, contact->alias, contact->generic_status,
        contact->avatar_token);
  }

  return contacts;
}

static void
free_contacts_array (GArray *array)
{
  guint i;

  for (i = 0; i < array->len; i++)
  {
    EBookBackendTpContact *contact;

    contact = g_array_index (array, EBookBackendTpContact *, i);
    e_book_backend_tp_contact_unref (contact);
  }

  g_array_free (array, TRUE);
}

static void
channel_members_changed_closure_free (ChannelMembersChangedClosure *closure)
{
  if (!closure)
    return;

  free_contacts_array (closure->contacts_to_add);
  free_contacts_array (closure->contacts_to_update);
  free_contacts_array (closure->contacts_to_remove);
  g_array_free (closure->handles_to_inspect, TRUE);
  g_object_unref (closure->tpcl);
  g_free (closure);
}

static void
update_changed_members (ChannelMembersChangedClosure *closure,
    GArray *updated_contacts)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (closure->tpcl);
  guint i = 0;
  EBookBackendTpContact *contact = NULL;

  /* Notify of the changes in the contact list */
  if (closure->contacts_to_add->len > 0)
    g_signal_emit (closure->tpcl, signals[CONTACTS_ADDED], 0,
        closure->contacts_to_add);

  if (closure->contacts_to_remove->len > 0)
    g_signal_emit (closure->tpcl, signals[CONTACTS_REMOVED], 0,
        closure->contacts_to_remove);

  if (closure->contacts_to_update->len > 0)
    g_signal_emit (closure->tpcl, signals[FLAGS_CHANGED], 0,
        closure->contacts_to_update);

  /* Actually remove our removed contacts from the hash table */
  for (i = 0; i < closure->contacts_to_remove->len; i++)
  {
    contact = g_array_index (closure->contacts_to_remove, EBookBackendTpContact *, i);
    g_hash_table_remove (priv->contacts_hash, GINT_TO_POINTER (contact->handle));
  }

  /* Now notify of the changes in the contacts */
  if (updated_contacts && updated_contacts->len > 0)
  {
    g_signal_emit (closure->tpcl, signals[ALIASES_CHANGED], 0, updated_contacts);
    g_signal_emit (closure->tpcl, signals[AVATAR_TOKENS_CHANGED], 0, updated_contacts);
    g_signal_emit (closure->tpcl, signals[PRESENCES_CHANGED], 0, updated_contacts);
  }

  channel_members_changed_closure_free (closure);
}

static void
remove_invalid_contacts (EBookBackendTpCl *tpcl, GArray *array)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  gint i;
  EBookBackendTpContact *contact;

  for (i = (gint) array->len - 1; i >= 0; i--)
  {
    contact = g_array_index (array, EBookBackendTpContact *, i);
    /* contact->name is NULL if the inspection failed, the contact is not in
     * the hash table if it was already inspected but then removed */
    if (!contact->name ||
        !g_hash_table_lookup (priv->contacts_hash,
          GUINT_TO_POINTER (contact->handle))) {
        g_array_remove_index_fast (array, i);
        e_book_backend_tp_contact_unref (contact);
    }
  }
}

static void get_capabilities_for_members_cb (TpConnection *conn,
    const GPtrArray *caps, const GError *error, gpointer userdata, 
    GObject *weak_object);

/* Inspect the features that are still drafts and are not yet supported by
 * tp_connection_get_contacts_by_handle. We have to do this after we retrieved
 * the TpContacts to avoid having the capabilities or the contact info 
 * information before having the contact ID. */
static void
inspect_additional_features (EBookBackendTpCl *tpcl, GArray *contacts)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GArray *handles;
  gint i;

  handles = g_array_sized_new (TRUE, TRUE, sizeof (TpHandle), contacts->len);
  for (i = 0; i < contacts->len; i++)
  {
    EBookBackendTpContact *contact;

    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    g_array_append_val (handles, contact->handle);
  }

  DEBUG ("getting capabilities for all members");
  /* FIXME: ContactCapabilities is still a draft so it's not yet supported by
   * tp-glib. Switch to use it when it's ready */
  tp_cli_connection_interface_capabilities_call_get_capabilities (priv->conn,
      -1,
      handles,
      get_capabilities_for_members_cb,
      NULL,
      NULL,
      (GObject *)tpcl);

  /* FIXME: ContactInfo is still a draft. Switch to use tp-glib API when
   * it's ready */
  if (tp_proxy_has_interface_by_id (priv->conn, 
        RTCOM_TP_IFACE_QUARK_CONNECTION_INTERFACE_CONTACT_INFO))
  {
    DEBUG ("getting contact info for all members");
    rtcom_tp_cli_connection_interface_contact_info_call_get_contact_info (
        priv->conn,
        -1,
        handles,
        get_contact_info_for_members_cb,
        NULL,
        NULL,
        (GObject *)tpcl);
  }
  else
  {
    DEBUG ("connection doesn't support ContactInfo interface");
  }

  g_array_free (handles, TRUE);
}

static void
members_changed_with_details_cb (TpConnection *connection,
    guint n_tp_contacts, TpContact * const *tp_contacts, guint n_failed,
    const TpHandle *failed, const GError *error, gpointer userdata,
    GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  ChannelMembersChangedClosure *closure = userdata;
  GArray *contacts;

  DEBUG ("contacts retrieved");

  if (error)
  {
    WARNING ("error when getting contacts: %s", error->message);
    channel_members_changed_closure_free (closure);
    return;
  }

  if (!verify_is_connected (tpcl, NULL))
  {
    channel_members_changed_closure_free (closure);
    return;
  }

  contacts = update_contact_details (tpcl, n_tp_contacts, tp_contacts);

  /* We have to remove invalid contacts from the GArrays before emitting any
   * signal. Contacts at this point can be invalid for two reasons:
   * - The inspection failed as the contact is already gone (in this case the
   *   handle is also in the failed array)
   * - The inspection didn't fail, but in the meantime the contact is gone
   *   and is already removed from our hash table mapping from handles to
   *   EBookBackendTpContacts */
  remove_invalid_contacts (tpcl, closure->contacts_to_add);
  remove_invalid_contacts (tpcl, closure->contacts_to_remove);
  remove_invalid_contacts (tpcl, closure->contacts_to_update);

  update_changed_members (closure, contacts);

  inspect_additional_features (tpcl, contacts);

  g_array_free (contacts, TRUE);
}

static gboolean
handle_members_changed_idle_cb (gpointer userdata)
{
  ChannelMembersChangedClosure *closure = userdata;

  if (!verify_is_connected (closure->tpcl, NULL))
  {
    channel_members_changed_closure_free (closure);
    return FALSE;
  }

  update_changed_members (closure, NULL);

  return FALSE;
}

#define CLEAR_LIST_FLAGS(contact, list_id) \
  contact->flags &= ~CONTACT_FLAG_FROM_ID (list_id);\
  contact->flags &= \
    ~CONTACT_FLAG_FROM_ID(CONTACT_LIST_ID_GET_LOCAL_FROM_CURRENT (list_id)); \
  contact->flags &= \
    ~CONTACT_FLAG_FROM_ID(CONTACT_LIST_ID_GET_REMOTE_FROM_CURRENT (list_id));

static void
tp_channel_members_changed_cb (TpChannel *channel, const gchar *message, 
    const GArray *added, const GArray *removed, 
    const GArray *local_pending, const GArray *remote_pending,
    guint actor, guint reason, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContactListId list_id = GPOINTER_TO_INT (userdata);
  guint i = 0;
  EBookBackendTpContact *contact = NULL;
  TpHandle handle;
  ChannelMembersChangedClosure *closure = NULL;

  if (!verify_is_connected (tpcl, NULL))
    return;

  closure = g_new0 (ChannelMembersChangedClosure, 1);

  closure->tpcl = g_object_ref (tpcl);

  closure->contacts_to_add = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpCl *));
  closure->contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpCl *));
  closure->contacts_to_remove = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpCl *));
  closure->handles_to_inspect = g_array_new (TRUE, TRUE, sizeof (TpHandle));

  for (i = 0; i < added->len; i++)
  {
    handle = g_array_index (added, TpHandle, i);
    contact = g_hash_table_lookup (priv->contacts_hash, GINT_TO_POINTER (handle));

    if (contact)
    {
      /* Okay. So this is an existing contact. So we probably just need to set
       * the appropriate flag. And mark it to be updated.
       */

      DEBUG ("existing contact found %s for adding to %s",
          contact->name, contact_list_id_to_string (list_id));

      /* clear any old pending flags */
      CLEAR_LIST_FLAGS (contact, list_id);
      e_book_backend_tp_contact_ref (contact);
      g_array_append_val (closure->contacts_to_update, contact);
    } else {
      DEBUG ("new contact for adding to %s",
         contact_list_id_to_string (list_id));
      contact = e_book_backend_tp_contact_new ();
      contact->handle = handle;
      g_array_append_val (closure->contacts_to_add, contact);
      g_array_append_val (closure->handles_to_inspect, handle);
    }
    contact->flags |= CONTACT_FLAG_FROM_ID (list_id);
  }

  for (i = 0; i < removed->len; i++)
  {
    handle = g_array_index (removed, TpHandle, i);
    contact = g_hash_table_lookup (priv->contacts_hash, GINT_TO_POINTER (handle));

    if (contact)
    {
      EBookBackendTpContactFlag flags_to_remove;

      DEBUG ("existing contact found %s for removal from %s",
          contact->name, contact_list_id_to_string (list_id));

      /* MembersChanged is emitted only for the main lists (listed in
       * EBookBackendTpPrimaryContactListId). This means that if a
       * contact is removed from "publish:local-pending" then list_id
       * will be just "publish", so we have to remove the flag for all the
       * variants of "publish". */
      flags_to_remove = CONTACT_FLAG_FROM_ID (list_id) |
                        CONTACT_FLAG_FROM_ID (list_id + 1) |
                        CONTACT_FLAG_FROM_ID (list_id + 2);
      contact->flags &= ~flags_to_remove;

      e_book_backend_tp_contact_ref (contact);
      g_array_append_val (closure->contacts_to_update, contact);

      if (contact->flags == 0)
      {
        DEBUG ("contact with name %s has no flags. removing.", 
            contact->name);
        e_book_backend_tp_contact_ref (contact);
        g_array_append_val (closure->contacts_to_remove, contact);
      }
    } else {
      WARNING ("told about the removal of unknown contact");
    }
  }

  for (i = 0; i < local_pending->len; i++)
  {
    handle = g_array_index (local_pending, TpHandle, i);
    contact = g_hash_table_lookup (priv->contacts_hash, GINT_TO_POINTER (handle));

    if (contact)
    {
      /* clear any old flags */
      CLEAR_LIST_FLAGS (contact, list_id);
      contact->flags |= 
        CONTACT_FLAG_FROM_ID(CONTACT_LIST_ID_GET_LOCAL_FROM_CURRENT (list_id));

      DEBUG ("existing contact %s has in local-pending for %s",
          contact->name, contact_list_id_to_string (list_id));
      e_book_backend_tp_contact_ref (contact);
      g_array_append_val (closure->contacts_to_update, contact);
    } else {
      DEBUG ("new local-pending contact for adding to %s",
         contact_list_id_to_string (list_id));
      contact = e_book_backend_tp_contact_new ();
      contact->handle = handle;
      g_array_append_val (closure->contacts_to_add, contact);
      g_array_append_val (closure->handles_to_inspect, handle);
    }

    contact->flags |= 
      CONTACT_FLAG_FROM_ID(CONTACT_LIST_ID_GET_LOCAL_FROM_CURRENT (list_id));
  }

  for (i = 0; i < remote_pending->len; i++)
  {
    handle = g_array_index (remote_pending, TpHandle, i);
    contact = g_hash_table_lookup (priv->contacts_hash, GINT_TO_POINTER (handle));

    if (contact)
    {
      /* clear any old flags */
      CLEAR_LIST_FLAGS (contact, list_id);
      contact->flags |= 
        CONTACT_FLAG_FROM_ID(CONTACT_LIST_ID_GET_REMOTE_FROM_CURRENT (list_id));

      DEBUG ("existing contact %s has in remote-pending for %s",
          contact->name, contact_list_id_to_string (list_id));
      e_book_backend_tp_contact_ref (contact);
      g_array_append_val (closure->contacts_to_update, contact);
    } else {
      DEBUG ("new remote-pending contact for adding to %s",
         contact_list_id_to_string (list_id));
      contact = e_book_backend_tp_contact_new ();
      contact->handle = handle;
      g_array_append_val (closure->contacts_to_add, contact);
      g_array_append_val (closure->handles_to_inspect, handle);
    }

    contact->flags |= 
      CONTACT_FLAG_FROM_ID(CONTACT_LIST_ID_GET_REMOTE_FROM_CURRENT (list_id));
  }

  /* So for our newly added contacts we need to add them to the hash table */
  for (i = 0; i < closure->contacts_to_add->len; i++)
  {
    contact = g_array_index (closure->contacts_to_add, EBookBackendTpContact *, i);
    g_hash_table_insert (priv->contacts_hash, 
        GINT_TO_POINTER (contact->handle), 
        e_book_backend_tp_contact_ref (contact));
  }

  if (closure->handles_to_inspect->len > 0)
  {
    /* We need to have the contact names before emitting any signal, so we
     * retrieve it (together with the other info) and then call
     * update_changed_members */
    const TpContactFeature features[] = {TP_CONTACT_FEATURE_ALIAS,
        TP_CONTACT_FEATURE_AVATAR_TOKEN, TP_CONTACT_FEATURE_PRESENCE};

    DEBUG ("getting contact alias, avatar and presence for added members");
    tp_connection_get_contacts_by_handle (priv->conn,
        closure->handles_to_inspect->len,
        (TpHandle *)closure->handles_to_inspect->data,
        G_N_ELEMENTS (features), features,
        members_changed_with_details_cb, closure, NULL, G_OBJECT (tpcl));
  }
  else
    /* Must do this work in an idle since various signal bits are synchronous
     * and we don't want to clog up the mainloop
     */
    g_idle_add (handle_members_changed_idle_cb, closure);
}

typedef struct
{
  EBookBackendTpCl *tpcl;
  EBookBackendTpContactListId list_id;
} ChannelReadyClosure;

static void
tp_channel_ready_cb (TpChannel *channel, const GError *error, gpointer userdata)
{
  ChannelReadyClosure *closure = (ChannelReadyClosure *)userdata;
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (closure->tpcl);
  EBookBackendTpContactListId list_id = closure->list_id;
  GError *error_connect = NULL;

  if (error)
  {
    WARNING ("error when getting channel %s ready: %s",
        contact_list_id_to_string (list_id), error->message);
  } else if (verify_is_connected (tpcl, NULL)) {
    DEBUG ("channel %s ready", 
        contact_list_id_to_string (list_id));

    g_assert (closure);

    if (list_id == CL_SUBSCRIBE)
    {
      /* If the subscribe channel is ready lets claim that we're online now */
      e_book_backend_tp_cl_set_status (tpcl, E_BOOK_BACKEND_TP_CL_ONLINE);
    }

    tp_cli_channel_interface_group_connect_to_members_changed (
        channel,
        tp_channel_members_changed_cb,
        GINT_TO_POINTER (closure->list_id),
        NULL,
        (GObject *)tpcl,
        &error_connect);

    if (error_connect)
    {
      WARNING ("Failed to connect to MembersChanged signal");

      g_clear_error (&error_connect);
    }
  }

  g_object_unref (closure->tpcl);
  g_free (closure);
}

static void
tp_request_channel_cb (TpConnection *conn, const gchar *object_path,
    const GError *error_in, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  EBookBackendTpClContactList *backend_channel = NULL;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContactListId list_id =
      (EBookBackendTpContactListId)GPOINTER_TO_INT (userdata);
  GError *error = NULL;
  ChannelReadyClosure *closure = NULL;

  if (error_in)
  {
    DEBUG ("requesting channel for %s failed: %s",
        contact_list_id_to_string (list_id), error_in->message);
  } else if (verify_is_connected (tpcl, NULL)) {
    if (object_path)
    {
      backend_channel = g_new0 (EBookBackendTpClContactList, 1);
      priv->contact_list_channels[list_id] = backend_channel;

      backend_channel->channel = tp_channel_new (priv->conn,
          object_path, TP_IFACE_CHANNEL_TYPE_CONTACT_LIST,
          TP_HANDLE_TYPE_LIST, 0, &error);

      if (!backend_channel->channel)
      {
        WARNING ("error when creating channel for %s: %s",
          contact_list_id_to_string (list_id),
          error ? error->message : "unknown error");
        g_clear_error (&error);
      } else {
        closure = g_new0 (ChannelReadyClosure, 1);
        closure->tpcl = g_object_ref (tpcl);
        closure->list_id = list_id;
        tp_channel_call_when_ready (backend_channel->channel,
            tp_channel_ready_cb, closure);
      }
    }
  }
}

static void
request_handles_for_contact_list_cb (TpConnection *conn, const GArray *handles, 
    const GError *error, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContactListId list_id =
      (EBookBackendTpContactListId)GPOINTER_TO_INT (userdata);
  TpHandle handle;

  if (error)
  {
    DEBUG ("cannot retrieve handle for %s: %s",
        contact_list_id_to_string (list_id), error->message);
  } else if (verify_is_connected (tpcl, NULL)) {
    if (handles)
    {
      /* we should only have one thing here */
      handle = g_array_index (handles, TpHandle, 0);

      DEBUG ("got handle for %s",
          contact_list_id_to_string (list_id));

      DEBUG ("requesting channel for %d", handle);

      /* If the CM has to do a lot of work to get the contact list then
       * the RequestChannel call could timeout, so we use G_MAXINT
       * (meaning infinite timeout) */
      tp_cli_connection_call_request_channel (priv->conn,
          G_MAXINT,
          TP_IFACE_CHANNEL_TYPE_CONTACT_LIST,
          TP_HANDLE_TYPE_LIST,
          handle,
          TRUE,
          tp_request_channel_cb,
          GINT_TO_POINTER (list_id),
          NULL,
          (GObject *)tpcl);
    }
  }
}


static void capabilities_changed_cb (TpConnection *conn, const GPtrArray *caps, 
    gpointer userdata, GObject *weak_object);

static void
aliases_changed_cb (TpConnection *conn, const GPtrArray *new_aliases, 
    gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  guint i = 0;
  EBookBackendTpContact *contact = NULL;
  GArray *contacts_changed = NULL;

  g_assert (new_aliases);

  if (!verify_is_connected (tpcl, NULL))
    return;

  contacts_changed = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  for (i = 0; i < new_aliases->len; i++)
  {
    GValueArray *alias_pair = NULL;
    GValue *value = NULL;
    TpHandle contact_handle;
    const gchar *new_alias = NULL;

    alias_pair = g_ptr_array_index (new_aliases, i);

    value = g_value_array_get_nth (alias_pair, 0);
    contact_handle = g_value_get_uint (value);

    value = g_value_array_get_nth (alias_pair, 1);
    new_alias = g_value_get_string (value);
    
    contact = g_hash_table_lookup (priv->contacts_hash,
        GUINT_TO_POINTER (contact_handle));

    if (contact)
    {
      g_free (contact->alias);
      contact->alias = g_strdup (new_alias);

      g_array_append_val (contacts_changed, contact);
    } else {
      WARNING ("mismatched contact and alias");
    }
  }

  if (contacts_changed->len >= 1)
  {
    g_signal_emit (tpcl, signals[ALIASES_CHANGED], 0, contacts_changed);
  }

  g_array_free (contacts_changed, TRUE);
}

static void
avatar_retrieved_cb (TpConnection *conn, TpHandle contact_handle,
    const gchar *new_avatar_token, const GArray *new_avatar_data,
    const gchar *new_avatar_mime, gpointer userdata,
    GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContact *contact = NULL;

  if (!verify_is_connected (tpcl, NULL))
    return;

  contact = g_hash_table_lookup (priv->contacts_hash,
      GUINT_TO_POINTER (contact_handle));
  if (contact)
  {
    DEBUG ("got new avatar data; len: %d, MIME type: %s",
        new_avatar_data->len, new_avatar_mime);

    contact->avatar_len = new_avatar_data->len;

    g_free (contact->avatar_data);
    contact->avatar_data = g_memdup (new_avatar_data->data,
        contact->avatar_len);

    g_free (contact->avatar_mime);
    contact->avatar_mime = g_strdup (new_avatar_mime);

    g_free (contact->avatar_token);
    contact->avatar_token = g_strdup (new_avatar_token);

    g_signal_emit (tpcl, signals[AVATAR_DATA_CHANGED], 0, contact);
  } else {
    WARNING ("got AvatarRetrieved for contact we don't know about "
               "(handle: %d", contact_handle);
  }
}

static void
avatar_updated_cb (TpConnection *conn, guint contact_handle,
    const gchar *new_avatar_token, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContact *contact = NULL;
  GArray *contacts = NULL;

  if (!verify_is_connected (tpcl, NULL))
    return;

  contact = g_hash_table_lookup (priv->contacts_hash,
      GUINT_TO_POINTER (contact_handle));
  if (contact)
  {
    if (!contact->avatar_token
        || !(g_str_equal (contact->avatar_token, new_avatar_token)))
    {
      DEBUG ("got new avatar token: '%s'",
          new_avatar_token);
      contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
      g_free (contact->avatar_token);
      contact->avatar_token = g_strdup (new_avatar_token);
      g_array_append_val (contacts, contact);
      g_signal_emit (tpcl, signals[AVATAR_TOKENS_CHANGED], 0, contacts);
      g_array_free (contacts, TRUE);
    }
  } else {
    WARNING ("got AvatarUpdated for contact we don't know about "
               "(handle: %d)", contact_handle);
  }
}

static void
presences_changed_cb (TpConnection *conn, GHashTable *presences, 
    gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GList *handles = NULL;
  GList *l = NULL;
  EBookBackendTpContact *contact = NULL;
  GArray *contacts_changed = NULL;
  GValueArray *values = NULL;
  TpHandle handle;
  const GValue *v;

  if (!verify_is_connected (tpcl, NULL))
    return;

  contacts_changed = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  handles = g_hash_table_get_keys (presences);

  for (l = handles; l; l = g_list_next (l))
  {
    handle = (TpHandle) GPOINTER_TO_UINT (l->data);

    values = (GValueArray *)g_hash_table_lookup (presences,
        GUINT_TO_POINTER (handle));
    contact = g_hash_table_lookup (priv->contacts_hash,
        GUINT_TO_POINTER (handle));

    if (values && contact)
    {
      v = g_value_array_get_nth (values, 0);
      contact->generic_status = presence_code_to_string (g_value_get_uint (v));

      g_free (contact->status);
      v = g_value_array_get_nth (values, 1);
      contact->status = g_value_dup_string (v);

      g_free (contact->status_message);
      v = g_value_array_get_nth (values, 2);
      contact->status_message = g_value_dup_string (v);

      g_array_append_val (contacts_changed, contact);
    } else {
      WARNING ("mismatched contact and presence");
    }
  }

  if (contacts_changed->len >= 1)
  {
    g_signal_emit (tpcl, signals[PRESENCES_CHANGED], 0, contacts_changed);
  }

  g_array_free (contacts_changed, TRUE);

  g_list_free (handles);
}

static void
get_contact_list_channels (EBookBackendTpCl *tpcl)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  const gchar *tmp[2] = {NULL, NULL};

  /* First request the subscribe list */
  tmp[0] = contact_list_id_to_string (CL_SUBSCRIBE);
  tp_cli_connection_call_request_handles (priv->conn,
      -1,
      TP_HANDLE_TYPE_LIST,
      tmp,
      request_handles_for_contact_list_cb,
      GINT_TO_POINTER (CL_SUBSCRIBE),
      NULL,
      (GObject *)tpcl);

  /* First request the publish list */
  tmp[0] = contact_list_id_to_string (CL_PUBLISH);
  tp_cli_connection_call_request_handles (priv->conn,
      -1,
      TP_HANDLE_TYPE_LIST,
      tmp,
      request_handles_for_contact_list_cb,
      GINT_TO_POINTER (CL_PUBLISH),
      NULL,
      (GObject *)tpcl);

  /* First request the stored list */
  tmp[0] = contact_list_id_to_string (CL_STORED);
  tp_cli_connection_call_request_handles (priv->conn,
      -1,
      TP_HANDLE_TYPE_LIST,
      tmp,
      request_handles_for_contact_list_cb,
      GINT_TO_POINTER (CL_STORED),
      NULL,
      (GObject *)tpcl);

  /* First request the deny list */
  tmp[0] = contact_list_id_to_string (CL_ALLOW);
  tp_cli_connection_call_request_handles (priv->conn,
      -1,
      TP_HANDLE_TYPE_LIST,
      tmp,
      request_handles_for_contact_list_cb,
      GINT_TO_POINTER (CL_ALLOW),
      NULL,
      (GObject *)tpcl);

  /* First request the deny list */
  tmp[0] = contact_list_id_to_string (CL_DENY);
  tp_cli_connection_call_request_handles (priv->conn,
      -1,
      TP_HANDLE_TYPE_LIST,
      tmp,
      request_handles_for_contact_list_cb,
      GINT_TO_POINTER (CL_DENY),
      NULL,
      (GObject *)tpcl);
}

static void
tp_connection_ready_cb (TpConnection *conn, const GError *error, gpointer userdata)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (userdata);
  EBookBackendTpClPrivate *priv = NULL;
  GError *error_connect = NULL;

  priv = GET_PRIVATE (tpcl);

  if (error)
  {
    WARNING ("Error when getting connection: %s", error->message);
  } else if (verify_is_connected (tpcl, NULL)) {
    DEBUG ("Telepathy connection for %s ready", priv->account_name);

    /* set up signal handlers */

    tp_cli_connection_interface_aliasing_connect_to_aliases_changed (
        priv->conn,
        aliases_changed_cb,
        NULL,
        NULL,
        (GObject *)tpcl,
        &error_connect);

    if (error_connect)
    {
      WARNING ("Failed to connect to AliasesChanged signal");

      g_clear_error (&error_connect);
    }

    tp_cli_connection_interface_simple_presence_connect_to_presences_changed (
        priv->conn,
        presences_changed_cb,
        NULL,
        NULL,
        (GObject *)tpcl,
        &error_connect);

    if (error_connect)
    {
      WARNING ("Failed to connect to PresencesChanged signal");

      g_clear_error (&error_connect);
    }

    tp_cli_connection_interface_avatars_connect_to_avatar_updated (
        priv->conn,
        avatar_updated_cb,
        NULL,
        NULL,
        (GObject *)tpcl,
        &error_connect);

    if (error_connect)
    {
      WARNING ("Failed to connect to AvatarUpdated signal");

      g_clear_error (&error_connect);
    }

    tp_cli_connection_interface_avatars_connect_to_avatar_retrieved (
        priv->conn,
        avatar_retrieved_cb,
        NULL,
        NULL,
        (GObject *)tpcl,
        &error_connect);

    if (error_connect)
    {
      WARNING ("Failed to connect to AvatarRetrieved signal");

      g_clear_error (&error_connect);
    }

    tp_cli_connection_interface_capabilities_connect_to_capabilities_changed (
        priv->conn,
        capabilities_changed_cb,
        NULL,
        NULL,
        (GObject *)tpcl,
        &error_connect);

    if (error_connect)
    {
      WARNING ("Failed to connect to CapabilitiesChanged signal");

      g_clear_error (&error_connect);
    }

    /* FIXME: ContactInfo is still a draft. Switch to use tp-glib API when it's ready */
    if (tp_proxy_has_interface_by_id (priv->conn, 
        RTCOM_TP_IFACE_QUARK_CONNECTION_INTERFACE_CONTACT_INFO)) 
    {
      rtcom_tp_cli_connection_interface_contact_info_connect_to_contactinfochanged (
          priv->conn,
          contact_info_changed_cb,
          NULL,
          NULL,
          (GObject *)tpcl,
          &error_connect);
      
      if (error_connect)
      {
        WARNING ("Failed to connect to ContactInfoChanged signal");

        g_clear_error (&error_connect);
      }
    } else {
      DEBUG ("connection not support ContactInfo interface");
    }

    get_contact_list_channels (tpcl);
  }

  g_object_unref (tpcl);
}

static void
_setup_tp_connection (EBookBackendTpCl *tpcl)
{
  EBookBackendTpClPrivate *priv = NULL;
  GError *error = NULL;

  priv = GET_PRIVATE (tpcl);

  if (!priv->conn)
  {
    const gchar *connection_path;
    DBusGConnection *bus;
    TpDBusDaemon *dbus_daemon;
   
    bus = tp_get_bus ();
    dbus_daemon = tp_dbus_daemon_new (bus);
    connection_path = mc_account_get_connection_path (priv->account);
    priv->conn = tp_connection_new (dbus_daemon, NULL, connection_path,
        &error);
    g_object_unref (dbus_daemon);

    if (priv->conn)
    {
      tp_connection_call_when_ready (priv->conn, 
        (TpConnectionWhenReadyCb)tp_connection_ready_cb, g_object_ref (tpcl));
    } else {
      WARNING ("Error when getting connection: %s",
          error ? error->message : "unknown");
      g_clear_error (&error);
    }
  }
}

static void
connection_status_changed_cb (McAccount *account, TpConnectionStatus status,
    TpConnectionStatusReason reason, gpointer userdata)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (userdata);

  DEBUG ("status changed to %d for reason %d", status, reason);

  /* Statuses other than CONNECTED are offline for us */
  if (status == TP_CONNECTION_STATUS_CONNECTED)
  {
    _setup_tp_connection (tpcl);
  } else {
    e_book_backend_tp_cl_set_status (tpcl, E_BOOK_BACKEND_TP_CL_OFFLINE);
    /* contact lists are no longer valid since we have been disconnected, so free them */
    free_channels_and_connection (tpcl);
  }
}

static void
account_ready_cb (McAccount *account, const GError *error, gpointer userdata)
{
  EBookBackendTpCl *tpcl = userdata;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl); 
  TpConnectionStatus connection_status;

  if (error)
  {
    WARNING ("Error when waiting for an account to be ready: %s",
        error->message);
    g_object_unref (tpcl);
    return;
  }

  g_assert (account == priv->account);

  connection_status = mc_account_get_connection_status (account);
  if (connection_status == TP_CONNECTION_STATUS_CONNECTED)
    _setup_tp_connection (tpcl);

  g_signal_connect (account, "connection-status-changed",
      G_CALLBACK (connection_status_changed_cb), tpcl);

  g_object_unref (tpcl);
}

gboolean
e_book_backend_tp_cl_load (EBookBackendTpCl *tpcl, McAccount *account, 
    GError **error_out)
{
  EBookBackendTpClPrivate *priv = NULL;

  g_assert (tpcl != NULL);
  e_book_backend_tp_return_val_with_error_if_fail (account, FALSE, error_out);

  priv = GET_PRIVATE (tpcl);

  /* It's a programming error to call this function twice */
  g_assert (priv->account == NULL);

  priv->account = g_object_ref (account);
  priv->account_name = priv->account->name;

  MESSAGE ("starting load process for %s", 
      priv->account_name);

  /* We have to ref tpcl to be sure we are not destroyed before the callback is
   * called */
  mc_account_call_when_ready (priv->account, account_ready_cb, g_object_ref (tpcl));

  return TRUE;
}

static void
e_book_backend_tp_cl_go_offline (EBookBackendTpCl *tpcl)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GArray *contacts;
  GHashTableIter iter;
  gpointer value;

  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  g_hash_table_iter_init (&iter, priv->contacts_hash);
  while (g_hash_table_iter_next (&iter, NULL, &value)) 
  {
    EBookBackendTpContact *contact = value;

    g_free (contact->status);
    contact->status = g_strdup ("unknown");

    contact->generic_status = presence_code_to_string (TP_CONNECTION_PRESENCE_TYPE_UNKNOWN);

    g_free (contact->status_message);
    contact->status_message = NULL;

    contact->capabilities = 0;

    g_array_append_val (contacts, contact);
  }

  if (contacts->len > 0) {
    g_signal_emit (tpcl, signals[PRESENCES_CHANGED], 0, contacts);
    g_signal_emit (tpcl, signals[CAPABILITIES_CHANGED], 0, contacts);
  }

  g_hash_table_remove_all (priv->contacts_hash);
  g_array_free (contacts, TRUE);
}

static void
e_book_backend_tp_cl_set_status (EBookBackendTpCl *tpcl, EBookBackendTpClStatus status)
{
  EBookBackendTpClPrivate *priv = NULL;

  g_assert (tpcl != NULL);

  priv = GET_PRIVATE (tpcl);

  if (status != priv->status)
  {
    priv->status = status;

    if (status == E_BOOK_BACKEND_TP_CL_ONLINE)
    {
      DEBUG ("Online and ready");
    } else {
      DEBUG ("Offline");
      e_book_backend_tp_cl_go_offline (tpcl);
    }

    g_signal_emit (tpcl, signals[STATUS_CHANGED], 0, status);
  }
}

EBookBackendTpClStatus
e_book_backend_tp_cl_get_status (EBookBackendTpCl *tpcl)
{
  EBookBackendTpClPrivate *priv;

  g_assert (tpcl != NULL);

  priv = GET_PRIVATE (tpcl);

  return priv->status;
}

typedef struct
{
  EBookBackendTpContactListId list_id;
  EBookBackendTpClGetMembersCallback cb;
  gpointer userdata;
} GetMembersClosure;

typedef struct {
  guint handle;
  const gchar *channel_type;
  guint generic;
  guint specific;
} ContactCapability;

static gint
contact_handle_sort_func (EBookBackendTpContact *a, EBookBackendTpContact *b)
{
  return -1 ? a->handle < b->handle: 0 ? a->handle == b->handle : -1;
}

static void
update_capabilities (EBookBackendTpCl *tpcl, GArray *capabilities)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContact *contact;
  GArray *contacts = NULL;
  guint i = 0;
  gint rev_index;
  ContactCapability *cap;
  TpHandle last_handle;
  TpHandle cur_handle;

  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  for (i = 0; i < capabilities->len; i++)
  {
    cap = &g_array_index (capabilities, ContactCapability, i);

    contact = g_hash_table_lookup (priv->contacts_hash, 
        GUINT_TO_POINTER (cap->handle));

    if (!contact)
    {
      DEBUG ("Told about capability on uknown contact: %d", cap->handle);
      continue;
    }

    g_array_append_val (contacts, contact);

    if (g_str_equal (cap->channel_type, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA))
    {
      contact->capabilities &= ~(CAP_VOICE | CAP_VIDEO);

      if (cap->specific & TP_CHANNEL_MEDIA_CAPABILITY_AUDIO)
      {
        contact->capabilities |= CAP_VOICE;
      }

      if (cap->specific & TP_CHANNEL_MEDIA_CAPABILITY_VIDEO)
      {
        contact->capabilities |= CAP_VIDEO;
      }

      if (cap->specific & TP_CHANNEL_MEDIA_CAPABILITY_IMMUTABLE_STREAMS)
      {
        contact->capabilities |= CAP_IMMUTABLE_STREAMS;
      }
    }

    if (g_str_equal (cap->channel_type, TP_IFACE_CHANNEL_TYPE_TEXT))
    {
      contact->capabilities |= CAP_TEXT;
    }
  }

  if (contacts->len == 0)
  {
    g_array_free (contacts, TRUE);
    return;
  }

  /* Remove duplicates. First we sort based on the handle */
  g_array_sort (contacts, (GCompareFunc)contact_handle_sort_func);

  /* 
   * Then we iterate backwards removing those that are duplicates. We go
   * backwards so that we can use g_array_remove_index_fast which just puts
   * the last element in the place of the one we removed.
   */
  contact = g_array_index (contacts, EBookBackendTpContact *, contacts->len - 1);
  last_handle = contact->handle;

  for (rev_index = contacts->len - 2; rev_index >= 0; rev_index--)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, rev_index);
    cur_handle = contact->handle;
    if (cur_handle == last_handle)
    {
      g_array_remove_index_fast (contacts, rev_index);
    } else {
      last_handle = cur_handle;
    }
  }

  g_signal_emit (tpcl, signals[CAPABILITIES_CHANGED], 0, contacts);
  g_array_free (contacts, TRUE);
}

static void
capabilities_changed_cb (TpConnection *conn, const GPtrArray *caps, 
    gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  GArray *capabilities;
  GValueArray *values;
  ContactCapability *cap;
  guint i = 0;

  if (!verify_is_connected (tpcl, NULL))
    return;

  capabilities = g_array_sized_new (TRUE, TRUE, sizeof (ContactCapability), caps->len);
  g_array_set_size (capabilities, caps->len);

  for (i = 0; i < caps->len; i++)
  {
    values = g_ptr_array_index (caps, i);

    cap = &g_array_index (capabilities, ContactCapability, i);

    cap->handle = g_value_get_uint (g_value_array_get_nth (values, 0));
    cap->channel_type = g_value_get_string (g_value_array_get_nth (values, 1));
    cap->generic = g_value_get_uint (g_value_array_get_nth (values, 3));
    cap->specific = g_value_get_uint (g_value_array_get_nth (values, 5));

    DEBUG ("Capability information received for %d on %s with %x and %x",
        cap->handle, cap->channel_type, cap->generic, cap->specific);
  }

  update_capabilities (tpcl, capabilities);
  g_array_free (capabilities, TRUE);
}

static void 
get_capabilities_for_members_cb (TpConnection *conn, const GPtrArray *caps, 
    const GError *error, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  GValueArray *values;
  guint i = 0;
  ContactCapability *cap;
  GArray *capabilities;

  if (error)
  {
    WARNING ("Error whilst getting capabilities: %s", 
        error->message);
    return;
  }

  if (!verify_is_connected (tpcl, NULL))
    return;

  capabilities = g_array_sized_new (TRUE, TRUE, sizeof (ContactCapability), caps->len);
  g_array_set_size (capabilities, caps->len);

  for (i = 0; i < caps->len; i++)
  {
    values = g_ptr_array_index (caps, i);

    cap = &g_array_index (capabilities, ContactCapability, i);

    cap->handle = g_value_get_uint (g_value_array_get_nth (values, 0));
    cap->channel_type = g_value_get_string (g_value_array_get_nth (values, 1));
    cap->generic = g_value_get_uint (g_value_array_get_nth (values, 2));
    cap->specific = g_value_get_uint (g_value_array_get_nth (values, 3));

    DEBUG ("Capability information received for %d on %s with %x and %x",
        cap->handle, cap->channel_type, cap->generic, cap->specific);
  }

  update_capabilities (tpcl, capabilities);
  g_array_free (capabilities, TRUE);
}

/*************************************************/
/* Functions for Telepathy ContactInfo interface */
/*************************************************/

static void 
attribute_add_param_foreach_cb (gpointer key,
                                gpointer value,
                                gpointer data)
{
  EVCardAttribute *attr = (EVCardAttribute *)data;
  EVCardAttributeParam *param = (EVCardAttributeParam *)value;
  
  e_vcard_attribute_add_param (attr, param);
}

static void
contact_info_attribute_add_params (EVCardAttribute *attr,
                                   GStrv parameters)
{
  gchar **p;
  GHashTable *param_table;
    
  param_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  
  /* For GStrv parameters[3] = {"type=home", "type=voice", NULL};
   * This function returns parameter list like TEL;TYPE=HOME,VOICE:1111
   * instead of TEL;TYPE=HOME;TYPE=VOICE:1111 */
  for (p = parameters; *p != NULL; p++) {
    EVCardAttributeParam *param;
    gchar **list;
    gchar *name = NULL;
    gchar *value = NULL;
    
    list = g_strsplit (*p, "=", 2);
    if (list[1]) {
      /* e.g. "type=home" */
      
      /* UI only loves uppercase parameters, e.g. TEL;TYPE=HOME:1111 */
      name = g_ascii_strup (list[0], -1);
      value = g_ascii_strup (list[1], -1);
      
      param = g_hash_table_lookup (param_table, name);      
      if (param) {
        e_vcard_attribute_param_add_value (param, value);
        g_free (name);
      }
      else {
        param = e_vcard_attribute_param_new (name);
        e_vcard_attribute_param_add_value (param, value);
        /* Leave the ownership of name to the hash table */
        g_hash_table_insert (param_table, name, param);
      }
      g_free (value);
    }
    else {
      /* e.g. "X-OSSO-READONLY" */
      name = g_ascii_strup (*p, -1);
      param = e_vcard_attribute_param_new (name);
      e_vcard_attribute_add_param (attr, param);
      g_free (name);
    }
    
    g_strfreev (list);
  }
  
  g_hash_table_foreach (param_table, attribute_add_param_foreach_cb, attr);
  
  g_hash_table_unref (param_table);
}

static gchar * 
contact_info_to_vcard_str (const GPtrArray *handle_contactinfo)
{
  EVCard *evc;
  gchar *vcard_str;
  guint i;
  
  if (handle_contactinfo == NULL ||
      handle_contactinfo->len == 0)
    return NULL;
  
  /* serialize a contact's vcard information to a single string */
  evc = e_vcard_new ();
  for (i = 0; i < handle_contactinfo->len; i++)
  {
    EVCardAttribute *attr;
    GValueArray *val;
    const gchar *field;
    GStrv parameters;
    GStrv values;
    gchar **p;

    val = g_ptr_array_index (handle_contactinfo, i);
    
    field = g_value_get_string (g_value_array_get_nth (val, 0));
    parameters = g_value_get_boxed (g_value_array_get_nth (val, 1));
    values = g_value_get_boxed (g_value_array_get_nth (val, 2));
    
    attr = e_vcard_attribute_new (NULL, field);
    
    /* parameter list. Fields such as adr, tel, email may have parameters */
    if (parameters[0])
      contact_info_attribute_add_params (attr, parameters);
          
    for (p = values; *p != NULL; p++)
      e_vcard_attribute_add_value (attr, *p);

    e_vcard_add_attribute (evc, attr);
  }
  
  vcard_str = e_vcard_to_string (evc, EVC_FORMAT_VCARD_30);
  DEBUG ("ContactInfo VCard string:\n%s", vcard_str);

  g_object_unref (evc);
  return vcard_str;
}

static void 
contact_info_changed_cb (TpConnection *conn, guint handle, 
    const GPtrArray *handle_contactinfo, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContact *contact = NULL;
    
  DEBUG ("contact_info_changed_cb");
  
  if (!verify_is_connected (tpcl, NULL))
    return;

  contact = g_hash_table_lookup (priv->contacts_hash, 
      GUINT_TO_POINTER (handle));
  
  if (contact) {
    gchar *vcard_str;
    
    vcard_str = contact_info_to_vcard_str (handle_contactinfo);
    
    g_free (contact->contact_info);
    contact->contact_info = vcard_str;
    
    if (vcard_str) {
      GArray *contacts;
      
      contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
      g_array_append_val (contacts, contact);
      g_signal_emit (tpcl, signals[CONTACT_INFO_CHANGED], 0, contacts);
      
      g_array_free (contacts, TRUE);
    }
  } else {
    WARNING ("mismatched contact and contact info");
  }
}

/**
 * @out_contactinfo: a dictionary whose keys are contact handles
 * and whose values are contact information 
 * 
 */
static void 
get_contact_info_for_members_cb (TpConnection *conn, GHashTable *out_contactinfo,
    const GError *error, gpointer user_data, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GList *handles = NULL, *l = NULL;
  TpHandle handle;
  GPtrArray *handle_contactinfo;
  gchar *vcard_str = NULL;
  
  GArray *contacts = NULL;
  EBookBackendTpContact *contact = NULL;
  
  if (error)
  {
    WARNING ("Error whilst getting contact info: %s", 
        error->message);
    return;
  }
  
  if (!verify_is_connected (tpcl, NULL))
    return;

  DEBUG ("get_contact_info_for_members_cb");
  
  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
  handles = g_hash_table_get_keys (out_contactinfo);
  
  for (l = handles; l; l = g_list_next(l))
  {
    handle = (TpHandle) GPOINTER_TO_UINT (l->data);
    
    contact = g_hash_table_lookup (priv->contacts_hash, 
        GUINT_TO_POINTER (handle));
    
    if (contact) {
      handle_contactinfo = (GPtrArray *)g_hash_table_lookup (out_contactinfo,
              GUINT_TO_POINTER (handle));
      vcard_str = contact_info_to_vcard_str (handle_contactinfo);
      
      g_free (contact->contact_info);
      contact->contact_info = vcard_str;
      
      if (vcard_str)
        g_array_append_val (contacts, contact);
    } else {
      WARNING ("mismatched contact and contact info");
    }
  }
  
  if (contacts->len > 0)
    g_signal_emit (tpcl, signals[CONTACT_INFO_CHANGED], 0, contacts);
  
  g_array_free (contacts, TRUE);
  g_list_free (handles);
}

/****************************************************************/

static gboolean
verify_is_connected_for_get_channel_members (EBookBackendTpCl *tpcl,
    GetMembersClosure *closure)
{
  GError *error = NULL;

  if (!verify_is_connected (tpcl, &error))
  {
    closure->cb (tpcl, NULL, error, closure->userdata);
    g_free (closure);

    return FALSE;
  }
  else
    return TRUE;
}

static void
get_contacts_cb (TpConnection *connection, guint n_tp_contacts,
    TpContact * const *tp_contacts, guint n_failed, const TpHandle *failed,
    const GError *error, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = E_BOOK_BACKEND_TP_CL (weak_object);
  GetMembersClosure *closure = userdata;
  GArray *contacts;

  DEBUG ("contacts retrieved");

  if (error)
  {
    closure->cb (tpcl, NULL, error, closure->userdata);
    g_free (closure);

    WARNING ("error when getting contacts: %s", error->message);
    return;
  }

  if (!verify_is_connected_for_get_channel_members (tpcl, closure))
    return;

  contacts = update_contact_details (tpcl, n_tp_contacts, tp_contacts);

  closure->cb (tpcl, contacts, NULL, closure->userdata);

  if (contacts->len > 0)
  {
    g_signal_emit (tpcl, signals[ALIASES_CHANGED], 0, contacts);
    g_signal_emit (tpcl, signals[AVATAR_TOKENS_CHANGED], 0, contacts);
    g_signal_emit (tpcl, signals[PRESENCES_CHANGED], 0, contacts);
    g_signal_emit (tpcl, signals[FLAGS_CHANGED], 0, contacts);
  }

  inspect_additional_features (tpcl, contacts);

  g_free (closure);
  g_array_free (contacts, TRUE);

  /* Failed contacts are contacts that we inspected but were removed in
   * the meantime (yay for async programming!) so their handle became
   * invalid. We just ignore the invalid handles as we get a separate
   * notification for their removal.
   * Note that is also possible that not all the TpContacts passed to
   * update_contact_details are converted to EBookBackendTpContact as
   * the inspection can succeed even for gone contacts. It's safe to
   * ignore this too. */
}

static void
finish_get_channel_members (EBookBackendTpCl *tpcl, GetMembersClosure *closure)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GArray *handles;
  GHashTableIter iter;
  gpointer key;
  const TpContactFeature features[] = {TP_CONTACT_FEATURE_ALIAS,
      TP_CONTACT_FEATURE_AVATAR_TOKEN, TP_CONTACT_FEATURE_PRESENCE};

  if (!verify_is_connected_for_get_channel_members (tpcl, closure))
    return;

  if (g_hash_table_size (priv->contacts_hash) == 0) {
    /* No need to inspect the contacts if there are no contacts */
    GArray *contacts;

    contacts = g_array_sized_new (TRUE, TRUE,
        sizeof (EBookBackendTpContact), 0);
    closure->cb (tpcl, contacts, NULL, closure->userdata);
    g_array_free (contacts, TRUE);

    g_free (closure);

    return;
  }

  handles = g_array_sized_new (TRUE, TRUE, sizeof (TpHandle),
      g_hash_table_size (priv->contacts_hash));

  g_hash_table_iter_init (&iter, priv->contacts_hash);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    TpHandle handle = GPOINTER_TO_UINT (key);
    g_array_append_val (handles, handle);
  }

  DEBUG ("getting contact details for all members");
  tp_connection_get_contacts_by_handle (priv->conn,
      handles->len,
      (TpHandle *)handles->data,
      G_N_ELEMENTS (features), features,
      get_contacts_cb, closure, NULL, G_OBJECT (tpcl));

  g_array_free (handles, TRUE);
}

static void get_next_channel_members (EBookBackendTpCl *tpcl, 
    GetMembersClosure *closure);

static void
channel_get_all_members_cb (TpChannel *channel, const GArray *current,
    const GArray *local_pending, const GArray *remote_pending,
    const GError *error_in, gpointer userdata, GObject *weak_object)
{
  EBookBackendTpCl *tpcl = (EBookBackendTpCl *)weak_object;
  GetMembersClosure *closure = (GetMembersClosure *)userdata;
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  guint i = 0;
  EBookBackendTpContact *contact = NULL;
  TpHandle handle;
  EBookBackendTpContactListId list_id = closure->list_id;
  EBookBackendTpContactFlag flag;

  DEBUG ("channel_get_all_members_cb called for %s", 
      contact_list_id_to_string (closure->list_id));

  if (error_in)
  {
    WARNING ("error when getting all members on %s: %s", 
        contact_list_id_to_string (closure->list_id), error_in->message);
    closure->cb (tpcl, NULL, error_in, closure->userdata);
    g_free (closure);
    return;
  }

  if (!verify_is_connected_for_get_channel_members (tpcl, closure))
    return;

  for (i = 0; i < current->len; i++)
  {
    handle = g_array_index (current, TpHandle, i);
    list_id = closure->list_id;
    flag = CONTACT_FLAG_FROM_ID (list_id);
    contact = g_hash_table_lookup (priv->contacts_hash,
        GUINT_TO_POINTER (handle));
    if (!contact)
    {
      contact = e_book_backend_tp_contact_new ();
      g_hash_table_insert (priv->contacts_hash, GUINT_TO_POINTER (handle),
          contact);
    }
    contact->handle = handle;
    contact->flags |= flag;
  }

  for (i = 0; i < local_pending->len; i++)
  {
    handle = g_array_index (local_pending, TpHandle, i);
    list_id = CONTACT_LIST_ID_GET_LOCAL_FROM_CURRENT (closure->list_id);
    flag = CONTACT_FLAG_FROM_ID (list_id);
    contact = g_hash_table_lookup (priv->contacts_hash,
        GUINT_TO_POINTER (handle));
    if (!contact)
    {
      contact = e_book_backend_tp_contact_new ();
      g_hash_table_insert (priv->contacts_hash, GUINT_TO_POINTER (handle),
          contact);
    }
    contact->handle = handle;
    contact->flags |= flag;
  }

  for (i = 0; i < remote_pending->len; i++)
  {
    handle = g_array_index (remote_pending, TpHandle, i);
    list_id = CONTACT_LIST_ID_GET_REMOTE_FROM_CURRENT (closure->list_id);
    flag = CONTACT_FLAG_FROM_ID (list_id);
    contact = g_hash_table_lookup (priv->contacts_hash,
        GUINT_TO_POINTER (handle));
    if (!contact)
    {
      contact = e_book_backend_tp_contact_new ();
      g_hash_table_insert (priv->contacts_hash, GUINT_TO_POINTER (handle),
          contact);
    }
    contact->handle = handle;
    contact->flags |= flag;
  }

  /* incremental the current id */
  /* TODO: macroify */
  closure->list_id += 3;
  get_next_channel_members (tpcl, closure);
}

static void
get_next_channel_members (EBookBackendTpCl *tpcl, GetMembersClosure *closure)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpClContactList *backend_channel = NULL;
  TpChannel *channel;

  if (!verify_is_connected_for_get_channel_members (tpcl, closure))
    return;

  if (closure->list_id < CL_LAST_LIST)
  {
    backend_channel = priv->contact_list_channels[closure->list_id];

    if (backend_channel)
    {
      channel = backend_channel->channel;

      DEBUG ("requesting handles for all members in %s",
          contact_list_id_to_string (closure->list_id));
      tp_cli_channel_interface_group_call_get_all_members (channel,
          -1,
          channel_get_all_members_cb,
          closure,
          NULL,
          (GObject *)tpcl);
    } else {
      /* We must skip one because this is an unknown list */
      /* TODO: macroify */
      closure->list_id += 3;
      return get_next_channel_members (tpcl, closure);
    }
  } else {
    finish_get_channel_members (tpcl, closure);
  }
}

gboolean
e_book_backend_tp_cl_get_members (EBookBackendTpCl *tpcl,
    EBookBackendTpClGetMembersCallback cb, gpointer userdata,
    GError **error_out)
{
  EBookBackendTpClPrivate *priv;
  GetMembersClosure *closure;

  g_assert (cb != NULL);
  g_assert (tpcl != NULL);

  priv = GET_PRIVATE (tpcl);

  if (priv->status == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    closure = g_new0 (GetMembersClosure, 1);
    closure->cb = cb;
    closure->userdata = userdata;
    closure->list_id = CL_SUBSCRIBE;
    get_next_channel_members (tpcl, closure);
  }

  return TRUE;
}

gboolean
e_book_backend_tp_cl_run_update_flags (EBookBackendTpCl *tpcl,
    EBookBackendTpContact *updated_contact, GError **error_out)
{
  EBookBackendTpClPrivate *priv = NULL;
  EBookBackendTpContact *contact = NULL;

  GArray *handles_to_add = NULL;
  GArray *handles_to_remove = NULL;

  gboolean success = FALSE;
  GError *error = NULL;

  gint i = 0;

  g_assert (tpcl);
  g_assert (updated_contact);

  if (!verify_is_connected (tpcl, error_out))
    return FALSE;

  g_object_ref (tpcl);
  e_book_backend_tp_contact_ref (updated_contact);

  priv = GET_PRIVATE (tpcl);

  /* Check we have a handle on the contact we've been given */
  if (updated_contact->handle > 0)
  {
    contact = g_hash_table_lookup (priv->contacts_hash, 
        GINT_TO_POINTER (updated_contact->handle));

    if (contact)
    {
      DEBUG ("found matching contact for %s", contact->name);

      e_book_backend_tp_contact_ref (contact);

      handles_to_add = g_array_new (TRUE, TRUE, sizeof (TpHandle));
      handles_to_remove = g_array_new (TRUE, TRUE, sizeof (TpHandle));

      for (i = 0; i < CL_LAST_LIST; i+=3)
      {
        if (updated_contact->pending_flags & CONTACT_FLAG_FROM_ID (i) && 
            !(contact->flags & CONTACT_FLAG_FROM_ID (i)))
        {
          DEBUG ("contact is to be added to %s",
              contact_list_id_to_string (i));
          g_array_append_val (handles_to_add, contact->handle);
        }

        if (contact->flags & CONTACT_FLAG_FROM_ID (i) &&
            !(updated_contact->pending_flags & CONTACT_FLAG_FROM_ID (i)))
        {
          DEBUG ("contact is to be removed from %s",
              contact_list_id_to_string (i));
          g_array_append_val (handles_to_remove, contact->handle);
        }

        DEBUG ("considering list %s",
            contact_list_id_to_string (i));

        if (priv->contact_list_channels[i])
        {
          if (!tp_cli_channel_interface_group_run_add_members (
              priv->contact_list_channels[i]->channel,
              -1,
              handles_to_add,
              NULL,
              &error,
              NULL))
          {
            WARNING ("error whilst running AddMembers for %s on %s: %s",
                contact->name, contact_list_id_to_string (i),
                error ? error->message : "unknown error");
            g_propagate_error (error_out, error);
          }

          /* The connnection could have been set to NULL during the
           * previous _run_ function */
          if (!verify_is_connected (tpcl, error_out))
            goto out;

          if (!tp_cli_channel_interface_group_run_remove_members (
              priv->contact_list_channels[i]->channel,
              -1,
              handles_to_remove,
              NULL,
              &error,
              NULL))
          {
            WARNING ("error whilst running RemoveMembers for %s on %s: %s",
                contact->name, contact_list_id_to_string (i),
                error ? error->message : "unknown error");
            g_propagate_error (error_out, error);
          }
        }

        g_array_set_size (handles_to_add, 0);
        g_array_set_size (handles_to_remove, 0);
      }
    } else {
      WARNING ("No valid handle on provided contact");
      g_set_error (error_out, E_BOOK_BACKEND_TP_CL_ERROR,
          E_BOOK_BACKEND_TP_CL_ERROR_FAILED,
          "No valid handle on contact provided.");
      goto out;
    }
  } else {
    WARNING ("No handle on provided contact");
    g_set_error (error_out, E_BOOK_BACKEND_TP_CL_ERROR,
        E_BOOK_BACKEND_TP_CL_ERROR_FAILED,
        "No handle on contact provided.");
    goto out;
  }

  success = TRUE;

out:
  g_object_unref (tpcl);
  e_book_backend_tp_contact_unref (updated_contact);
  if (contact)
    e_book_backend_tp_contact_unref (contact);

  g_array_free (handles_to_add, TRUE);
  g_array_free (handles_to_remove, TRUE);

  return success;
}

gboolean 
e_book_backend_tp_cl_run_add_contact (EBookBackendTpCl *tpcl,
    EBookBackendTpContact *new_contact, GError **error_out)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GError *error = NULL;
  GArray *handles = NULL;
  const gchar *names_to_request[2] = {0, 0};
  gint i = 0;
  guint group_flags = 0;
  gchar **names = NULL;
  gboolean success = FALSE;

  if (!verify_is_connected (tpcl, error_out))
    return FALSE;

  g_object_ref (tpcl);
  e_book_backend_tp_contact_ref (new_contact);

  /* Request a handle from the CM to use for our 'name' i.e. Jabber id or
   * whatever
   */
  names_to_request[0] = new_contact->name;

  if (!tp_cli_connection_run_request_handles (priv->conn,
        -1,
        TP_HANDLE_TYPE_CONTACT,
        names_to_request,
        &handles,
        &error,
        NULL))
  {
    WARNING ("Error whilst requesting handle: %s",
        error ? error->message : "unknown error");
    g_propagate_error (error_out, error);
    goto out;
  }

  /* The connnection could have been set to NULL during the
   * previous _run_ function */
  if (!verify_is_connected (tpcl, error_out))
    goto out;

  new_contact->handle = g_array_index (handles, TpHandle, 0);

  /* Lets get our name back and update our idea. This is our way of getting
   * the normalised name.
   */
  if (!tp_cli_connection_run_inspect_handles (
        priv->conn,
        -1,
        TP_HANDLE_TYPE_CONTACT,
        handles,
        &names,
        &error,
        NULL))
  {
    WARNING ("error whilst inspecting our handle: %s",
        error ? error->message : "unknown error");
    g_propagate_error (error_out, error);
    goto out;
  }

  e_book_backend_tp_contact_update_name  (new_contact, names[0]);
  g_strfreev (names);

  /* For all the lists, check if we're marked to add if so add */
  for (i = 0; i < CL_LAST_LIST; i+=3)
  {
    if (new_contact->pending_flags & CONTACT_FLAG_FROM_ID (i))
    {
      if (priv->contact_list_channels[i])
      {
        /* But first we must check if we can add */
        if (!tp_cli_channel_interface_group_run_get_group_flags (
            priv->contact_list_channels[i]->channel,
            -1,
            &group_flags,
            &error,
            NULL))
        {
          WARNING ("Error getting group flags: %s",
              error ? error->message : "unkwnown error");
          g_propagate_error (error_out, error);
          goto out;
        }

        if (group_flags & TP_CHANNEL_GROUP_FLAG_CAN_ADD)
        {
          if (!tp_cli_channel_interface_group_run_add_members (
              priv->contact_list_channels[i]->channel,
              -1,
              handles,
              NULL,
              &error,
              NULL))
          {
            WARNING ("error whilst running AddMembers for %s on %s: %s",
                new_contact->name, contact_list_id_to_string (i),
                error ? error->message : "unknown error");
            g_propagate_error (error_out, error);
            goto out;
          }
        }
      }
    }
  }
 
  success = TRUE;

out:
  g_object_unref (tpcl);
  e_book_backend_tp_contact_unref (new_contact);

  if (handles)
    g_array_free (handles, TRUE);

  return success;
}

gboolean
e_book_backend_tp_cl_run_remove_contact (EBookBackendTpCl *tpcl, 
    EBookBackendTpContact *contact_in, GError **error_out)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  GArray *handles = NULL;
  guint32 group_flags = 0;
  gint i = 0;
  GError *error = NULL;
  EBookBackendTpContact *contact = NULL;
  gboolean success = TRUE;

  if (!verify_is_connected (tpcl, error_out))
    return FALSE;

  contact = g_hash_table_lookup (priv->contacts_hash, 
      GUINT_TO_POINTER (contact_in->handle));

  if (!contact)
  {
    g_set_error (error_out, 
        E_BOOK_BACKEND_TP_CL_ERROR,
        E_BOOK_BACKEND_TP_CL_ERROR_FAILED,
        "Requesting deletion of unknown contact");
    return FALSE;
  }

  /* The tp_*_run_* functions are pseudo sync but they run their own main
   * loop, so signals are received in the meantime. This mean that the
   * contact could be removed from the hash table and unreferenced while
   * this function is still running. */
  e_book_backend_tp_contact_ref (contact);
  g_object_ref (tpcl);

  handles = g_array_new (TRUE, TRUE, sizeof (TpHandle));
  g_array_append_val (handles, contact->handle);

  /* For all the lists, check if we're marked to add if so add */
  for (i = 0; i < CL_LAST_LIST; i+=3)
  {
    if (contact->flags & CONTACT_FLAG_FROM_ID (i) ||
        contact->flags & CONTACT_FLAG_FROM_ID (i+1) ||
        contact->flags & CONTACT_FLAG_FROM_ID (i+2))
    {
      if (priv->contact_list_channels[i])
      {
        /* The connnection could have been set to NULL during the
         * a _run_ function call in the previous loop. */
        if (!verify_is_connected (tpcl, error_out))
        {
          success = FALSE;
          break;
        }

        /* But first we must check if we can remove */
        if (!tp_cli_channel_interface_group_run_get_group_flags (
            priv->contact_list_channels[i]->channel,
            -1,
            &group_flags,
            &error,
            NULL))
        {
          WARNING ("Error getting group flags: %s",
              error ? error->message : "unknown error");
          g_propagate_error (error_out, error);
          success = FALSE;
          break;
        }

        /* The connnection could have been set to NULL during the
         * previous _run_ function */
        if (!verify_is_connected (tpcl, error_out))
        {
          success = FALSE;
          break;
        }

        if (group_flags & TP_CHANNEL_GROUP_FLAG_CAN_REMOVE)
        {
          if (!tp_cli_channel_interface_group_run_remove_members (
              priv->contact_list_channels[i]->channel,
              -1,
              handles,
              NULL,
              &error,
              NULL))
          {
            WARNING ("error whilst running RemoveMembers for %s on %s: %s",
                contact->name, contact_list_id_to_string (i),
                error ? error->message : "unknown error");
            g_propagate_error (error_out, error);
            success = FALSE;
            break;
          }
        }
      }
    }
  }

  g_array_free (handles, TRUE);

  g_object_unref (tpcl);
  e_book_backend_tp_contact_unref (contact);

  return success;
}

gboolean
e_book_backend_tp_cl_run_unblock_contact (EBookBackendTpCl *tpcl, 
    EBookBackendTpContact *contact_in, GError **error_out)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpClContactList *deny_channel;
  guint32 group_flags = 0;
  GError *error = NULL;
  EBookBackendTpContact *contact = NULL;
  gboolean success = TRUE;

  if (!verify_is_connected (tpcl, error_out))
    return FALSE;

  contact = g_hash_table_lookup (priv->contacts_hash, 
      GUINT_TO_POINTER (contact_in->handle));

  if (!contact)
  {
    g_set_error (error_out, 
        E_BOOK_BACKEND_TP_CL_ERROR,
        E_BOOK_BACKEND_TP_CL_ERROR_FAILED,
        "Requesting unblocking of unknown contact");
    return FALSE;
  }

  deny_channel = priv->contact_list_channels[CL_DENY];
  if (!deny_channel)
  {
    /* If we don't have the deny list calling this function doesn't make
     * sense, but we return TRUE as the contact is already unblocked */
    MESSAGE ("trying to unblock a contact for a connection that doesn't "
        "support contact blocking");
    return TRUE;
  }

  e_book_backend_tp_contact_ref (contact);
  g_object_ref (tpcl);

  /* First we must check if we can remove */
  if (!tp_cli_channel_interface_group_run_get_group_flags (
        deny_channel->channel, -1, &group_flags, &error, NULL))
  {
    WARNING ("Error getting group flags: %s",
        error ? error->message : "unknown error");
    g_propagate_error (error_out, error);
    success = FALSE;
  }

  /* The connnection could have been set to NULL during the
   * previous _run_ function */
  if (!verify_is_connected (tpcl, error_out))
    success = FALSE;

  if (group_flags & TP_CHANNEL_GROUP_FLAG_CAN_REMOVE && success)
  {
    GArray *handles = NULL;

    handles = g_array_new (TRUE, TRUE, sizeof (TpHandle));
    g_array_append_val (handles, contact->handle);

    if (!tp_cli_channel_interface_group_run_remove_members (
          deny_channel->channel, -1, handles, NULL, &error, NULL))
    {
      WARNING ("error whilst running RemoveMembers for %s on deny: %s",
          contact->name,
          error ? error->message : "unknown error");
      g_propagate_error (error_out, error);
      success = FALSE;
    }

    g_array_free (handles, TRUE);
  }

  g_object_unref (tpcl);
  e_book_backend_tp_contact_unref (contact);

  return success;
}

gboolean 
e_book_backend_tp_cl_request_avatar_data (EBookBackendTpCl *tpcl,
    GArray *contacts, GError **error_out)
{
  EBookBackendTpClPrivate *priv = GET_PRIVATE (tpcl);
  EBookBackendTpContact *contact;
  guint i = 0;
  GArray *handles = NULL;

  if (!contacts || !contacts->len)
    return TRUE;

  if (!verify_is_connected (tpcl, error_out))
    return FALSE;

  handles = g_array_new (TRUE, TRUE, sizeof (TpHandle));
  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    g_array_append_val (handles, contact->handle);
  }

  tp_cli_connection_interface_avatars_call_request_avatars (priv->conn,
          -1,
          handles,
          NULL,
          NULL,
          NULL,
          NULL);

  g_array_free (handles, TRUE);

  /* We return always TRUE as there is no way to return a success/error
   * flag in a sync way. Using _run_request_avatars() is not an option
   * as it introduces other problems because it runs the mainloop. */
  return TRUE;
}
