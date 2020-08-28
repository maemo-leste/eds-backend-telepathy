/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *   @author Rob Bradford
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

#include <libedataserver/e-data-server-module.h>
#include <libedata-book/e-book-backend-factory.h>

#include "e-book-backend-tp.h"
#include "e-book-backend-tp-db.h"
#include "e-book-backend-tp-log.h"

E_BOOK_BACKEND_FACTORY_SIMPLE (tp, Tp, e_book_backend_tp_new);

DBusGConnection *e_book_backend_tp_system_bus_connection = NULL;

static GType tp_factory_type;

static gboolean
clean_unused_dbs_timout_cb (gpointer userdata)
{
  e_book_backend_tp_db_cleanup_unused_dbs ();
  return FALSE;
}

void
eds_module_initialize (GTypeModule *module)
{
  GError *error = NULL;

  tp_factory_type = _tp_factory_get_type (module);

  g_timeout_add_seconds_full (G_PRIORITY_LOW, 1, clean_unused_dbs_timout_cb,
      NULL, NULL); 

  e_book_backend_tp_log_domain_id = e_log_get_id ("tp");

  e_book_backend_tp_system_bus_connection = dbus_g_bus_get (
      DBUS_BUS_SYSTEM, &error);
  if (!e_book_backend_tp_system_bus_connection)
  {
    g_critical ("Failed to get system bus connection: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
  }
}

void
eds_module_shutdown (void)
{

}

void
eds_module_list_types (const GType **types, int *ntypes)
{
  *types = &tp_factory_type;
  *ntypes = 1;
}

