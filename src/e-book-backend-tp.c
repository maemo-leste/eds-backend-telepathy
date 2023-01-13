/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *   @author Rob Bradford
 *   @author Travis Reitter <travis.reitter@maemo.org>
 *   @author Marco Barisione <marco.barisione@collabora.co.uk>
 *   @author Mathias Hasselmann <mathias.hasselmann@maemo.org>
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

#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <mce/dbus-names.h>
#include <telepathy-glib/dbus.h>
#include <libedata-book/libedata-book.h>
#include <gio/gio.h>
#include <telepathy-glib/util.h>

#include "e-book-backend-tp.h"
#include "e-book-backend-tp-cl.h"
#include "e-book-backend-tp-contact.h"
#include "e-book-backend-tp-db.h"
#include "e-book-backend-tp-log.h"

#define EC_ERROR(_code) \
  (e_client_error_create (E_CLIENT_ERROR_ ## _code, NULL))
#define EC_ERROR_EX(_code, _msg) \
  (e_client_error_create (E_CLIENT_ERROR_ ## _code, _msg))

#define EBC_ERROR(_code) \
  (e_book_client_error_create (E_BOOK_CLIENT_ERROR_ ## _code, NULL))

#define GET_PRIVATE(o) \
  ((EBookBackendTpPrivate *) e_book_backend_tp_get_instance_private (E_BOOK_BACKEND_TP (o)))

#define INVACTIVITY_MATCH_RULE \
  "type='signal',interface='" MCE_SIGNAL_IF "',member='" MCE_INACTIVITY_SIG "'"

#define MAX_PENDING_CONTACTS 50

static GQuark mce_signal_interface_quark = 0;
static GQuark mce_inactivity_signal_quark = 0;

struct _EBookBackendTpPrivate {
  EBookBackendTpCl *tpcl;
  GHashTable *handle_to_contact;
  GHashTable *name_to_contact;
  GHashTable *uid_to_contact;
  EBookBackendTpDb *tpdb;
  gboolean load_started; /* initial populate from database */
  gboolean members_ready; /* members ready to report to views */
  gulong members_ready_signal_id;
  GList *views;
  TpAccount *account;
  gchar *vcard_field;
  gchar *protocol_name;
  DBusGProxy *mce_request_proxy;
  gboolean system_inactive;
  gboolean load_error; /* we cannot report errors back when asynchronously
                        * loading an account, so we have to use this hack */

  GHashTable *contacts_to_delete; /* contacts scheduled for deletion */
  GHashTable *contacts_to_update; /* contacts scheduled for update */
  GHashTable *contacts_to_add; /* contacts scheduled for addition */

  GHashTable *contacts_to_update_in_db;

  gboolean is_loading; /* we are syncing the contacts with the roster */
  gboolean need_contacts_reload; /* need to repeat the initialization */

  /* When we receive a signal like aliases-changed we delay the update to
   * an idle callback, so:
   * 1. we avoid to starve the main loop
   * 2. if more signals arrive in sequence we generate the vcards only once
   */
  GHashTable *contacts_remotely_changed; /* the contacts that changed */
  guint contacts_remotely_changed_update_id; /* source id of the callback */
};

G_DEFINE_TYPE_WITH_PRIVATE (EBookBackendTp,
                            e_book_backend_tp,
                            E_TYPE_BOOK_BACKEND_SYNC);

enum
{
  READY_SIGNAL = 0,
  MEMBERS_READY_SIGNAL = 1,
  LAST_SIGNAL
};

static guint32 signals[LAST_SIGNAL] = { 0 };

typedef enum {
    CONTACT_SORT_ORDER_FIRST_LAST,
    CONTACT_SORT_ORDER_LAST_FIRST,
    CONTACT_SORT_ORDER_NICKNAME
} ContactSortOrder;

/* Key used to store the sort order on a book view using g_object_set_data */
#define BOOK_VIEW_SORT_ORDER_DATA_KEY "tp-backend-contact-sort-order"

static gchar *
e_book_backend_tp_generate_uid (EBookBackendTp *backend, const gchar *name)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  gchar *tmp = NULL;
  gint count = 0;
  const gchar *path_suffix;

  path_suffix = tp_account_get_path_suffix(priv->account);

  tmp = g_strdup_printf ("%s-%s", path_suffix, name);

  while (g_hash_table_lookup (priv->uid_to_contact, tmp))
  {
    count++;
    g_free (tmp);
    tmp = g_strdup_printf ("%s-%s-%d", path_suffix, name, count);
  }

  return tmp;
}

static void notify_complete_all_views (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = NULL;
  GList *l = NULL;

  priv = GET_PRIVATE (backend);

  for (l = priv->views; l != NULL; l = l->next)
    e_data_book_view_notify_complete ((EDataBookView *)l->data, NULL);
}

static guint
notify_remotely_updated_contacts (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = NULL;
  guint n_updated_contacts = 0;
  GHashTableIter iter;
  gpointer contact_pointer;
  EBookBackendTpContact *contact = NULL;
  GList *l = NULL;
  EContact *ec;

  priv = GET_PRIVATE (backend);

  DEBUG ("notifying of change in contacts details");

  /* We are flushing contacts now, so there is no point in having the idle
   * callback running */
  if (priv->contacts_remotely_changed_update_id)
  {
    g_source_remove (priv->contacts_remotely_changed_update_id);
    priv->contacts_remotely_changed_update_id = 0;
  }

  if (!priv->views)
    goto done;

  g_hash_table_iter_init (&iter, priv->contacts_remotely_changed);
  while (g_hash_table_iter_next (&iter, NULL, &contact_pointer))
  {
    contact = contact_pointer;

    if (!e_book_backend_tp_contact_is_visible (contact))
      continue;

    ec = e_book_backend_tp_contact_to_econtact (contact, priv->vcard_field,
        priv->protocol_name);

    /* For each view we known about tell them about the contact */
    for (l = priv->views; l != NULL; l = l->next)
    {
      DEBUG ("notifying contact: %s", contact->name);
      e_data_book_view_notify_update ((EDataBookView *)l->data, ec);
    }

    n_updated_contacts++;

    g_object_unref (ec);
  }

done:
  g_hash_table_remove_all (priv->contacts_remotely_changed);

  return n_updated_contacts;
}

static void
notify_remotely_updated_contacts_and_complete (EBookBackendTp *backend)
{
  if (notify_remotely_updated_contacts (backend) > 0)
    notify_complete_all_views (backend);
}

static void
notify_updated_contact (EBookBackendTp *backend,
    EBookBackendTpContact *contact)
{
  EBookBackendTpPrivate *priv = NULL;
  EContact *ec;
  GList *l = NULL;

  priv = GET_PRIVATE (backend);

  if (!priv->views)
    return;

  /* Do not notify twice if the contact is already in the list of changed
   * contacts */
  g_hash_table_remove (priv->contacts_remotely_changed, contact->uid);

  notify_remotely_updated_contacts (backend);

  ec = e_book_backend_tp_contact_to_econtact (contact, priv->vcard_field,
      priv->protocol_name);

  /* For each view we known about tell them about the contact */
  for (l = priv->views; l != NULL; l = l->next)
  {
    DEBUG ("notifying contact: %s", contact->name);
    e_data_book_view_notify_update ((EDataBookView *)l->data, ec);
  }

  g_object_unref (ec);

  notify_complete_all_views (backend);
}

static void
flush_db_updates (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);

  GList *tmp_list, *l;
  GArray *contacts;

  GError *error = NULL;

  /*
   * It's possible that a flush of the contacts info is requested after the
   * DB has been deleted, so we avoid to fail or emit criticals if this
   * happen.
   */
  if (!priv->tpdb)
  {
    DEBUG ("skipping flush as the database was deleted");

    g_hash_table_remove_all (priv->contacts_to_update_in_db);
    return;
  }

  tmp_list = g_hash_table_get_values (priv->contacts_to_update_in_db);
  if (!tmp_list)
    return;

  DEBUG ("flushing pending contacts to db");

  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  for (l = tmp_list; l; l = l->next)
  {
    g_array_append_val (contacts, l->data);
  }

  if (!e_book_backend_tp_db_update_contacts (priv->tpdb, contacts, &error))
  {
    g_critical ("Error whilst flushing pending contacts to db: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
  }

  g_hash_table_remove_all (priv->contacts_to_update_in_db);

  g_array_free (contacts, TRUE);
  g_list_free (tmp_list);
}

static gboolean
update_contacts_idle_cb (gpointer userdata)
{
  EBookBackendTp *backend = userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  gboolean notify_now;

  DEBUG ("update remotely changed contacts");

  if (!priv->system_inactive)
  {
    DEBUG ("notifying pending changes now as the system is not inactive");
    notify_now = TRUE;
  }
  else if (g_hash_table_size (priv->contacts_remotely_changed) > MAX_PENDING_CONTACTS)
  {
    /* Do not keep to many pending changes to avoid having to much work to
     * do in the UI when we come back from inactivity.
     * Note that the threshold is completely arbitrary and, moreover, is
     * per account. */
    DEBUG ("notifying pending changes now as there are too many changes");
    notify_now = TRUE;
  }
  else
    notify_now = FALSE;

  if (notify_now)
    notify_remotely_updated_contacts_and_complete (backend);
  else
    DEBUG ("inactive system, skipping update");

  flush_db_updates (backend);

  priv->contacts_remotely_changed_update_id = 0;

  return FALSE;
}

static void
update_contacts (EBookBackendTp *backend, GArray *contacts, gboolean update_db)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact;
  guint i = 0;

  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);

    if (!g_hash_table_lookup (priv->contacts_remotely_changed, contact->uid))
    {
      DEBUG ("notification of update scheduled for contact: %s",
          contact->uid);
      g_hash_table_insert (priv->contacts_remotely_changed,
          g_strdup (contact->uid), e_book_backend_tp_contact_ref (contact));
    }

    /* Some data, like presence, is not saved on DB */
    if (update_db)
    {
      if (!g_hash_table_lookup (priv->contacts_to_update_in_db, contact->uid))
      {
        DEBUG ("update in the DB scheduled for contact: %s",
            contact->uid);
        g_hash_table_insert (priv->contacts_to_update_in_db,
            g_strdup (contact->uid),
            e_book_backend_tp_contact_ref (contact));
      } else {
        DEBUG ("update in the DB requested for already scheduled contact: %s",
            contact->uid);
      }
    }
  }

  if (!priv->contacts_remotely_changed_update_id)
    priv->contacts_remotely_changed_update_id = g_idle_add_full (
        G_PRIORITY_LOW, update_contacts_idle_cb, backend, NULL);
}

static void
delete_contacts (EBookBackendTp *backend, GArray *contacts)
{
  EBookBackendTpPrivate *priv = NULL;
  EBookBackendTpContact *contact = NULL;
  guint i = 0;
  gchar *tmp = NULL;
  GArray *uids_to_delete = NULL;
  GList *l;

  priv = GET_PRIVATE (backend);

  g_return_if_fail (priv->tpdb);

  uids_to_delete = g_array_sized_new (TRUE, TRUE, sizeof (gchar *),
      contacts->len);

  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);

    MESSAGE ("removing contact %s", contact->name);

    if (contact->handle > 0)
    {
      DEBUG ("removing from handle to contact mapping");
      g_hash_table_remove (priv->handle_to_contact,
          GINT_TO_POINTER (contact->handle));
    }

    DEBUG ("ensure there are no pending changes for the contact");
    g_hash_table_remove (priv->contacts_remotely_changed, contact->uid);
    g_hash_table_remove (priv->contacts_to_update_in_db, contact->uid);

    tmp = g_strdup (contact->uid);
    g_array_append_val (uids_to_delete, tmp);

    DEBUG ("removing from name to contact mapping");
    g_hash_table_remove (priv->name_to_contact, contact->name);
    DEBUG ("removing from uid to contact mapping");
    g_hash_table_remove (priv->uid_to_contact, contact->uid);
  }

  flush_db_updates (backend);
  notify_remotely_updated_contacts (backend);

  DEBUG ("removing contacts from database");
  e_book_backend_tp_db_remove_contacts (priv->tpdb, uids_to_delete, NULL);

  for (i = 0; i < uids_to_delete->len; i++)
  {
    tmp = g_array_index (uids_to_delete, gchar *, i);
    DEBUG ("notifying %s", tmp);

    /* It's possible that we already sent a notification for this if the
     * contact was removed by us and not by another client, but deletion
     * of contacts is rare enough that there is no real need to optimise
     * this. */
    for (l = priv->views; l != NULL; l = l->next)
    {
      e_data_book_view_notify_remove ((EDataBookView *)l->data,
          tmp);
    }

    g_free (tmp);
  }

  notify_complete_all_views (backend);

  g_array_free (uids_to_delete, TRUE);
}

