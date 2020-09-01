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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "e-book-backend-tp-cl.h"
#include "e-book-backend-tp-contact.h"

static void
contact_list_get_members_cb (EBookBackendTpCl *tpcl, 
   GArray *contacts, const GError *error, gpointer userdata)
{
  EBookBackendTpContact *contact;

  guint i = 0;

  for (i = 0; contacts && (i < contacts->len); i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    g_debug ("got handle %d with name %s and alias %s (%s: %s)",
        contact->handle, contact->name, contact->alias, contact->status,
        contact->status_message);
  }
}

static void
contacts_added_cb (EBookBackendTpCl *tpcl, GArray *contacts, 
    gpointer userdata)
{
  EBookBackendTpContact *contact;
  guint i = 0;

  for (i = 0; contacts && (i < contacts->len); i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact*, i);
    g_debug ("got new contact: handle %d, name: %s, alias: %s",
        contact->handle, contact->name, contact->alias);
  }
}

static void
aliases_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts, 
    gpointer userdata)
{
  EBookBackendTpContact *contact;
  guint i = 0;

  for (i = 0; contacts && (i < contacts->len); i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact*, i);
    g_debug ("handle %d has new alias: %s", contact->handle, contact->alias);
  }
}

static void
avatar_changed_cb (EBookBackendTpCl *tpcl, EBookBackendTpContact *contact, 
    gpointer userdata)
{
  int image_fd;
  gchar *filename = NULL;
  gint write_count = -1;

  g_debug ("handle %d has new avatar: (token: '%s', len: %d, MIME type: %s)",
      contact->handle, contact->avatar_token, contact->avatar_len,
      contact->avatar_mime);

  /* TODO: interpret MIME type and append it to the filename appropriately */
  filename = g_strdup_printf ("/tmp/img-handle-%d", contact->handle);
  image_fd = open (filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
  g_assert (image_fd >= 0);
  write_count = write (image_fd, contact->avatar_data, contact->avatar_len);
  close (image_fd);

  g_debug ("wrote %d bytes to %s", write_count, filename);

  g_free (filename);
}

static void
presences_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts, 
    gpointer userdata)
{
  EBookBackendTpContact *contact;
  guint i = 0;

  for (i = 0; contacts && (i < contacts->len); i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact*, i);
    g_debug ("handle %d has new presence: (%s: %s)",
        contact->handle, contact->status, contact->status_message);
  }
}

static void
contact_info_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts, 
    gpointer userdata)
{
  EBookBackendTpContact *contact;
  guint i = 0;

  for (i = 0; contacts && (i < contacts->len); i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact*, i);
    g_debug ("handle %d has new contactinfo: \n%s",
        contact->handle, contact->contact_info);
  }
}

static void
status_changed_cb (EBookBackendTpCl *tpcl, EBookBackendTpClStatus status)
{
  if (status == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    e_book_backend_tp_cl_get_members (tpcl, contact_list_get_members_cb, NULL,
        NULL);
  }
}

gint
main (gint argc, gchar **argv)
{
  GMainLoop *loop;
  EBookBackendTpCl *tpcl;
  const gchar *account_name;
  gchar *object_path;
  TpDBusDaemon *dbus_daemon;
  TpAccount *account;

  if (argc > 1)
    account_name = argv[1];
  else
    account_name = "gabble/jabber/account1";

  tpcl = e_book_backend_tp_cl_new ();

  object_path = g_strconcat (TP_ACCOUNT_OBJECT_PATH_BASE, account_name, NULL);
  dbus_daemon = tp_dbus_daemon_dup (NULL);
  account = tp_account_new (dbus_daemon, object_path, NULL);
  g_object_unref (dbus_daemon);
  g_free (object_path);
 
  if (account)
  {
    e_book_backend_tp_cl_load (tpcl, account, NULL);
    g_signal_connect (tpcl, "contacts-added",
    G_CALLBACK (contacts_added_cb), NULL);
    g_signal_connect (tpcl, "status-changed", G_CALLBACK (status_changed_cb),
    NULL);
    g_signal_connect (tpcl, "aliases-changed",
    G_CALLBACK (aliases_changed_cb), NULL);
    g_signal_connect (tpcl, "avatar-data-changed",
    G_CALLBACK (avatar_changed_cb), NULL);
    g_signal_connect (tpcl, "presences-changed",
    G_CALLBACK (presences_changed_cb), NULL);
    
    g_signal_connect (tpcl, "contact-info-changed",
    G_CALLBACK (contact_info_changed_cb), NULL);
    
    loop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (loop);
  } else {
    g_warning ("failed to look up Mission Control account '%s'", account_name);
  }

  return 0;
}
