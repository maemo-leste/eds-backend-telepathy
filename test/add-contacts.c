/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
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
#include <stdlib.h>
#include <string.h>
#include "e-book-backend-tp-cl.h"
#include "e-book-backend-tp-contact.h"
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/defs.h>
#include <telepathy-glib/interfaces.h>
#include "test-utils.h"

/* This callback may be triggered any number of times; we don't assume all
 * contacts will be added within a single contacts-added signal emittance.
 * However, we don't bother checking for duplicate "contacts-added" entries, so
 * that will need to be its own test if it's a concern */
static void
contacts_added_cb (EBookBackendTpCl *tpcl, GArray *contacts_added, 
    gpointer userdata)
{
  TestUserData *test_userdata = (TestUserData *)userdata;
  EBookBackendTpContact *contact_added = NULL;
  EBookBackendTpContact *contact_stored = NULL;
  guint i = 0;

  /* FIXME: turn this into an error once the bug is resolved in the main source
   */
  if (!contacts_added)
  {
    g_warning (G_STRLOC ": 'contacts-added' emitted with a NULL contacts_added "
        "array");
    return;
  }

  if (contacts_added->len == 0)
  {
    g_warning (G_STRLOC ": 'contacts-added' emitted with a 0-member "
        "contacts_added array");
  }

  g_debug (G_STRLOC ": got 'contacts-added' with %d contacts",
      contacts_added->len);

  for (i = 0; i < contacts_added->len; i++)
  {
    contact_added = g_array_index (contacts_added, EBookBackendTpContact *, i);

    g_debug (G_STRLOC ": examining contact_added: "
        "(handle: %d, name: %s, alias: %s)", contact_added->handle,
        contact_added->name, contact_added->alias);

    contact_stored = g_hash_table_lookup (test_userdata->name_to_contact_hash,
        contact_added->name);

    if (contact_stored)
    {
      if (contacts_equal (contact_stored, contact_added))
        {
          g_debug (G_STRLOC ": got 'contacts-added' for contact we added: "
              "(handle: %d, name: %s, alias: %s)", contact_stored->handle,
              contact_stored->name, contact_stored->alias);

          g_hash_table_remove (test_userdata->name_to_contact_hash,
              contact_stored->name);

          /* if we have gotten "contacts-added" signals for all the contacts we
           * attempted to add (before the full-test timeout is reached), we've
           * succeeded */
          if (g_hash_table_size (test_userdata->name_to_contact_hash) == 0)
          {
            g_main_loop_quit (test_userdata->loop);
          }
        } else {
          g_warning (G_STRLOC ": found contact with same username as an "
              "expected contact, but other fields are different.\n" 
              "got:      (handle: %d, name: %s, alias: %s)\n"
              "expected: (handle: %d, name: %s, alias: %s)",
              contact_added->handle, contact_added->name, contact_added->alias,
              contact_stored->handle, contact_stored->name,
              contact_stored->alias);
          exit (7);
        }
    } else {
      /* XXX: we're assuming nobody else is modifying our contact list while
       * this test runs */
      g_warning (G_STRLOC ": got unexpected contact in 'contacts-added'" 
          "(handle: %d, name: %s, alias: %s)", contact_added->handle,
          contact_added->name, contact_added->alias);

      /* FIXME: uncomment once the bug is resolved in the main source */
      /*
      exit (8);
      */
    }
  }

  /* if we have gotten "contacts-added" signals for all the contacts we
   * attempted to add (before the full-test timeout is reached), we've succeeded
   */
  if (g_hash_table_size (test_userdata->name_to_contact_hash) == 0)
  {
    g_debug (G_STRLOC ": exiting via the fallback g_main_loop_quit");
    g_main_loop_quit (test_userdata->loop);
  }
}

static void
get_members_result_cb (EBookBackendTpCl *tpcl, GArray *contacts_in,
    const GError *error_in, gpointer userdata)
{
  TestUserData *test_userdata = (TestUserData *)userdata;
  EBookBackendTpContact *contact_new = NULL;
  gint i = 0;
  GError *error = NULL;

  if (error_in)
  {
    g_warning (G_STRLOC ": Error getting contacts: %s", error_in->message);
    exit (3);
  }

  /* our roster is empty, or something buggy happened? */
  if (contacts_in == NULL)
  {
    g_debug (G_STRLOC ": contacts list is empty; it shouldn't be");
    exit (4);
  }

  g_debug (G_STRLOC ": get_members called with %d contacts", contacts_in->len);

  /* 
   * Empty the handle tables. We've just gone online and so any old handles may
   * be invalid.
   */
  g_hash_table_remove_all (test_userdata->name_to_contact_hash);

  for (i = 0; ALIASES[i]; i++)
  {
    /* build a hash of contacts we're adding, for later comparison */
    /* XXX: we only copy what we care about as of this writing, so extend this
     * carefully */
    contact_new = e_book_backend_tp_contact_new ();

    contact_new->name = alias_to_username (ALIASES[i]);
    contact_new->alias = g_strdup (ALIASES[i]);

    contact_new->pending_flags |= SCHEDULE_ADD;
    contact_new->pending_flags |= SUBSCRIBE | PUBLISH;

    g_debug (G_STRLOC ": inserted contact (handle: %d, name: %s, alias: %s)",
        contact_new->handle, contact_new->name, contact_new->alias);

    g_hash_table_insert (test_userdata->name_to_contact_hash, contact_new->name,
        contact_new);

    if (!e_book_backend_tp_cl_add_contact (test_userdata->tpcl, contact_new,
        &error))
    {
      g_warning (G_STRLOC ": Error whilst adding contact (name: %s, "
          "'alias: %s'): %s", contact_new->name, contact_new->alias,
          error->message);
      g_clear_error (&error);

      exit (6);
    }

    /* Clear the flag. We don't want this to happen again */
    contact_new->pending_flags &= ~SCHEDULE_ADD;
  }
}