static void
tp_cl_aliases_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  guint i = 0;
  EBookBackendTpContact *contact_in = NULL;
  EBookBackendTpContact *contact = NULL;
  GArray *contacts_to_update = NULL;

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);

    contact = g_hash_table_lookup (priv->handle_to_contact,
        GINT_TO_POINTER (contact_in->handle));

    if (contact)
    {
      DEBUG ("updating alias for uid %s, handle %d and name %s from %s to %s",
          contact->uid, contact->handle, contact->name,
          contact->alias, contact_in->alias);
      g_free (contact->alias);
      contact->alias = g_strdup (contact_in->alias);

      if (contacts_to_update == NULL)
      {
        contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
      }

      g_array_append_val (contacts_to_update, contact);
    }
  }

  if (contacts_to_update)
  {
    update_contacts (backend, contacts_to_update, TRUE);
    g_array_free (contacts_to_update, TRUE);
  }
}

static void
tp_cl_presence_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  guint i = 0;
  EBookBackendTpContact *contact_in = NULL;
  EBookBackendTpContact *contact = NULL;
  GArray *contacts_to_update = NULL;

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);

    contact = g_hash_table_lookup (priv->handle_to_contact,
        GINT_TO_POINTER (contact_in->handle));

    if (contact)
    {
      DEBUG ("updating status for uid %s, handle %d and name %s "
          "from %s;%s to %s;%s",
          contact->uid, contact->handle, contact->name,
          contact->status, contact->status_message,
          contact_in->status, contact_in->status_message);

      g_free (contact->status);
      g_free (contact->status_message);

      contact->generic_status = contact_in->generic_status;
      contact->status = g_strdup (contact_in->status);
      contact->status_message = g_strdup (contact_in->status_message);

      if (contacts_to_update == NULL)
      {
        contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
      }

      g_array_append_val (contacts_to_update, contact);
    }
  }

  if (contacts_to_update)
  {
    update_contacts (backend, contacts_to_update, FALSE);
    g_array_free (contacts_to_update, TRUE);
  }
}

static void
tp_cl_flags_changed (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact = NULL;
  EBookBackendTpContact *contact_in = NULL;
  GArray *contacts_to_update = NULL;
  guint i = 0;

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);

    contact = g_hash_table_lookup (priv->handle_to_contact,
        GINT_TO_POINTER (contact_in->handle));

    if (contact)
    {
      DEBUG ("updating flags for uid %s, handle %d and name %s",
          contact->uid, contact->handle, contact->name);

      contact->flags = contact_in->flags;

      if (contacts_to_update == NULL)
      {
        contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
      }

      g_array_append_val (contacts_to_update, contact);
    }
  }

  if (contacts_to_update)
  {
    update_contacts (backend, contacts_to_update, TRUE);
    g_array_free (contacts_to_update, TRUE);
  }
}

/* At least in XMPP it's possible to retrieve the avatars for offline
 * contacts, even if we don't know the avatar token.
 * Requesting the avatar for all the offline contacts is an expensive
 * operation as it requests the full contacts' vcards so we do this only
 * for new contacts (because we added them, because they were added in
 * another client or because we just configured an IM account).
 */
static void
request_avatar_data_for_offline_contacts (EBookBackendTp *backend,
    GArray *contacts)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GArray *contacts_with_unknown_token;
  EBookBackendTpContact *contact;
  gint i;

  if (!contacts || !contacts->len)
    return;

  contacts_with_unknown_token = g_array_sized_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *), contacts->len);

  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    /* A NULL avatar token means that the avatar token is unknown, for
     * instance because the contact is offline.
     * Note that this is different from an empty avatar token that means
     * that the contact doesn't have an avatar. */
    if (!contact->avatar_token)
      g_array_append_val (contacts_with_unknown_token, contact);
  }

  if (contacts_with_unknown_token->len)
    e_book_backend_tp_cl_request_avatar_data (priv->tpcl,
        contacts_with_unknown_token, NULL);

  g_array_free (contacts_with_unknown_token, TRUE);
}

static void
tp_cl_contacts_added (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = NULL;
  EBookBackendTpContact *contact = NULL;
  EBookBackendTpContact *contact_in = NULL;
  guint i = 0;
  GArray *contacts_to_update = NULL;
  GArray *contacts_to_add = NULL;
  GError *error = NULL;

  priv = GET_PRIVATE (userdata);

  g_return_if_fail (priv->tpdb);

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);
    contact = g_hash_table_lookup (priv->name_to_contact,
        contact_in->name);

    if (contact)
    {
      DEBUG ("contact %s already known.", contact_in->name);

      /* Let's update the flags, handle and alias at the least */
      contact->flags = contact_in->flags;
      contact->handle = contact_in->handle;

      g_free (contact->alias);
      contact->alias = g_strdup (contact_in->alias);

      /* Clear the schedule add flag */
      contact->pending_flags &= ~SCHEDULE_ADD;
    } else {
      MESSAGE ("new contact found %s", contact_in->name);
      contact = e_book_backend_tp_contact_dup (contact_in);
      contact->uid = e_book_backend_tp_generate_uid (backend, contact->name);

      g_hash_table_insert (priv->name_to_contact,
          g_strdup (contact->name),
          e_book_backend_tp_contact_ref (contact));

      /* Get rid of the initial reference. If we didn't do this here we'd need
       * to make sure that we ref'ed everything going into the update_contacts
       * array and then unref after.
       */
      e_book_backend_tp_contact_unref (contact);

      if (!contacts_to_add)
        contacts_to_add = g_array_new (TRUE, TRUE, sizeof (EBookBackendTp *));

      g_array_append_val (contacts_to_add, contact);
    }

    if (!contacts_to_update)
      contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTp *));

    g_array_append_val (contacts_to_update, contact);

    g_hash_table_insert (priv->handle_to_contact,
        GINT_TO_POINTER (contact->handle),
        e_book_backend_tp_contact_ref (contact));
    g_hash_table_insert (priv->uid_to_contact,
        g_strdup (contact->uid),
        e_book_backend_tp_contact_ref (contact));
  }

  flush_db_updates (backend);

  if (contacts_to_add)
  {
    if (!e_book_backend_tp_db_add_contacts (priv->tpdb, contacts_to_add, &error))
    {
      g_critical ("Error when trying to save new contacts to database: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    }

    request_avatar_data_for_offline_contacts (backend, contacts_to_add);

    g_array_free (contacts_to_add, TRUE);
  }

  if (contacts_to_update)
  {
    update_contacts (backend, contacts_to_update, TRUE);
    g_array_free (contacts_to_update, TRUE);
  }

  /* When new contacts appear show them immediately, do not wait to
   * come back from idle */
  notify_remotely_updated_contacts_and_complete (backend);
}

static void
tp_cl_contacts_removed (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = NULL;
  EBookBackendTpContact *contact = NULL;
  EBookBackendTpContact *contact_in = NULL;
  GArray *contacts_to_remove = NULL;
  guint i = 0;

  priv = GET_PRIVATE (backend);

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);

    contact = g_hash_table_lookup (priv->handle_to_contact,
        GINT_TO_POINTER (contact_in->handle));

    if (contact)
    {
      MESSAGE ("told about removal of %s", contact->name);
      if (!contacts_to_remove)
        contacts_to_remove = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

      /* clear the schedule delete flag */
      contact->pending_flags &= ~SCHEDULE_DELETE;
      g_array_append_val (contacts_to_remove, contact);
    } else {
      DEBUG ("Told about the removal of unknown contact (%s)",
          contact_in->name);
    }
  }

  if (contacts_to_remove)
  {
    delete_contacts (backend, contacts_to_remove);
    g_array_free (contacts_to_remove, TRUE);
  }
}

static void
tp_cl_avatar_tokens_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  guint i = 0;
  EBookBackendTpContact *contact_in;
  EBookBackendTpContact *contact = NULL;
  GArray *contacts_to_request;
  GError *error = NULL;
  gchar *avatar_path = NULL;
  GArray *contacts_to_update = NULL;

  contacts_to_update = g_array_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *));
  contacts_to_request = g_array_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *));

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);
    contact = g_hash_table_lookup (priv->handle_to_contact,
        GUINT_TO_POINTER (contact_in->handle));

    if (!contact)
    {
      DEBUG ("Told about changed token for unknown contact handle: %d",
          contact_in->handle);
      continue;
    }

    /* If the contact_in->avatar_token is NULL (for instance because the
     * contact is offline) just use the one from the DB, else update the
     * avatar token */
    if (contact_in->avatar_token &&
        tp_strdiff (contact->avatar_token, contact_in->avatar_token))
    {
      g_free (contact->avatar_token);
      contact->avatar_token = g_strdup (contact_in->avatar_token);
    }

    if (contact->avatar_token && contact->avatar_token[0] != '\0')
      avatar_path = g_build_filename (g_get_home_dir (), ".osso-abook",
          "avatars", contact->avatar_token, NULL);
    else
      avatar_path = NULL;

    if (avatar_path && !g_file_test (avatar_path, G_FILE_TEST_EXISTS))
    {
      g_array_append_val (contacts_to_request, contact);
    } else {
      g_array_append_val (contacts_to_update, contact);
    }

    g_free (avatar_path);
  }

  /* We need to notify about contacts which have just changed avatars to an
   * existing image file (the contacts changed to avatars which need to retrieve
   * data will be updated once the data is stored) */
  if (contacts_to_update->len > 0)
  {
    update_contacts (backend, contacts_to_update, TRUE);
  }

  if (!e_book_backend_tp_cl_request_avatar_data (priv->tpcl, contacts_to_request, &error))
  {
    WARNING ("Error whilst requesting avatar data for changed tokens");
    g_clear_error (&error);
  }

  g_array_free (contacts_to_update, TRUE);
  g_array_free (contacts_to_request, TRUE);
}

typedef struct
{
  EBookBackendTp *backend;
  EBookBackendTpContact *contact;
} AvatarDataSavedClosure;

static void
avatar_data_saved_cb (GObject *source, GAsyncResult *res,
    gpointer userdata)
{
  AvatarDataSavedClosure *closure = (AvatarDataSavedClosure *)userdata;
  EBookBackendTp *backend = closure->backend;
  EBookBackendTpContact *contact = closure->contact;
  GError *error = NULL;
  GArray *contacts;

  if (!g_file_replace_contents_finish (G_FILE (source), res, NULL, &error))
  {
    WARNING ("Error whilst writing to avatar file: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    goto done;
  }

  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
  g_array_append_val (contacts, contact);
  update_contacts (backend, contacts, TRUE);
  g_array_free (contacts, TRUE);

done:
  g_object_unref (backend);
  e_book_backend_tp_contact_unref (contact);
  g_free (closure);
}

static void
tp_cl_avatar_data_changed_cb (EBookBackendTpCl *tpcl,
    EBookBackendTpContact *contact_in, gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact;
  gchar *avatar_path;
  GFile *avatar_file;
  AvatarDataSavedClosure *closure;

  contact = g_hash_table_lookup (priv->handle_to_contact,
      GUINT_TO_POINTER (contact_in->handle));

  if (!contact)
  {
    DEBUG ("Given avatar data for unknown contact with handle: %d",
        contact_in->handle);
    return;
  }

  if (contact_in->avatar_token &&
      tp_strdiff (contact->avatar_token, contact_in->avatar_token))
  {
    /* Usually when we request an avatar we already know the avatar token,
     * but it can happen that the avatar is received for contacts with a
     * NULL (i.e. unknown) avatar token or that the retrieved avatar is
     * different from the one we were expecting (it changed in the meantime).
     * In this case we have to update the DB. */
    GArray *contacts_to_update;

    g_free (contact->avatar_token);
    contact->avatar_token = g_strdup (contact_in->avatar_token);

    contacts_to_update = g_array_sized_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *), 1);
    g_array_append_val (contacts_to_update, contact);
    update_contacts (backend, contacts_to_update, TRUE);
    g_array_free (contacts_to_update, TRUE);
  }

  g_free (contact->avatar_mime);
  contact->avatar_mime = g_strdup (contact_in->avatar_mime);

  g_free (contact->avatar_data);
  contact->avatar_data = g_memdup (contact_in->avatar_data, contact_in->avatar_len);

  contact->avatar_len = contact_in->avatar_len;

  avatar_path = g_build_filename (g_get_home_dir (), ".osso-abook", "avatars",
      contact->avatar_token, NULL);
  avatar_file = g_file_new_for_path (avatar_path);

  closure = g_new0 (AvatarDataSavedClosure, 1);
  closure->backend = g_object_ref (backend);
  closure->contact = e_book_backend_tp_contact_ref (contact);

  g_file_replace_contents_async (avatar_file,
      contact->avatar_data,
      contact->avatar_len,
      NULL,
      FALSE,
      G_FILE_CREATE_NONE,
      NULL,
      avatar_data_saved_cb,
      closure);
  g_object_unref (avatar_file);
  g_free (avatar_path);
}

