/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *   @author Rob Bradford
 *   @author Travis Reitter <travis.reitter@maemo.org>
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
#include "e-book-backend-tp-cl.h"
#include "e-book-backend-tp-contact.h"
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/defs.h>
#include <telepathy-glib/interfaces.h>
#include <libmcclient/dbus-api.h>
#include <libmcclient/mc-account.h>
#include "test-utils.h"

static void
aliases_changed_cb (EBookBackendTpCl *tpcl, GArray *contacts_in, 
    gpointer userdata)
{
  TestUserData *test_userdata = (TestUserData *)userdata;
  EBookBackendTpContact *contact;
  EBookBackendTpContact *contact_in;
  gint i = 0;

  /* XXX: this could have a race condition if someone/thing else is modifying
   * the list at the same time; maybe we should use a custom account and make
   * part of the testing be conditional upon the tester only running this at
   * once */
  /* TODO: check edge cases (like the list and array being different lengths) */
  for (i = 0; contacts_in && (i < contacts_in->len); i++)
  {
    contact_in = g_array_index (contacts_in, EBookBackendTpContact *, i);

    /* if we know about this contact, check that our update went through */
    contact = g_hash_table_lookup (test_userdata->name_to_contact_hash,
        contact_in->name);
    if (contact)
    {
      if (!g_str_equal (contact->alias, contact_in->alias))
      {
        g_warning (G_STRLOC ": read back alias: '%s' (expected '%s')",
            contact_in->alias, contact->alias);

        exit (5);
      } else {
        g_debug (G_STRLOC ": read back the expected alias ('%s')",
            contact_in->alias);
      }
    } else {
      g_debug (G_STRLOC ": got unexpected contact (handle: %d, name: %s)\n\n",
          contact_in->handle, contact_in->name);
    }
  }

  /* if we got here, the test succeeded */
  g_main_loop_quit (test_userdata->loop);
}

static void
get_members_result_cb (EBookBackendTpCl *tpcl, GArray *contacts_in,
    const GError *error_in, gpointer userdata)
{
  TestUserData *test_userdata = (TestUserData *)userdata;
  EBookBackendTpContact *contact_in = NULL;
  EBookBackendTpContact *contact = NULL;
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
   * Empty the handle table. We've just gone online and so any old handles may
   * be invalid.
   */
  g_hash_table_remove_all (test_userdata->name_to_contact_hash);

  for (i = 0; i < contacts_in->len; i++)
  {
    contact_in = g_array_index (contacts_in, EBookBackendTpContact *, i);

    /* make our own parallel version of the contact;
     * XXX: we only copy what we care about as of this writing, so extend this
     * carefully */
    contact = e_book_backend_tp_contact_new ();
    contact->handle = contact_in->handle;
    contact->name = g_strdup (contact_in->name);
    contact->alias = g_strdup (ALIASES[i]);

    contact->pending_flags |= SCHEDULE_UPDATE_ALIAS;

    g_hash_table_insert (test_userdata->name_to_contact_hash, contact->name,
        contact);

    g_debug (G_STRLOC ": original contact (handle: %d, name: %s, alias: %s)",
        contact_in->handle, contact_in->name, contact_in->alias);
    g_debug (G_STRLOC ": inserted contact (handle: %d, name: %s, alias: %s)",
        contact->handle, contact->name, contact->alias);

    if (!e_book_backend_tp_cl_update_alias (test_userdata->tpcl, contact,
        &error))
    {
      g_warning (G_STRLOC ": Error whilst updating contact alias: %s",
          error->message);
      g_clear_error (&error);

      exit (5);
    }

    /* Clear the flag. We don't want this to happen again */
    contact->pending_flags &= ~SCHEDULE_UPDATE_ALIAS;
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
      "change-aliases MISSION_CONTROL_ACCOUNT_NAME\n"
      "eg,\n"
      "     change-aliases dummy0\n");
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

  /* FIXME: the last destroy function needs to free deeply */
  test_userdata->name_to_contact_hash = g_hash_table_new_full (g_str_hash,
      g_str_equal, g_free, g_free);

  /* store the account name for debugging use later */
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

  g_signal_connect (test_userdata->tpcl, "aliases-changed",
      G_CALLBACK (aliases_changed_cb), test_userdata);

  if (!e_book_backend_tp_cl_load (test_userdata->tpcl, test_userdata->account,
                                  &error))
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