static void
get_members (TestUserData *test_userdata)
{
  GError *error = NULL;

  g_debug (G_STRLOC ": online and getting members");
  if (!e_book_backend_tp_cl_get_members (test_userdata->tpcl,
      get_members_result_cb, (gpointer)test_userdata, &error))
  {
    g_warning (G_STRLOC ": Error when asking for members: %s",
        error->message);
    g_clear_error (&error);

    exit (2);
  }
}

static void
status_changed_get_members_cb (EBookBackendTpCl *tpcl,
    EBookBackendTpClStatus status, gpointer userdata)
{
  TestUserData *test_userdata = (TestUserData *)userdata;

  g_debug (G_STRLOC ": Status changed %d", status);

  /* Ahaha. We are online now, so we can read in the existing members */
  if (status == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    get_members (test_userdata);
  }
}

static gboolean
get_members_idle_cb (gpointer userdata)
{
  TestUserData *test_userdata = (TestUserData *)userdata;
  EBookBackendTpClStatus status;

  g_signal_connect (test_userdata->tpcl, "status-changed",
      (GCallback)status_changed_get_members_cb, userdata);

  /* get the members now if we already are online */
  status = e_book_backend_tp_cl_get_status (test_userdata->tpcl);

  if (status == E_BOOK_BACKEND_TP_CL_ONLINE)
  {
    get_members (test_userdata);
  } else {
    g_debug (G_STRLOC ": not online");
  }

  return FALSE;
}

static gboolean
timeout_cb (EBookBackendTpCl *tpcl)
{
  g_debug (G_STRLOC ": timed out waiting on test-critical event(s)");

  exit(1);
}

static void
print_usage_and_exit (guint status)
{
  g_printerr ("usage:\n"
      "add-contacts MISSION_CONTROL_ACCOUNT_NAME\n"
      "eg,\n"
      "     add-contacts dummy0\n");
  exit (status);
}

static void
validate_args (gint argc, gchar **argv, gchar **account)
{
  if (argc != 2)
  {
    print_usage_and_exit (10);
  }

  *account = g_strdup (argv[1]);
}

gint
main (gint argc, gchar **argv)
{
  TestUserData *test_userdata;
  gchar *account = NULL;
  gchar *object_path;
  DBusGConnection *bus;
  TpDBusDaemon *dbus_daemon;
  GError *error = NULL;

  validate_args (argc, argv, &account);

  g_type_init ();

  test_userdata = g_new0 (TestUserData, 1);
  test_userdata->loop = g_main_loop_new (NULL, FALSE);

  /* FIXME: for some reason, if we set up the proper contact destroy function
   * (e_book_backend_tp_contact_unref()), we trigger a double-free somewhere
   * else */
  test_userdata->name_to_contact_hash = g_hash_table_new_full (g_str_hash,
      g_str_equal, g_free, NULL);

  test_userdata->account_name = g_strdup (account);

  object_path = g_strconcat (MC_ACCOUNT_DBUS_OBJECT_BASE, account, NULL);
  bus = tp_get_bus ();
  dbus_daemon = tp_dbus_daemon_new (bus);
  test_userdata->account = mc_account_new (dbus_daemon, object_path);
  g_object_unref (dbus_daemon);
  g_free (object_path);

  if (!test_userdata->account)
  {
    g_warning (G_STRLOC ": error looking up account name: %s",
        test_userdata->account_name);
    exit (9);
  }

  test_userdata->tpcl = e_book_backend_tp_cl_new ();

  g_signal_connect (test_userdata->tpcl, "contacts-added",
      G_CALLBACK (contacts_added_cb), test_userdata);

  if (!e_book_backend_tp_cl_load (test_userdata->tpcl, (McAccount*)(NULL), &error))
  {
    g_warning (G_STRLOC ": Error loading contact list: %s",
        error->message);
    g_clear_error (&error);

    exit (11);
  }

  g_idle_add ((GSourceFunc)get_members_idle_cb, test_userdata);
  /* FIXME: we should probably push this closer to the final "wind up" */
  g_timeout_add (20000, (GSourceFunc)timeout_cb, test_userdata->tpcl);

  g_main_loop_run (test_userdata->loop);

  return 0;
}