static void
tp_cl_capabilities_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact;
  EBookBackendTpContact *contact_in;
  guint i = 0;
  GArray *contacts_to_update = NULL;

  DEBUG ("capabalities_changed_cb");

  contacts_to_update = g_array_sized_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *), contacts->len);

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);
    contact = g_hash_table_lookup (priv->handle_to_contact,
        GUINT_TO_POINTER (contact_in->handle));

    if (!contact)
    {
      DEBUG ("Unknown contact with handle %d",
          contact_in->handle);
      continue;
    }

    contact->capabilities = contact_in->capabilities;

    g_array_append_val (contacts_to_update, contact);
  }

  if (contacts_to_update->len > 0 )
  {
    update_contacts (backend, contacts_to_update, FALSE);
  }

  g_array_free (contacts_to_update, TRUE);
}

static void
tp_cl_contact_info_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts,
    gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact;
  EBookBackendTpContact *contact_in;
  guint i = 0;
  GArray *contacts_to_update = NULL;

  DEBUG ("tp_cl_contact_info_changed_cb");

  contacts_to_update = g_array_sized_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *), contacts->len);

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);
    contact = g_hash_table_lookup (priv->handle_to_contact,
        GUINT_TO_POINTER (contact_in->handle));

    if (!contact)
    {
      DEBUG ("Unknown contact with handle %d",
          contact_in->handle);
      continue;
    }

    g_free (contact->contact_info);

    contact->contact_info = g_strdup (contact_in->contact_info);
    g_array_append_val (contacts_to_update, contact);
  }

  if (contacts_to_update->len > 0 )
  {
    update_contacts (backend, contacts_to_update, TRUE);
  }

  g_array_free (contacts_to_update, TRUE);
}

/* The following code pretty much responsible for merging the state of
 * telepathy with our original imported data from the database and then later
 * reconciliating the other way. Hold on to your hats it's going to get pretty
 * hairy
*/

typedef struct
{
  EBookBackendTp *backend;
  GArray *contacts_to_add;
  GArray *contacts_to_update;
} GetMembersClosure;

static gboolean
run_update_contact (EBookBackendTp *backend, EBookBackendTpContact *contact)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  gboolean changed = FALSE;
  GError *error= NULL;

  g_object_ref (backend);
  e_book_backend_tp_contact_ref (contact);

  if (contact->pending_flags & SCHEDULE_UPDATE_FLAGS)
  {
    if (!e_book_backend_tp_cl_run_update_flags (priv->tpcl, contact, &error))
    {
      WARNING ("Error whilst updating contact flags: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    }

    /* Clear the flag. We don't want this to happen again */
    contact->pending_flags &= ~SCHEDULE_UPDATE_FLAGS;
    changed = TRUE;
  }

  if (contact->pending_flags & SCHEDULE_UNBLOCK)
  {
    if (e_book_backend_tp_cl_run_unblock_contact (priv->tpcl, contact, &error))
    {
      /* Clear the flag. We don't want this to happen again */
      contact->pending_flags &= ~SCHEDULE_UNBLOCK;
    }
    else
    {
      WARNING ("Error whilst unblocking contact: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);

      /* We couldn't unblock the contact now (maybe we are offline?), so we
       * will try the next time we connect. In the meantime we pretend to
       * not be in the deny list anymore so the UI can show the contact */
      contact->flags &= ~DENY;
    }

    changed = TRUE;
  }

  if (contact->pending_flags & SCHEDULE_UPDATE_MASTER_UID)
  {
    /* Clear the flag. We don't want this to happen again */
    contact->pending_flags &= ~SCHEDULE_UPDATE_MASTER_UID;
    changed = TRUE;
  }

  if (contact->pending_flags & SCHEDULE_UPDATE_VARIANTS)
  {
    /* Clear the flag. We don't want this to happen again */
    contact->pending_flags &= ~SCHEDULE_UPDATE_VARIANTS;
    changed = TRUE;
  }

  g_object_unref (backend);
  e_book_backend_tp_contact_unref (contact);

  return changed;
}

/* Given a contact list returns the flags for all the associated contact
 * lists, for instance ALL_FLAGS_FROM_CL(CL_SUBSCRIBE) returns
 * SUBSCRIBE | SUBSCRIBE_LOCAL_PENDING | SUBSCRIBE_REMOTE_PENDING */
#define ALL_FLAGS_FROM_CL(cl) ((1 << (cl)) | \
                               (1 << ((cl) + 1)) | \
                               (1 << ((cl) + 2)))

static gboolean
run_add_contact (EBookBackendTp *backend, EBookBackendTpContact *contact,
    GError **error_in)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  gboolean success;
  GError *error = NULL;

  e_book_backend_tp_return_val_with_error_if_fail (contact, FALSE, error_in);

  if (contact->flags & CONTACT_INVALID)
    /* We already know it's invalid, no need to test again */
    return TRUE;

  g_object_ref (backend);
  e_book_backend_tp_contact_ref (contact);

  success = e_book_backend_tp_cl_run_add_contact (priv->tpcl, contact, &error);

  if (!success)
  {
    if (error && error->domain == TP_ERROR &&
        error->code == TP_ERROR_INVALID_HANDLE)
    {
      /* The contact has an invalid ID, in this case we want to keep the
       * contact in our address book with some UI element showing it's
       * invalid.
       * Just showing an error banner is impossible as if we are offline
       * the error banner will appear only when we go online */
      g_clear_error (&error);

      contact->flags |= CONTACT_INVALID;

      /* Remove flags that would do nothing on invalid contacts */
      contact->pending_flags &= ~SCHEDULE_UPDATE_FLAGS;
      /* Remove flags for list membership */
      contact->pending_flags &= ~(
            ALL_FLAGS_FROM_CL (CL_SUBSCRIBE) |
            ALL_FLAGS_FROM_CL (CL_PUBLISH) |
            ALL_FLAGS_FROM_CL (CL_ALLOW) |
            ALL_FLAGS_FROM_CL (CL_DENY) |
            ALL_FLAGS_FROM_CL (CL_STORED)
          );

      success = TRUE;
    }
    else
    {
      g_propagate_error (error_in, error);
    }
  }

  g_object_unref (backend);
  e_book_backend_tp_contact_unref (contact);

  return success;
}

static void
merge_contacts (EBookBackendTp *backend, EBookBackendTpContact *dest,
    EBookBackendTpContact *src)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GArray *contacts_to_delete;

  /* Merge the fields that need to be preserved */
  e_book_backend_tp_contact_add_variants_from_contact (dest, src);
  e_book_backend_tp_contact_update_master_uids (dest, src->master_uids);
  /* The only interesting flag is the one to schedule unblocking */
  if (src->pending_flags & SCHEDULE_UNBLOCK)
    dest->pending_flags |= SCHEDULE_UNBLOCK;

  /* Notify of the update of the existing contact */
  if (priv->views)
  {
    EContact *ec;
    GList *l;

    ec = e_book_backend_tp_contact_to_econtact (dest, priv->vcard_field,
        priv->protocol_name);
    for (l = priv->views; l != NULL; l = l->next)
    {
      DEBUG ("notifying contact: %s", dest->name);
      e_data_book_view_notify_update ((EDataBookView *)l->data, ec);
    }

    notify_complete_all_views (backend);

    g_object_unref (ec);
  }

  /* Remove the old contact */
  contacts_to_delete = g_array_sized_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *), 1);
  g_array_append_val (contacts_to_delete, src);

  delete_contacts (backend, contacts_to_delete);

  g_array_free (contacts_to_delete, TRUE);

  /* Be sure to have existing_contact in all the tables as src could
   * have ended up there instead od dest and because delete_contact
   * could have removed the contact from some tables. */
  g_hash_table_insert (priv->handle_to_contact,
      GINT_TO_POINTER (dest->handle),
      e_book_backend_tp_contact_ref (dest));
  g_hash_table_insert (priv->name_to_contact,
      g_strdup (dest->name),
      e_book_backend_tp_contact_ref (dest));
  g_hash_table_insert (priv->uid_to_contact,
      g_strdup (dest->uid),
      e_book_backend_tp_contact_ref (dest));
}

static void finish_online_initialization (EBookBackendTp *backend);

/*
 * Phase 3:
 *
 * Here we apply pending changes to the roster that have been queued in the
 * database. We don't need the closure here. Everything we care about is in
 * the private structure.
 */
static gboolean
_sync_phase_3_idle_cb (gpointer userdata)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (userdata);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GList *l = NULL;
  GList *contacts_to_update = NULL;
  GList *contacts_to_add = NULL;
  GList *contacts_to_delete = NULL;
  EBookBackendTpContact *contact = NULL;
  GError *error = NULL;
  GArray *contacts_to_update_in_db = NULL;
  EBookBackendTpClStatus status;
  guint i;

  g_return_val_if_fail (priv->tpdb, FALSE);

  /* Check if we're still online */
  status = e_book_backend_tp_cl_get_status (priv->tpcl);
  if (status != E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    finish_online_initialization (backend);
    g_object_unref (backend);
    return FALSE;
  }

  contacts_to_update_in_db = g_array_new (TRUE, TRUE,
      sizeof (EBookBackendTpContact *));

  contacts_to_update = g_hash_table_get_values (priv->contacts_to_update);

  for (l = contacts_to_update; l; l = l->next)
  {
    contact = l->data;

    if (run_update_contact (backend, contact))
    {
      e_book_backend_tp_contact_ref (contact);
      g_array_append_val (contacts_to_update_in_db, contact);
    }
  }

  g_list_free (contacts_to_update);

  contacts_to_add = g_hash_table_get_values (priv->contacts_to_add);

  for (l = contacts_to_add; l; l = l->next)
  {
    gchar *old_name;

    contact = (EBookBackendTpContact *)l->data;
    old_name = g_strdup (contact->name);

    if (!run_add_contact (backend, contact, &error))
    {
      WARNING ("Unable to create contact: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    } else {
      contact->pending_flags &= ~SCHEDULE_ADD;

      if (strcmp (old_name, contact->name) == 0)
      {
        e_book_backend_tp_contact_ref (contact);
        g_array_append_val (contacts_to_update_in_db, contact);
      } else {
        EBookBackendTpContact *existing;
        /* The user added a contact while offline and now we discovered that
         * the normalized version of the user name is different from what the
         * user inserted */
        g_hash_table_remove (priv->name_to_contact, old_name);
        existing = g_hash_table_lookup (priv->name_to_contact, contact->name);
        if (existing) {
          /* There is already a contact with the normalized name, so let's
           * just merge them */
          merge_contacts (backend, existing, contact);
          e_book_backend_tp_contact_ref (existing);
          g_array_append_val (contacts_to_update_in_db, existing);
        } else {
          g_hash_table_insert (priv->name_to_contact, g_strdup (contact->name),
              e_book_backend_tp_contact_ref (contact));
        }
      }

      g_hash_table_remove (priv->contacts_to_add, contact->uid);

      g_free (old_name);
    }
  }

  g_list_free (contacts_to_add);

  contacts_to_delete = g_hash_table_get_values (priv->contacts_to_delete);

  for (l = contacts_to_delete; l; l = l->next)
  {
    contact = (EBookBackendTpContact *)l->data;
    MESSAGE ("Deleting contact: %s", contact->uid);
    if (!e_book_backend_tp_cl_run_remove_contact (priv->tpcl, contact, &error))
    {
      WARNING ("Unable to delete contact: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    } else {
      contact->pending_flags &= ~SCHEDULE_DELETE;

      e_book_backend_tp_contact_ref (contact);
      g_array_append_val (contacts_to_update_in_db, contact);

      g_hash_table_remove (priv->contacts_to_delete, contact->uid);
    }
  }

  g_list_free (contacts_to_delete);

  if (contacts_to_update_in_db->len > 0)
  {
    if (!e_book_backend_tp_db_update_contacts (priv->tpdb,
          contacts_to_update_in_db, &error))
    {
      g_critical ("Error whilst updating contacts in database: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    }
  }

  finish_online_initialization (backend);

  for (i = 0; i < contacts_to_update_in_db->len; i++)
  {
    EBookBackendTpContact *contact;

    contact = g_array_index (contacts_to_update_in_db,
          EBookBackendTpContact *, i);
    e_book_backend_tp_contact_unref (contact);
  }

  g_array_free (contacts_to_update_in_db, TRUE);

  g_object_unref (backend);

  return FALSE;
}

/*
 * Phase 2:
 *
 * Here we apply some of the changes that we have scheduled to apply to the
 * database. Including adding, updating and removing contacts that we find out
 * that have changed during the synchronisation process. These could be
 * considered part of phase 1 but we carry them out here inside an idle to
 * try and avoid starving out the main loop.
 */

static gboolean
_sync_phase_2_idle_cb (gpointer userdata)
{
  GetMembersClosure *closure = (GetMembersClosure *)userdata;
  EBookBackendTp *backend = closure->backend;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  guint i;
  GList *contacts, *l;
  EBookBackendTpContact *contact;
  GArray *contacts_to_delete = NULL;

  g_return_val_if_fail (priv->tpdb, FALSE);

  /* Add new contacts to the database */
  if (closure->contacts_to_add)
  {
    e_book_backend_tp_db_add_contacts (priv->tpdb, closure->contacts_to_add, NULL);

    for (i = 0; i < closure->contacts_to_add->len; i++)
    {
      contact = g_array_index (closure->contacts_to_add,
          EBookBackendTpContact *, i);
      e_book_backend_tp_contact_unref (contact);
    }

    g_array_free (closure->contacts_to_add, TRUE);
  }

  /* Update refreshed contacts in database */
  if (closure->contacts_to_update)
  {
    e_book_backend_tp_db_update_contacts (priv->tpdb, closure->contacts_to_update, NULL);

    for (i = 0; i < closure->contacts_to_update->len; i++)
    {
      contact = g_array_index (closure->contacts_to_update,
          EBookBackendTpContact *, i);
      e_book_backend_tp_contact_unref (contact);
    }

    g_array_free (closure->contacts_to_update, TRUE);
  }

  /*
   * We must iterate through all contacts looking for the unseen flag and use
   * that to know which ones to remove
   */
  contacts = g_hash_table_get_values (priv->name_to_contact);

  for (l = contacts; l != NULL; l = l->next)
  {
    contact = (EBookBackendTpContact *)l->data;

    if (contact->flags & CONTACT_UNSEEN)
    {
      if (!contacts_to_delete)
        contacts_to_delete = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

      DEBUG ("found unseen contact with uid %s and name %s",
          contact->uid, contact->name);

      g_array_append_val (contacts_to_delete, contact);
    }
  }

  if (contacts_to_delete)
  {
    delete_contacts (backend, contacts_to_delete);
    g_array_free (contacts_to_delete, TRUE);
  }

  g_list_free (contacts);

  g_idle_add (_sync_phase_3_idle_cb, g_object_ref (backend));

  g_object_unref (closure->backend);
  g_free (closure);
  return FALSE;
}

/*
 * Phase 1:
 *
 * The addition of new contacts and updates to contacts is done here. The
 * process is more complicated when you consider that we may have local
 * changes to the contacts.
 *
 * The local changes we could have to existing contacts is the change in
 * membership. e.g. something could be blocked offline or the alias can be
 * changed.
 *
 * The majority of the work for this phase is done in the tp_cl_get_members_cb
 * callback. This will fired when the data comes back from the request made in
 * the _sync_phase_1 function which is called when we have finished doing our
 * initial database population AND when we are online.
 */
static void
tp_cl_get_members_cb (EBookBackendTpCl *tpcl, GArray *contacts,
    const GError *error, gpointer userdata)
{
  EBookBackendTp *backend = (EBookBackendTp *)userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact_in = NULL;
  EBookBackendTpContact *contact = NULL;
  guint i = 0;
  GetMembersClosure *closure;

  g_return_if_fail (error || contacts);

  if (error)
  {
    WARNING ("error retrieving members of contact list: %s", error->message);
    finish_online_initialization (backend);
    g_object_unref (backend);
    return;
  }

  /* Note that we cannot just return here even if the roster is empty
   * or we will skip some needed steps, for instance we will not mark
   * for deletion unseen contacts. */

  DEBUG ("get_members called with %d contacts", contacts->len);

  closure = g_new0 (GetMembersClosure, 1);
  closure->contacts_to_add = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
  closure->contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
  closure->backend = g_object_ref (backend);

  for (i = 0; i < contacts->len; i++)
  {
    contact_in = g_array_index (contacts, EBookBackendTpContact *, i);

    /* See if we have an existing contact with this 'name' */
    contact = g_hash_table_lookup (priv->name_to_contact, contact_in->name);

    /* we've already got this contact */
    if (contact != NULL)
    {
      gboolean changed = FALSE; /* whether we need to update contact in db */

      /* Update our fields with the latest ones from the contact list */
      /* TODO: Add more fields here */

      if (contact->alias == NULL || (contact_in->alias &&
          !g_str_equal (contact->alias, contact_in->alias)))
      {
        g_free (contact->alias);
        contact->alias = g_strdup (contact_in->alias);
        changed = TRUE;
      }

      contact->generic_status = contact_in->generic_status;

      if (contact->status == NULL || (contact_in->status &&
          !g_str_equal (contact->status, contact_in->status)))
      {
        g_free (contact->status);
        contact->status = g_strdup (contact_in->status);
      }

      if (contact->status_message == NULL || (contact_in->status_message &&
          !g_str_equal (contact->status_message, contact_in->status_message)))
      {
        g_free (contact->status_message);
        contact->status_message = g_strdup (contact_in->status_message);
      }

      if (contact->contact_info == NULL || (contact_in->contact_info &&
          !g_str_equal (contact->contact_info, contact_in->contact_info)))
      {
        g_free (contact->contact_info);
        contact->contact_info = g_strdup (contact_in->contact_info);
      }

      /* Update our idea of the handle */
      contact->handle = contact_in->handle;

      if (changed)
      {
        /* Add to the array of contacts to update in the database */
        e_book_backend_tp_contact_ref (contact);
        g_array_append_val (closure->contacts_to_update, contact);
      }

      /* Remove the UNSEEN flag because it's been seen now */
      contact->flags &= ~CONTACT_UNSEEN;

      DEBUG ("Refreshing contact with handle %d and name %s",
          contact->handle, contact->name);
    } else {
      /* Woohoo a new contact. We duplicate the contact here we do this so we
       * can maintain a completely separate set of the contacts to that used
       * in the backend. I think this is the best thing to do.
       */

      contact = e_book_backend_tp_contact_dup (contact_in);

      /* Generate a UID for it */
      contact->uid = e_book_backend_tp_generate_uid (backend, contact->name);

      /* Save in the uid hash table */
      g_hash_table_insert (priv->uid_to_contact, g_strdup (contact->uid),
          e_book_backend_tp_contact_ref (contact));

      /* Save in the name hash table */
      g_hash_table_insert (priv->name_to_contact, g_strdup (contact->name),
          e_book_backend_tp_contact_ref (contact));

      /* Save for adding to the database (leave ownership of the contact) */
      g_array_append_val (closure->contacts_to_add, contact);

      DEBUG ("New contact with handle %d and name %s",
          contact->handle, contact->name);
    }

    /* Add to the handle lookup table */
    if (g_hash_table_lookup (priv->handle_to_contact,
          GINT_TO_POINTER (contact_in->handle)) == NULL)
    {
      g_hash_table_insert (priv->handle_to_contact,
          GINT_TO_POINTER (contact_in->handle),
          e_book_backend_tp_contact_ref (contact));
    } else {
      WARNING ("duplicate contact for handle: %d found", contact_in->handle);
    }
  }

  request_avatar_data_for_offline_contacts (backend, closure->contacts_to_add);

  if (!priv->views)
  {
    DEBUG ("no known views; will notify about members later");
  }

  g_idle_add (_sync_phase_2_idle_cb, closure);

  g_object_unref (backend);
}

static void
_sync_phase_1 (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GError *error = NULL;

  DEBUG ("online and getting members");

  if (priv->is_loading) {
    /* The process of syncing the contacts with the roster is completely
     * async and split in multiple functions, so it can happen that we
     * disconnect and reconnect again before this process ends.
     * In this case a new loading starts before the previous sequence
     * terminates, but we want to avoid this to avoid any mess up.
     * We just skip the second loading process and wait for the first one
     * to finish/fail by itself. At this point a new loading process will
     * be started by finish_online_initialization. */
    DEBUG ("already in the loading stage, giving up for now");
    priv->need_contacts_reload = TRUE;
    return;
  }

  priv->is_loading = TRUE;

  if (!e_book_backend_tp_cl_get_members (priv->tpcl, tp_cl_get_members_cb,
          g_object_ref (backend), &error))
  {
    WARNING ("Error when asking for members: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    finish_online_initialization (backend);
    g_object_unref (backend);
  }
}

static void
finish_online_initialization (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);

  priv->is_loading = FALSE;

  if (priv->need_contacts_reload)
  {
    priv->need_contacts_reload = FALSE;
    _sync_phase_1 (backend);
  }
}

/*
 * Phase 0
 *
 * Cheekily calling this phase 0 because it's important in the synchronisation
 * prcoess but isn't actually part of it itself since we might not actually
 * get online ever in any given session.
 *
 * This code is responsible for doing the initial population from the
 * database. And then once that is completed waiting for a status change so
 * that we go online.
 *
 * This is process is started when the backend in loaded.
 */
static void
tp_cl_status_changed_cb (EBookBackendTpCl *tpcl, EBookBackendTpClStatus status,
    gpointer userdata)
{
  EBookBackendTp *backend = (EBookBackendTp *)userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);

  DEBUG ("Status changed to %s",
      status == E_BOOK_BACKEND_TP_CL_ONLINE ? "online" : "offline");

  /* Ahaha. We are online now so we can start the synchronisation process */
  if (status == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    _sync_phase_1 (backend);
  }
  else
  {
    /* Empty the handle table as handles are useful only as long as the
     * account is online */
    g_hash_table_remove_all (priv->handle_to_contact);
  }
}

static void
tp_ready_cb (EBookBackendTp *backend, gpointer userdata)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpClStatus status;

  MESSAGE ("database import complete, we're ready");

  /* Now that we are ready lets listen for the status changed signal */
  g_signal_connect (priv->tpcl, "status-changed",
      (GCallback)tp_cl_status_changed_cb, backend);

  g_signal_connect (priv->tpcl, "aliases-changed",
      (GCallback)tp_cl_aliases_changed_cb, backend);

  g_signal_connect (priv->tpcl, "presences-changed",
      (GCallback)tp_cl_presence_changed_cb, backend);

  g_signal_connect (priv->tpcl, "flags-changed",
      (GCallback)tp_cl_flags_changed, backend);

  g_signal_connect (priv->tpcl, "contacts-added",
      (GCallback)tp_cl_contacts_added, backend);

  g_signal_connect (priv->tpcl, "contacts-removed",
      (GCallback)tp_cl_contacts_removed, backend);

  g_signal_connect (priv->tpcl, "avatar-tokens-changed",
      (GCallback)tp_cl_avatar_tokens_changed_cb, backend);

  g_signal_connect (priv->tpcl, "avatar-data-changed",
      (GCallback)tp_cl_avatar_data_changed_cb, backend);

  g_signal_connect (priv->tpcl, "capabilities-changed",
      (GCallback)tp_cl_capabilities_changed_cb, backend);

  g_signal_connect (priv->tpcl, "contact-info-changed",
      (GCallback)tp_cl_contact_info_changed_cb, backend);

  status = e_book_backend_tp_cl_get_status (priv->tpcl);

  /* Signal that we're ready to pass along the cached contacts; additional
   * contacts and their changes will follow */
  priv->members_ready = TRUE;
  g_signal_emit_by_name (backend, "members-ready");

  /* Of course if we are already online we should grab the contacts anyway */
  if (status == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    DEBUG ("online");
    _sync_phase_1 (backend);
  } else {
    DEBUG ("not online");
  }
}

/* TODO: May need to split this into a set of idle callback functions not to
 * starve the mainloop */
static gboolean
_sync_phase_0_idle_cb (gpointer userdata)
{
  EBookBackendTp *backend = (EBookBackendTp *)userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GArray *contacts = NULL;
  EBookBackendTpContact *contact = NULL;
  guint i = 0;

  g_return_val_if_fail (priv->tpdb, FALSE);

  /* We need to know when the database import is complete. So we can
   * consider moving to phase 1 but only when ready*/
  g_signal_connect (backend, "ready", (GCallback)tp_ready_cb, userdata);

  /* TODO: GError foo */
  contacts = e_book_backend_tp_db_fetch_contacts (priv->tpdb, NULL);

  if (contacts != NULL)
  {
    DEBUG ("retrieved %d contacts from database", contacts->len);

    /*
     * Import the contacts from the database into the initial set of hash
     * tables
     */
    for (i = 0; i < contacts->len; i++)
    {
      contact = g_array_index (contacts, EBookBackendTpContact *, i);
      g_hash_table_insert (priv->uid_to_contact,
          g_strdup (contact->uid),
          e_book_backend_tp_contact_ref (contact));
      g_hash_table_insert (priv->name_to_contact,
          g_strdup (contact->name),
          e_book_backend_tp_contact_ref (contact));

      if (contact->pending_flags & SCHEDULE_DELETE)
      {
        g_hash_table_insert (priv->contacts_to_delete,
            g_strdup (contact->uid),
            e_book_backend_tp_contact_ref (contact));
      }

      if (contact->pending_flags & SCHEDULE_ADD)
      {
        g_hash_table_insert (priv->contacts_to_add,
            g_strdup (contact->uid),
            e_book_backend_tp_contact_ref (contact));
      }

      if (contact->pending_flags & SCHEDULE_UPDATE_FLAGS
          || contact->pending_flags & SCHEDULE_UNBLOCK)
      {
        g_hash_table_insert (priv->contacts_to_update,
            g_strdup (contact->uid),
            e_book_backend_tp_contact_ref (contact));
      }

      if (!(contact->pending_flags & SCHEDULE_ADD) &&
          !(contact->flags & CONTACT_INVALID))
      {
        /* Marking as 'UNSEEN' so we know what to remove */
        contact->flags |= CONTACT_UNSEEN;
      }

      /* We don't need the reference ourselves anymore */
      e_book_backend_tp_contact_unref (contact);
    }

    g_array_free (contacts, TRUE);
  }

  /* Fire the signal so that any 'pending' book views can do their thing. */
  g_signal_emit_by_name (backend, "ready");

  /* The reference was added by account_compat_ready_cb */
  g_object_unref (backend);

  return FALSE;
}

/* backend implementations */
static void
account_ready_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  TpAccount *account = (TpAccount *) object;
  EBookBackendTp *backend = user_data;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GError *error = NULL;
  const gchar *path_suffix;

  if (!tp_proxy_prepare_finish (object, res, &error))
  {
    WARNING ("Error preparing account %s: %s\n",
             tp_account_get_path_suffix (account), error->message);
    goto error_out;
  }

  if (!priv->tpdb)
  {
    g_warn_if_fail (priv->tpdb);
    goto error_out;
  }

  path_suffix = tp_account_get_path_suffix (account);

  if (!e_book_backend_tp_db_open (priv->tpdb, path_suffix, &error))
  {
    g_critical ("Error when opening database: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    goto error_out;
  }

  if (!e_book_backend_tp_cl_load (priv->tpcl, priv->account, &error))
  {
    g_critical ("Error when loading the contact list: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    goto error_out;
  }

  /* This idle will populate from the database and when it has done so fire
   * the 'ready' signal */
  g_idle_add (_sync_phase_0_idle_cb, backend);

  return;

error_out:
  /* Even implementing the async interface EDS wants use to do the load in a
   * sync way. This is not possible using Telepathy so we do the real load
   * asynchronously but we lose the ability to report proper errors if
   * something goes wrong. priv->load_error is used to prevent backend
   * methods to be called is the load didn't succeed. */
  priv->load_error = TRUE;
  g_object_unref (backend);
}


static void
cm_ready_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  TpConnectionManager *cm = (TpConnectionManager *) object;
  TpProtocol *protocol;
  EBookBackendTp *backend = user_data;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (object, res, &error))
  {
    WARNING ("Error preparing account connection manager %s: %s\n",
             tp_account_get_path_suffix (priv->account), error->message);
    goto error_out;
  }

  protocol =
      tp_connection_manager_get_protocol_object (cm, priv->protocol_name);

  priv->vcard_field = g_strdup (tp_protocol_get_vcard_field (protocol));
  g_object_unref(cm);

  tp_proxy_prepare_async (priv->account, NULL, account_ready_cb, backend);

  return;

error_out:
  priv->load_error = TRUE;
  g_object_unref (cm);
  g_object_unref (backend);
}

static void
set_inactivity_status (EBookBackendTp *backend, gboolean inactivity)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);

  if (inactivity != priv->system_inactive)
  {
    priv->system_inactive = inactivity;
    if (!priv->system_inactive &&
        g_hash_table_size (priv->contacts_remotely_changed) > 0)
    {
      DEBUG ("back from inactivity, notifying pending contacts");
      notify_remotely_updated_contacts_and_complete (backend);
    }
  }
}

static void
get_inactivity_status_cb (DBusGProxy *proxy, DBusGProxyCall *call, void *userdata)
{
  EBookBackendTp *backend = userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  gboolean status;

  if (dbus_g_proxy_end_call (proxy, call, NULL, G_TYPE_BOOLEAN, &status, G_TYPE_INVALID))
    set_inactivity_status (backend, status);

  g_object_unref (priv->mce_request_proxy);
  priv->mce_request_proxy = NULL;
  g_object_unref (backend);
}

static gboolean
e_book_backend_tp_open_sync (EBookBackendSync *backend,
                             GCancellable *cancellable,
                             GError **error)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  ESource *source;
  TpDBusDaemon *dbus_daemon = NULL;
  TpSimpleClientFactory *factory;
  gchar *object_path;
  const gchar *uid = NULL;
  ESourceResource *resource;

  /* e_book_backend_tp_load_source can be called more than once so we have to
   * avoid problems with it. */
  if (priv->load_started)
    return TRUE;

  priv->load_started = TRUE;

  source = e_backend_get_source (E_BACKEND (backend));

  if (!e_source_has_extension (source, E_SOURCE_EXTENSION_RESOURCE))
  {
    *error = EC_ERROR_EX (INVALID_ARG,
                           "no source extension E_SOURCE_EXTENSION_RESOURCE");
  }
  else
  {
    resource = e_source_get_extension (source, E_SOURCE_EXTENSION_RESOURCE);
    uid = e_source_resource_get_identity (resource);

    e_book_backend_set_writable (E_BOOK_BACKEND (backend), TRUE);

    dbus_daemon = tp_dbus_daemon_dup (NULL);
    factory = tp_simple_client_factory_new (dbus_daemon);

    object_path = g_strconcat (TP_ACCOUNT_OBJECT_PATH_BASE, uid, NULL);
    priv->account = tp_simple_client_factory_ensure_account (
          factory, object_path, NULL, error);
    g_free (object_path);
    g_object_unref (factory);
  }

  if (*error)
  {
    WARNING ("Unable to open telepathy account: %s - %s", uid,
             (*error)->message);
    goto err;
  }
  else
  {
    const gchar *cm_name = tp_account_get_cm_name (priv->account);
    TpConnectionManager *manager = tp_connection_manager_new (
          dbus_daemon, cm_name, NULL, error);

    priv->protocol_name =
        g_strdup (tp_account_get_protocol_name (priv->account));
    tp_proxy_prepare_async (manager, NULL, cm_ready_cb, g_object_ref (backend));
  }

  g_object_unref (dbus_daemon);

  priv->mce_request_proxy = dbus_g_proxy_new_for_name (
      e_book_backend_tp_system_bus_connection,
      MCE_SERVICE, MCE_REQUEST_PATH, MCE_REQUEST_IF);
  dbus_g_proxy_begin_call (priv->mce_request_proxy, MCE_INACTIVITY_STATUS_GET,
      get_inactivity_status_cb, g_object_ref (backend), NULL, G_TYPE_INVALID);

  return TRUE;

err:
  if (dbus_daemon)
    g_object_unref (dbus_daemon);

  return FALSE;
}

static gchar *
e_book_backend_tp_get_backend_property (EBookBackend *backend,
                                        const gchar *prop_name)
{
  g_return_val_if_fail (prop_name != NULL, NULL);

  if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES))
    return g_strdup ("local,do-initial-query,contact-lists");

  /* Chain up to parent's get_backend_property() method. */
  return E_BOOK_BACKEND_CLASS (e_book_backend_tp_parent_class)->
          impl_get_backend_property (backend, prop_name);

}
/* Stream the contacts over to the client.in the view. */

typedef struct {
    /* g_utf8_collate_key can remove spaces so we cannot just concatenate
     * the first name with the last name */
    gchar *tag1;
    gchar *tag2;
    EContact *econtact;
} ContactSortData;

static ContactSortData *
contact_sort_data_new (EContact *ec, ContactSortOrder sort_order,
    const gchar *vcard_field)
{
  ContactSortData *data;
  const gchar *first;
  const gchar *last;
  const gchar *tag1 = NULL;
  const gchar *tag2 = NULL;
  gchar *tmp;
  gboolean free_tag1 = FALSE;

  data = g_new0 (ContactSortData, 1);
  data->econtact = g_object_ref (ec);

  if (sort_order == CONTACT_SORT_ORDER_LAST_FIRST ||
      sort_order == CONTACT_SORT_ORDER_FIRST_LAST) {
    first = e_contact_get_const (ec, E_CONTACT_GIVEN_NAME);
    last = e_contact_get_const (ec, E_CONTACT_FAMILY_NAME);

    if (first && last) {
        /* We have both first and last name */
        if (sort_order == CONTACT_SORT_ORDER_FIRST_LAST) {
            tag1 = first;
            tag2 = last;
        } else {
            tag1 = last;
            tag2 = first;
        }

        goto done;
    }

    if (first) {
        /* Only first name, just use it */
        tag1 = first;
        goto done;
    }

    if (last) {
        /* Only last name, just use it */
        tag1 = last;
        goto done;
    }

    /* No name fields, fallback to the nickname */
  }

  tag1 = e_contact_get_const (ec, E_CONTACT_NICKNAME);
  if (!tag1) {
      /* If the nickname (i.e. Telepathy alias) is not set then just use the
       * JID (or any other contact ID for other protocols) as a fallback */
      EVCardAttribute *attr;

      attr = e_vcard_get_attribute (E_VCARD (ec), vcard_field);
      tag1 = e_vcard_attribute_get_value (attr);
      free_tag1 = TRUE;
  }

done:
  tmp = g_utf8_casefold (tag1, -1);
  data->tag1 = g_utf8_collate_key (tmp, -1);
  g_free (tmp);

  if (tag2) {
    tmp = g_utf8_casefold (tag2, -1);
    data->tag2 = g_utf8_collate_key (tmp, -1);
    g_free (tmp);
  }

  /* tag1 can come from e_vcard_attribute_get_value that allocates a new
   * string instead of e_contact_get_const */
  if (free_tag1)
    g_free ((gchar *)tag1);

  return data;
}

static void
contact_sort_data_free (ContactSortData *data)
{
  if (!data)
    return;

  g_free (data->tag1);
  g_free (data->tag2);
  g_object_unref (data->econtact);
  g_free (data);
}

static gint
contact_sort_data_compare (const ContactSortData **pa, const ContactSortData **pb)
{
  const ContactSortData *a = *pa;
  const ContactSortData *b = *pb;
  gint cmp;

  cmp = g_strcmp0 (a->tag1, b->tag1);
  if (cmp == 0)
    cmp = g_strcmp0 (a->tag2, b->tag2);

  return cmp;
}

/* TODO: Should we perhaps split this up OR spin it into a separate thread to
 * allow freeze-thaw.
 */
static void
notify_all_contacts_updated_for_view (EBookBackendTp *backend,
                                      EDataBookView *book_view)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GPtrArray *all_contacts;
  ContactSortOrder sort_order;
  GHashTableIter iter;
  gpointer contact_pointer;
  guint i;

  DEBUG ("sending contacts");

  /* This function is called when the members just become ready, so
   * there should be no pending notifications, or when a new view is
   * added, so the pending notification have been already flushed
   * before adding this view */
  if (g_hash_table_size (priv->contacts_remotely_changed))
    g_critical ("There are pending contacts that have not been sent to "
        "the views");

  all_contacts = g_ptr_array_sized_new (g_hash_table_size (priv->uid_to_contact));

  /* If for some reason the sort order was not set then g_object_get_data will
   * return NULL, that will be casted to CONTACT_SORT_ORDER_FIRST_LAST. This
   * is fine as it's a good default. */
  sort_order = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (book_view),
        BOOK_VIEW_SORT_ORDER_DATA_KEY));

  g_hash_table_iter_init (&iter, priv->uid_to_contact);
  while (g_hash_table_iter_next (&iter, NULL, &contact_pointer)) {
    EBookBackendTpContact *contact = contact_pointer;

    if (e_book_backend_tp_contact_is_visible (contact)) {
      EContact *ec;

      ec = e_book_backend_tp_contact_to_econtact (contact, priv->vcard_field,
          priv->protocol_name);
      g_ptr_array_add (all_contacts,
          contact_sort_data_new (ec, sort_order, priv->vcard_field));
      g_object_unref (ec);
    }
  }

  g_ptr_array_sort (all_contacts, (GCompareFunc) contact_sort_data_compare);

  for (i  = 0; i < all_contacts->len; i++) {
    ContactSortData *data = g_ptr_array_index (all_contacts, i);
    e_data_book_view_notify_update (book_view, data->econtact);
    contact_sort_data_free (data);
  }

  g_ptr_array_free (all_contacts, TRUE);

  e_data_book_view_notify_complete (book_view, NULL);
}

static void
notify_all_contacts_updated (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GList *l;

  DEBUG ("notifying book views about members");

  for (l = priv->views; l != NULL; l = l->next)
    notify_all_contacts_updated_for_view (backend, l->data);
}

static void
book_view_tp_members_ready_cb (EBookBackendTp *backend, gpointer userdata)
{
  DEBUG ("members ready to send through book view (callback)");

  notify_all_contacts_updated (backend);
}

typedef struct
{
  EBookBackendTp *backend;
  EDataBookView *book_view;
} BookViewClosure;

static gboolean
start_book_view_idle_cb (gpointer userdata)
{
  BookViewClosure *closure = (BookViewClosure *)userdata;
  EBookBackendTp *backend = (EBookBackendTp *)closure->backend;
  EBookBackendTpPrivate *priv = GET_PRIVATE (closure->backend);

  DEBUG ("book view idle callback");

  if (priv->load_error)
  {
    g_critical ("the book was not loaded correctly so the book view cannot"
        "be started");

    e_data_book_view_notify_complete (closure->book_view,
                                      EC_ERROR (OTHER_ERROR));

    goto done;
  }

  /* Flush pending notifications to keep existing views consistent with
   * newly-started view */
  notify_remotely_updated_contacts_and_complete (backend);

  /* Store the list of views that we have since we need this to notify of
   * changes, etc.
   */
  priv->views = g_list_append (priv->views, closure->book_view);
  g_object_ref (closure->book_view);

  /* If we have finished import and have the members, send them along; otherwise
   * setup a callback to be fired when we are ready.
   */
  if (priv->members_ready)
  {
    DEBUG ("members ready to send through book view (immediate)");
    notify_all_contacts_updated_for_view (backend, closure->book_view);
  } else if (!priv->members_ready_signal_id) {
    priv->members_ready_signal_id = g_signal_connect (backend,
        "members-ready", (GCallback)book_view_tp_members_ready_cb, NULL);
  }

done:
  g_object_unref (closure->backend);
  g_object_unref (closure->book_view);
  g_free (closure);

  return FALSE;
}

/*
 * Create a closure, populate with all the objects we need and then spin off
 * into the mainloop via an idle.
 */
static void
e_book_backend_tp_start_view (EBookBackend *backend, EDataBookView *book_view)
{
  BookViewClosure *closure;

  closure = g_new0 (BookViewClosure, 1);
  closure->backend = (EBookBackendTp *)backend;
  closure->book_view = book_view;

  g_object_ref (closure->backend);
  g_object_ref (closure->book_view);

  g_idle_add (start_book_view_idle_cb, closure);
}

/* idle to avoid thread pain */
static gboolean
stop_book_view_idle_cb (gpointer userdata)
{
  BookViewClosure *closure = (BookViewClosure *)userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (closure->backend);

  if (priv->load_error)
  {
    g_critical ("the book was not loaded correctly so the book view cannot "
        "be stopped");
    goto done;
  }

  flush_db_updates (closure->backend);

  priv->views = g_list_remove (priv->views, closure->book_view);
  g_object_unref (closure->book_view);

done:
  g_object_unref (closure->book_view);
  g_object_unref (closure->backend);
  g_free (closure);

  return FALSE;
}

static void
e_book_backend_tp_stop_view (EBookBackend *backend, EDataBookView *book_view)
{
  BookViewClosure *closure = NULL;

  closure = g_new0 (BookViewClosure, 1);
  closure->book_view = g_object_ref (book_view);
  closure->backend = (EBookBackendTp *)g_object_ref (backend);

  g_idle_add (stop_book_view_idle_cb, closure);
}

static DBusHandlerResult
message_filter (DBusConnection *connection, DBusMessage *message,
    gpointer userdata)
{
  const char *tmp;
  GQuark interface, member;
  int message_type;
  EBookBackendTp *backend = userdata;

  tmp = dbus_message_get_interface (message);
  interface = tmp ? g_quark_try_string (tmp) : 0;

  tmp = dbus_message_get_member (message);
  member = tmp ? g_quark_try_string (tmp) : 0;

  message_type = dbus_message_get_type (message);

  if (interface == mce_signal_interface_quark &&
      message_type == DBUS_MESSAGE_TYPE_SIGNAL &&
      member == mce_inactivity_signal_quark)
  {
    gboolean new_state;
    dbus_message_get_args (message, NULL, DBUS_TYPE_BOOLEAN, &new_state,
        DBUS_TYPE_INVALID);
    set_inactivity_status (backend, new_state);
  }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* object class implementations */
static void
e_book_backend_tp_dispose (GObject *object)
{
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (object);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  DBusConnection *connection;

  flush_db_updates (backend);

  g_signal_handlers_disconnect_matched (priv->tpcl, G_SIGNAL_MATCH_DATA, 0, 0,
      NULL, NULL, object);

  connection = dbus_g_connection_get_connection (
      e_book_backend_tp_system_bus_connection);
  dbus_connection_remove_filter (connection, message_filter, backend);
  dbus_bus_remove_match (connection, INVACTIVITY_MATCH_RULE, NULL);

  if (priv->account)
  {
    g_object_unref (priv->account);
    priv->account = NULL;
  }

  if (priv->vcard_field)
  {
    g_free (priv->vcard_field);
    priv->vcard_field = NULL;
  }

  if (priv->protocol_name)
  {
    g_free (priv->protocol_name);
    priv->protocol_name = NULL;
  }

  g_object_unref (priv->tpcl);

  if (priv->tpdb)
  {
    e_book_backend_tp_db_close (priv->tpdb, NULL);
    g_object_unref (priv->tpdb);
    priv->tpdb = NULL;
  }

  if (priv->mce_request_proxy)
  {
    g_object_unref (priv->mce_request_proxy); /* this cancels pending calls */
    priv->mce_request_proxy = NULL;
  }

  g_hash_table_unref (priv->uid_to_contact);
  g_hash_table_unref (priv->name_to_contact);
  g_hash_table_unref (priv->handle_to_contact);

  g_hash_table_unref (priv->contacts_to_delete);
  g_hash_table_unref (priv->contacts_to_update);
  g_hash_table_unref (priv->contacts_to_add);

  g_hash_table_unref (priv->contacts_to_update_in_db);

  g_hash_table_unref (priv->contacts_remotely_changed);
  if (priv->contacts_remotely_changed_update_id)
    g_source_remove (priv->contacts_remotely_changed_update_id);

  G_OBJECT_CLASS (e_book_backend_tp_parent_class)->dispose (object);
}

static void
e_book_backend_tp_finalize (GObject *object)
{
}

static void
e_book_backend_tp_init (EBookBackendTp *backend)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  gchar *avatar_dir;
  DBusConnection *connection;

  priv->tpcl = e_book_backend_tp_cl_new ();
  priv->tpdb = e_book_backend_tp_db_new ();

  priv->uid_to_contact = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);
  priv->name_to_contact = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);
  priv->handle_to_contact = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) e_book_backend_tp_contact_unref);
  priv->contacts_to_delete = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);
  priv->contacts_to_update = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);
  priv->contacts_to_add = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);

  priv->contacts_to_update_in_db = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);

  priv->contacts_remotely_changed = g_hash_table_new_full (g_str_hash,
      g_str_equal, g_free, (GDestroyNotify) e_book_backend_tp_contact_unref);

  /* Create the avatar directory */
  avatar_dir = g_build_filename (g_get_home_dir (), ".osso-abook", "avatars",
      NULL);
  if (g_mkdir_with_parents (avatar_dir, 0755) < 0)
  {
    WARNING ("Error creating avatar directory: %s",
        g_strerror (errno));
  }
  g_free (avatar_dir);

  /* Set up the stuff needed to get notifications when the device
   * is idle */
  if (!mce_signal_interface_quark)
  {
    mce_signal_interface_quark = g_quark_from_static_string (MCE_SIGNAL_IF);
    mce_inactivity_signal_quark = g_quark_from_static_string (
        MCE_INACTIVITY_SIG);
  }

  connection = dbus_g_connection_get_connection (
      e_book_backend_tp_system_bus_connection);
  dbus_connection_add_filter (connection, message_filter, backend, NULL);
  dbus_bus_add_match (connection, INVACTIVITY_MATCH_RULE, NULL);
}

typedef struct
{
  EBookBackend *backend;
  EContact *contact;
  EDataBook *book;
  guint32 opid;
} ModifyContactClosure;

static gboolean
modify_contact_idle_cb (gpointer userdata)
{
  ModifyContactClosure *closure = (ModifyContactClosure *)userdata;
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (closure->backend);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  const gchar *uid = NULL;
  EBookBackendTpContact *contact = NULL;
  EBookBackendTpClStatus tpcl_status;
  EContact *updated_econtact;
  GError *error = NULL;

  if (priv->load_error)
  {
    g_critical ("the book was not loaded correctly so the contact cannot "
        "be modified");
    error = EC_ERROR (INVALID_ARG);
    goto done;
  }

  notify_remotely_updated_contacts_and_complete (backend);

  if (!priv->tpdb)
  {
    error = EBC_ERROR (NO_SUCH_BOOK) ;
    goto done;
  }

  if (!e_book_backend_tp_db_check_available_disk_space ())
  {
    error = EBC_ERROR (NO_SPACE);
    goto done;
  }

  flush_db_updates (backend);

  uid = e_contact_get_const (closure->contact, E_CONTACT_UID);

  if (uid)
  {
    contact = g_hash_table_lookup (priv->uid_to_contact, uid);

    if (contact)
    {
      /* FIXME - do not call e_vcard_to_string if tmp is not going to be
         printed */
      gchar *tmp =
          e_vcard_to_string (E_VCARD (closure->contact), EVC_FORMAT_VCARD_30);
      MESSAGE ("Modifying contact: %s", contact->name);
      DEBUG ("%s", tmp);
      g_free (tmp);

      /* update our contact in place, noting any changes */
      e_book_backend_tp_contact_update_from_econtact (contact,
          closure->contact, priv->vcard_field);

      DEBUG ("pending flags: %x %x", contact->pending_flags,
             SCHEDULE_UPDATE_MASTER_UID);

      if (contact->pending_flags & SCHEDULE_UPDATE_FLAGS
          || contact->pending_flags & SCHEDULE_UNBLOCK
          || contact->pending_flags & SCHEDULE_UPDATE_MASTER_UID
          || contact->pending_flags & SCHEDULE_UPDATE_VARIANTS)
      {
        GError *update_error = NULL;

        tpcl_status = e_book_backend_tp_cl_get_status (priv->tpcl);

        if (tpcl_status == E_BOOK_BACKEND_TP_CL_ONLINE)
        {
          if (contact->pending_flags & SCHEDULE_UPDATE_FLAGS)
          {
            if (!e_book_backend_tp_cl_run_update_flags (priv->tpcl, contact,
                                                        &update_error))
            {
              WARNING ("Error whilst trying to update flags: %s",
                  update_error ? update_error->message : "unknown error");
              g_clear_error (&update_error);
            } else {
              contact->pending_flags &= ~SCHEDULE_UPDATE_FLAGS;
            }
          }
        }

        g_hash_table_insert (priv->contacts_to_update,
            g_strdup (contact->uid),
            e_book_backend_tp_contact_ref (contact));

        if (!e_book_backend_tp_db_update_contact (priv->tpdb, contact,
                                                  &update_error))
        {
          WARNING ("Error whilst updating database contact: %s",
              update_error ? update_error->message : "unknown error");
          g_clear_error (&update_error);
        }
        else
        {
          contact->pending_flags &= ~SCHEDULE_UPDATE_MASTER_UID;
          contact->pending_flags &= ~SCHEDULE_UPDATE_VARIANTS;
        }

        notify_updated_contact (backend, contact);
      }
    }
    else
    {
      WARNING ("Unknown uid (%s) on submitted vcard", uid);
      error = EC_ERROR (INVALID_ARG);
    }
  } else {
    WARNING ("No uid found on submitted vcard");
    error = EC_ERROR (INVALID_ARG);;
  }

done:
  if (error == NULL)
  {
    GSList modified_contacts;

    updated_econtact = e_book_backend_tp_contact_to_econtact (
          contact, priv->vcard_field, priv->protocol_name);
    modified_contacts.data = updated_econtact;
    modified_contacts.next = NULL;
    e_data_book_respond_modify_contacts (closure->book, closure->opid, NULL,
                                         &modified_contacts);
    g_object_unref(updated_econtact);
  }
  else
  {
    e_data_book_respond_modify_contacts (closure->book, closure->opid, error,
                                         NULL);
  }

  g_object_unref (closure->contact);
  g_object_unref (closure->book);
  g_object_unref (closure->backend);
  g_free (closure);

  return FALSE;
}



static void
e_book_backend_tp_modify_contacts (EBookBackend *backend, EDataBook *book,
                                   guint32 opid, GCancellable *cancellable,
                                   const gchar * const *vcards, guint32 opflags)
{
  // XXX: opflags?
  ModifyContactClosure *closure;
  guint vcard_len = g_strv_length((gchar**) vcards);

  if (vcard_len > 1)
  {
    e_data_book_respond_modify_contacts (
          book, opid,
          EC_ERROR_EX (NOT_SUPPORTED,
                        "The backend does not support bulk modifications"),
          NULL);

          return;
  }

  const gchar *vcard = vcards[0];

  g_critical ("Modifying contacts is not supported for IM contacts and "
      "can lead to race conditions. Just add the same contacts again to "
      "update it");

  closure = g_new0 (ModifyContactClosure, 1);

  closure->backend = g_object_ref (backend);
  closure->book = g_object_ref (book);
  closure->contact = e_contact_new_from_vcard (vcard);
  closure->opid = opid;

  g_idle_add (modify_contact_idle_cb, closure);
}

typedef struct
{
  EBookBackend *backend;
  EContact *econtact;
  EDataBook *book;
  guint32 opid;
} CreateContactClosure;

static EBookBackendTpContact *
run_create_contact (EBookBackendTp *backend, EContact *econtact, GError **error_out)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact = NULL;
  GError *error = NULL;
  EBookBackendTpContact *existing_contact = NULL;

  g_return_val_if_fail (priv->tpdb, FALSE);

  g_object_ref (backend);

  flush_db_updates (backend);

  /* create our contact, populate with a UID and also with details from the
   * vcard */
  contact = e_book_backend_tp_contact_new ();

  if (!e_book_backend_tp_contact_update_from_econtact (contact, econtact,
        priv->vcard_field))
  {
    g_object_unref (backend);
    e_book_backend_tp_contact_unref (contact);
    return NULL;
  }

  contact->uid = e_book_backend_tp_generate_uid (backend, contact->name);

  contact->pending_flags |= SCHEDULE_ADD;
  contact->pending_flags |= SUBSCRIBE | PUBLISH | STORED;

  if (e_book_backend_tp_cl_get_status (priv->tpcl) == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    if (!run_add_contact (backend, contact, &error))
    {
      WARNING ("Error whilst creating contact: %s",
          error ? error->message : "unknown error");
      g_propagate_error (error_out, error);

      g_object_unref (backend);
      e_book_backend_tp_contact_unref (contact);
      return NULL;
    }

    /* Don't remove the SCHEDULE_ADD flag:
     * We probably have to store some master UID.
     */
  }

  /* Lets check to see if we already have a contact with this name before. We
   * can cheat and just return our existing contact back through EDS.
   *
   * We do this here. After perhaps trying to add in order to potentially
   * resolve the normalised name (which gets written back into the contact
   * field.)
   *
   * However even if we aren't online we want to look for other contacts that
   * also have been added with the same name.
   */
  existing_contact = g_hash_table_lookup (priv->name_to_contact, contact->name);

  if (existing_contact)
  {
    /* So we already had a contact with this name.
     *
     * The e_book_add_contact() function also is used to atomically add master
     * contact UIDs: Calling e_book_get_contact() and e_book_commit_contact()
     * sequencially bears the risk of loosing updates.
     *
     * Therefore we now check if this duplicate contact introduces new master
     * contact UIDS and if that's the case we update the database.
     */
    if (!e_book_backend_tp_contact_update_master_uids (existing_contact, contact->master_uids))
    {
      DEBUG ("Trying to add a contact with a duplicate name");
      contact->pending_flags &= ~SCHEDULE_ADD;
    }

    e_book_backend_tp_contact_add_variants_from_contact (
        existing_contact, contact);

    e_book_backend_tp_contact_unref (contact);
    contact = e_book_backend_tp_contact_ref (existing_contact);

    /* The add function is also called to unblock blocked contacts, so if
     * the contact is in the deny list we unblock it */
    if (contact->flags & DENY)
    {
      contact->pending_flags &= ~DENY;
      contact->pending_flags |= SCHEDULE_UNBLOCK;
    }

    if (run_update_contact (backend, contact))
    {
      if (!e_book_backend_tp_db_update_contact (priv->tpdb, contact, &error))
      {
        g_critical ("Error whilst updating contact in database: %s",
            error ? error->message : "unknown error");
        g_clear_error (&error);
      }

      notify_updated_contact (backend, contact);
    }
  } else {
    /* Add to our main tables */
    g_hash_table_insert (priv->uid_to_contact,
        g_strdup (contact->uid),
        e_book_backend_tp_contact_ref (contact));
    g_hash_table_insert (priv->name_to_contact,
        g_strdup (contact->name),
        e_book_backend_tp_contact_ref (contact));

    if (contact->pending_flags & SCHEDULE_ADD)
    {
      g_hash_table_insert (priv->contacts_to_add,
          g_strdup (contact->uid), e_book_backend_tp_contact_ref (contact));

      if (!e_book_backend_tp_db_add_contact (priv->tpdb, contact, &error))
      {
        g_critical ("Error adding contact to database: %s",
            error ? error->message : "unknown error");
        g_propagate_error (error_out, error);

        e_book_backend_tp_contact_unref (contact);
        g_object_unref (backend);

        return NULL;
      }
    }
  }

  g_object_unref (backend);
  return contact;
}

typedef struct
{
  EBookBackend *backend;
  GSList *econtacts; /* GSList of EContact* */
  EDataBook *book;
  guint32 opid;
} CreateContactsClosure;

static gboolean
create_contacts_idle_cb (gpointer userdata)
{
  CreateContactsClosure *closure = (CreateContactsClosure *)userdata;
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (closure->backend);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  GSList *econtact_in;
  GSList *econtacts = NULL;
  EBookClientError status;
  gboolean status_ok = TRUE;
  GError *error = NULL;

  if (priv->load_error)
  {
    g_critical ("the book was not loaded correctly so the contacts cannot "
        "be created");
    status = E_CLIENT_ERROR_INVALID_ARG;
    status_ok = FALSE;
    goto done;
  }

  notify_remotely_updated_contacts_and_complete (backend);

  if (!e_book_backend_tp_db_check_available_disk_space ())
  {
    status = E_BOOK_CLIENT_ERROR_NO_SPACE;
    status_ok = FALSE;
    goto done;
  }

  econtact_in = closure->econtacts;

  while (econtact_in)
  {
    EBookBackendTpContact *contact;

    contact = run_create_contact (backend, econtact_in->data, &error);

    if (contact)
    {
      EContact *econtact = e_book_backend_tp_contact_to_econtact (
            contact, priv->vcard_field, priv->protocol_name);

      econtacts = g_slist_prepend (econtacts, econtact);
      e_book_backend_tp_contact_unref (contact);
    }
    else
    {
      g_slist_free_full (econtacts, g_object_unref);
      econtacts = NULL;
      status = E_CLIENT_ERROR_INVALID_ARG;
      g_clear_error (&error);
      break;
    }

    econtact_in = g_slist_next (econtact_in);
  }

done:
  if (status_ok)
    e_data_book_respond_create_contacts (closure->book, closure->opid,
                                         NULL, econtacts);
  else
    e_data_book_respond_create_contacts (closure->book, closure->opid,
                                         e_client_error_create(status, NULL),
                                         econtacts);

  g_object_unref (closure->book);

  g_slist_free_full (closure->econtacts, g_object_unref);
  g_slist_free_full (econtacts, g_object_unref);
  g_object_unref (closure->backend);
  g_free (closure);

  return FALSE;
}

static void
e_book_backend_tp_create_contacts (EBookBackend *backend, EDataBook *book,
                                   guint32 opid, GCancellable *cancellable,
                                   const gchar * const *vcards, guint32 opflags)
{
  // XXX: opflags?
  CreateContactsClosure *closure;

  guint vcard_len = g_strv_length((gchar**) vcards);

  closure = g_new0 (CreateContactsClosure, 1);

  closure->backend = g_object_ref (backend);
  closure->book = g_object_ref (book);
  closure->opid = opid;

  for (int idx = 0; idx < vcard_len; idx++)
  {
    const char* vcard = vcards[idx];
    closure->econtacts = g_slist_prepend (
          closure->econtacts, e_contact_new_from_vcard (vcard));
  }

  g_idle_add (create_contacts_idle_cb, closure);
}

static EBookBackendTpContact*
run_remove_contact (EBookBackendTp         *backend,
                    EBookBackendTpClStatus  status,
                    const char             *uid,
                    gboolean               *ret_really_remove)
{
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookBackendTpContact *contact = NULL;
  GError *error = NULL;
  char **uid_list;
  int i;
  gboolean really_remove = TRUE;

  g_object_ref (backend);

  flush_db_updates (backend);

  /* The frontend appends master contact uids with a ; delimiter to notify us of
   * master contacts to remove from this contact before deletion */
  uid_list = g_strsplit (uid, ";", -1);
  contact = g_hash_table_lookup (priv->uid_to_contact, uid_list[0]);

  if (!contact)
  {
    WARNING ("Unknown UID asked to be deleted: %s", uid);
    goto cleanup;
  }

  /* Check special cases... */
  if (uid_list[1] && strcmp (uid_list[1], "*") == 0)
  {
    /* We want to remove all master uids, but not removing contact from roster */
    e_book_backend_tp_contact_remove_all_master_uids (contact);
    really_remove = FALSE;
  } else if (uid_list[1]) {
    /* We want to remove a list of master uids, and remove contact from roster
     * only if there are no more master uid. */
    for (i = 1; uid_list[i]; ++i) {
      if (g_strcmp0 (uid_list[i], "preserve") == 0)
        /* The address book wants to preserve the contact even if there are
         * not master contacts left.
         * This can happen if the master contact is deleted while the IM
         * account is disabled, so the master UID becomes invalid. At this
         * point the address book want to remove the invalid master UID
         * but without removing the IM contact */
        really_remove = FALSE;
      else if (g_strcmp0 (uid_list[i], "if-unused") == 0)
        /* Remove only if no master UIDs are left.
         * Just calling this method with only the roster UID in case there
         * are no master UIDs would be racy as another process could have
         * just added a master UID */
        continue;
      else
        e_book_backend_tp_contact_remove_master_uid (contact, uid_list[i]);
    }

    if (contact->master_uids->len > 0)
      really_remove = FALSE;
  }

  if (!really_remove)
  {
    /* We don't really want to remove this contact,
     * just an update of master uids */
    if (!(contact->pending_flags & SCHEDULE_UPDATE_MASTER_UID))
      goto cleanup;
    if (!e_book_backend_tp_db_update_contact (priv->tpdb, contact, &error))
    {
      WARNING ("Error whilst updating database contact: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    } else {
      contact->pending_flags &= ~SCHEDULE_UPDATE_MASTER_UID;
    }

    notify_updated_contact (backend, contact);
    goto cleanup;
  }

  contact->pending_flags = 0;

  if (contact->flags & CONTACT_INVALID)
  {
    /* Invalid contacts are not known to Telepathy, so there is no point in
     * calling e_book_backend_tp_cl_run_remove_contact on them and we just
     * remove them directly. */
    GArray *contacts_to_remove;

    contacts_to_remove = g_array_sized_new (TRUE, TRUE,
        sizeof (EBookBackendTpContact *), 1);
    g_array_append_val (contacts_to_remove, contact);

    delete_contacts (backend, contacts_to_remove);

    g_array_free (contacts_to_remove, TRUE);
  } else if (status == E_BOOK_BACKEND_TP_CL_ONLINE &&
      e_book_backend_tp_cl_run_remove_contact (priv->tpcl, contact, &error))
  {
    /* We've successfully asked for deletion. The actual removal from the
     * database, etc, will happen in the MembersChanged signal */
  } else {
    if (error)
    {
      WARNING ("Error whilst requesting contact deletion: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    }

    /* Mark for schedule removal */
    contact->pending_flags |= SCHEDULE_DELETE;
    g_hash_table_insert
      (priv->contacts_to_delete, g_strdup (contact->uid),
       e_book_backend_tp_contact_ref (contact));
  }

cleanup:
  g_strfreev (uid_list);
  g_object_unref (backend);

  if (ret_really_remove)
    *ret_really_remove = really_remove;

  return contact;
}

typedef struct
{
  EBookBackend *backend;
  EDataBook *book;
  guint32 opid;
  GList *id_list;
} RemoveMembersClosure;

static gboolean
remove_contacts_idle_cb (gpointer userdata)
{
  RemoveMembersClosure *closure = userdata;
  EBookBackendTp *backend = E_BOOK_BACKEND_TP (closure->backend);
  EBookBackendTpPrivate *priv = GET_PRIVATE (backend);
  EBookClientError status;
  gboolean status_ok = TRUE;
  EBookBackendTpContact *contact = NULL;
  EBookBackendTpClStatus tpcl_status;
  GArray *contacts_to_update = NULL;
  GSList *ids_removed = NULL;
  GError *error = NULL;
  GList *l = NULL;

  if (priv->load_error)
  {
    g_critical ("the book was not loaded correctly so the contacts cannot "
                "be removed");
    status = E_CLIENT_ERROR_NOT_OPENED;
    status_ok = FALSE;
    goto done;
  }

  if (!priv->tpdb)
  {
    status = E_BOOK_CLIENT_ERROR_NO_SUCH_BOOK;
    status_ok = FALSE;
    goto done;
  }

  if (!e_book_backend_tp_db_check_available_disk_space ())
  {
    status = E_BOOK_CLIENT_ERROR_NO_SPACE;
    status_ok = FALSE;
    goto done;
  }

  notify_remotely_updated_contacts_and_complete (backend);
  flush_db_updates (backend);

  tpcl_status = e_book_backend_tp_cl_get_status (priv->tpcl);

  /* Removing contacts is really easy. We basically just want to zero the
   * flags and then the members changed stuff deals with it fine. */

  for (l = closure->id_list; l; l = l->next)
  {
    gboolean really_remove = TRUE;

    contact = run_remove_contact (backend, tpcl_status, l->data, &really_remove);

    if (!contact)
      continue;

    if (really_remove)
    {
      ids_removed = g_slist_prepend (ids_removed, g_strdup (contact->uid));
      if (!contacts_to_update)
        contacts_to_update = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));
      g_array_append_val (contacts_to_update, contact);
    }
  }

done:
  if (status_ok)
    e_data_book_respond_remove_contacts (closure->book, closure->opid,
                                         NULL, ids_removed);
  else
    e_data_book_respond_remove_contacts (closure->book, closure->opid,
                                         e_client_error_create(status, NULL),
                                         ids_removed);

  /* Now update the database */
  if (contacts_to_update)
  {
    if (!e_book_backend_tp_db_update_contacts (priv->tpdb, contacts_to_update,
          &error))
    {
      g_critical ("error whilst updating database: %s",
          error ? error->message : "unknown error");
      g_clear_error (&error);
    }

    g_array_free (contacts_to_update, TRUE);
  }

  g_slist_free_full (ids_removed, g_free);
  g_object_unref (closure->backend);
  g_object_unref (closure->book);
  g_list_free_full (closure->id_list, g_free);
  g_free (closure);

  return FALSE;
}

static void
e_book_backend_tp_remove_contacts (EBookBackend *backend, EDataBook *book,
                                   guint32 opid, GCancellable *cancellable,
                                   const gchar * const *uids, guint32 opflags)
{
  // XXX: opflags?
  RemoveMembersClosure *closure = NULL;
  guint uids_len = g_strv_length((gchar**) uids);

  closure = g_new0 (RemoveMembersClosure, 1);
  closure->backend = g_object_ref (backend);
  closure->book = g_object_ref (book);
  closure->opid = opid;

  for (int idx = 0; idx < uids_len; idx++)
    closure->id_list = g_list_append (closure->id_list, g_strdup (uids[idx]));

  g_idle_add (remove_contacts_idle_cb, closure);
}

typedef struct
{
  EBookBackendTp *backend;
  EDataBook *book;
  guint32 opid;
  char *uid;
} GetContactClosure;

static gboolean
get_contact_idle_cb (gpointer userdata)
{
  GetContactClosure *closure = userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (closure->backend);
  EBookClientError status = E_CLIENT_ERROR_INVALID_ARG;
  gboolean status_ok = FALSE;
  EBookBackendTpContact *contact;
  EContact *ec = NULL;
  gchar *vcard = NULL;

  notify_remotely_updated_contacts_and_complete (closure->backend);

  if (closure->uid == NULL || closure->uid[0] == '\0')
  {
    WARNING ("Empty contact id");
    goto done;
  }

  contact = g_hash_table_lookup (priv->uid_to_contact, closure->uid);

  if (!contact)
  {
    status = E_BOOK_CLIENT_ERROR_CONTACT_NOT_FOUND;
    goto done;
  }

  ec = e_book_backend_tp_contact_to_econtact (contact, priv->vcard_field,
      priv->protocol_name);
  vcard = e_vcard_to_string (E_VCARD (ec), EVC_FORMAT_VCARD_30);

  status_ok = TRUE;

done:
  if (status_ok)
    e_data_book_respond_get_contact (closure->book, closure->opid,
                                     NULL, ec);
  else
    e_data_book_respond_get_contact (closure->book, closure->opid,
                                     e_client_error_create(status, NULL),
                                     ec);
  if (ec)
  {
    g_object_unref (ec);
  }

  g_free (vcard);
  g_object_unref (closure->backend);
  g_object_unref (closure->book);
  g_free (closure->uid);
  g_free (closure);

  return FALSE;
}

static void
e_book_backend_tp_get_contact (EBookBackend *backend, EDataBook *book,
                               guint32 opid, GCancellable *cancellable,
                               const gchar *id)
{
  GetContactClosure *closure = NULL;

  closure = g_new0 (GetContactClosure, 1);
  closure->backend = (EBookBackendTp *)g_object_ref (backend);
  closure->book = g_object_ref (book);
  closure->opid = opid;
  closure->uid = g_strdup (id);

  g_idle_add (get_contact_idle_cb, closure);
}

typedef struct
{
  EBookBackendTp *backend;
  EDataBook *book;
  guint32 opid;
  gchar *query;
} GetContactListClosure;

static gboolean
get_contact_list_idle_cb (gpointer userdata)
{
  GetContactListClosure *closure = userdata;
  EBookBackendTpPrivate *priv = GET_PRIVATE (closure->backend);
  EBookClientError status = E_CLIENT_ERROR_INVALID_ARG;
  gboolean status_ok = FALSE;
  EBookBackendSExp *sexp = NULL;
  gboolean get_all = FALSE;
  GHashTableIter iter;
  gpointer contact_pointer;
  GSList *contact_list = NULL;

  notify_remotely_updated_contacts_and_complete (closure->backend);

  if (closure->query == NULL || closure->query[0] == '\0') {
    WARNING ("Empty query");
    goto done;
  }

  sexp = e_book_backend_sexp_new (closure->query);
  if (sexp == NULL) {
    WARNING ("Could not create sexp");
    goto done;
  }

  DEBUG ("query: %s", closure->query);
  if (!g_ascii_strcasecmp (closure->query,
        "(contains \"x-evolution-any-field\" \"\")")) {
    get_all = TRUE;
  }

  g_hash_table_iter_init (&iter, priv->uid_to_contact);
  /* We cannot pass directly an EBookBackendTpContact * as it would break
   * strict aliasing */
  while (g_hash_table_iter_next (&iter, NULL, &contact_pointer)) {
    EBookBackendTpContact *contact = contact_pointer;
    EContact *ec;
    gchar *vcard;

    ec = e_book_backend_tp_contact_to_econtact (contact, priv->vcard_field,
        priv->protocol_name);
    vcard = e_vcard_to_string (E_VCARD (ec), EVC_FORMAT_VCARD_30);
    g_object_unref (ec);

    if (vcard == NULL || vcard[0] == '\0') {
      WARNING ("Could not generate vcard from record.");
      g_free (vcard);
      continue;
    }

    if (get_all || e_book_backend_sexp_match_vcard (sexp, vcard))
      contact_list = g_slist_prepend (contact_list, vcard);
  }

  status_ok = TRUE;

done:
  if (status_ok)
    e_data_book_respond_get_contact_list (closure->book, closure->opid,
                                          NULL, contact_list);
  else
    e_data_book_respond_get_contact_list (closure->book, closure->opid,
                                          e_client_error_create(status, NULL),
                                          contact_list);

  /* elements are released by libedata-book */
  g_slist_free (contact_list);

  if (sexp)
    g_object_unref (sexp);

  g_object_unref (closure->backend);
  g_object_unref (closure->book);
  g_free (closure->query);
  g_free (closure);

  return FALSE;
}

static void
e_book_backend_tp_get_contact_list (EBookBackend *backend, EDataBook *book,
                                    guint32 opid, GCancellable *cancellable,
                                    const gchar *query)
{
  GetContactListClosure *closure = NULL;

  closure = g_new0 (GetContactListClosure, 1);
  closure->backend = (EBookBackendTp *)g_object_ref (backend);
  closure->book = g_object_ref (book);
  closure->opid = opid;
  closure->query = g_strdup (query);

  g_idle_add (get_contact_list_idle_cb, closure);
}

#if 0
static void
e_book_backend_tp_set_view_sort_order (EBookBackend *backend,
                                       EDataBookView *book_view,
                                       const gchar *query_term)
{
  ContactSortOrder sort_order;

  if (g_strcmp0 (query_term, "first-last") == 0)
    sort_order = CONTACT_SORT_ORDER_FIRST_LAST;
  else if (g_strcmp0 (query_term, "last-first") == 0)
    sort_order = CONTACT_SORT_ORDER_LAST_FIRST;
  else if (g_strcmp0 (query_term, "nick") == 0)
    sort_order = CONTACT_SORT_ORDER_NICKNAME;
  else {
    WARNING ("unsupported sort order '%s'", query_term);
    return;
  }

  g_object_set_data (G_OBJECT (book_view), BOOK_VIEW_SORT_ORDER_DATA_KEY,
      GINT_TO_POINTER (sort_order));
}
#endif

static void
e_book_backend_tp_class_init (EBookBackendTpClass *klass)
{
  GObjectClass *object_class = NULL;
  EBookBackendClass *backend_class = NULL;
  EBookBackendSyncClass *sync_class = NULL;

  object_class = G_OBJECT_CLASS (klass);
  backend_class = E_BOOK_BACKEND_CLASS (klass);
  sync_class = E_BOOK_BACKEND_SYNC_CLASS(klass);

  object_class->dispose = e_book_backend_tp_dispose;
  object_class->finalize = e_book_backend_tp_finalize;

  sync_class->open_sync = e_book_backend_tp_open_sync;
  backend_class->impl_get_backend_property = e_book_backend_tp_get_backend_property;
  backend_class->impl_start_view = e_book_backend_tp_start_view;
  backend_class->impl_stop_view = e_book_backend_tp_stop_view;

  backend_class->impl_modify_contacts = e_book_backend_tp_modify_contacts;
  backend_class->impl_create_contacts = e_book_backend_tp_create_contacts;
  backend_class->impl_remove_contacts = e_book_backend_tp_remove_contacts;
  backend_class->impl_get_contact = e_book_backend_tp_get_contact;
  backend_class->impl_get_contact_list = e_book_backend_tp_get_contact_list;

  /* There should be exactly one async OR sync implementation of each function,
   * so we don't create stubs for the sync functions here. */

  signals[READY_SIGNAL] = g_signal_new ("ready",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (EBookBackendTpClass, ready),
      NULL, NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[MEMBERS_READY_SIGNAL] = g_signal_new ("members-ready",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (EBookBackendTpClass, members_ready),
      NULL, NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);
}

EBookBackend *
e_book_backend_tp_new (void)
{
  return (EBookBackend *)g_object_new (E_TYPE_BOOK_BACKEND_TP, NULL);
}
