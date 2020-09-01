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

//#include <libedataserver/e-data-server-module.h>
//#include <libedata-book/e-book-backend-factory.h>

#include "e-book-backend-tp.h"
#include "e-book-backend-tp-db.h"
#include "e-book-backend-tp-log.h"


DBusGConnection *e_book_backend_tp_system_bus_connection = NULL;

static GType tp_factory_type;

#define FACTORY_NAME "tp"

typedef EBookBackendFactory EBookBackendTpFactory;
typedef EBookBackendFactoryClass EBookBackendTpFactoryClass;

static EModule *e_module;

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

/* Forward Declarations */
GType e_book_backend_tp_factory_get_type (void);

G_DEFINE_DYNAMIC_TYPE (
	EBookBackendTpFactory,
	e_book_backend_tp_factory,
	E_TYPE_BOOK_BACKEND_FACTORY)

static void
e_book_backend_tp_factory_class_init (EBookBackendFactoryClass *klass)
{
	EBackendFactoryClass *backend_factory_class;

	backend_factory_class = E_BACKEND_FACTORY_CLASS (klass);
	backend_factory_class->e_module = e_module;
	backend_factory_class->share_subprocess = TRUE;

	klass->factory_name = FACTORY_NAME;
	klass->backend_type = E_TYPE_BOOK_BACKEND_TP;
}

static void
e_book_backend_tp_factory_class_finalize (
    EBookBackendFactoryClass *klass)
{
}

static void
e_book_backend_tp_factory_init (EBookBackendFactory *factory)
{
}


static gboolean
clean_unused_dbs_timout_cb (gpointer userdata)
{
  e_book_backend_tp_db_cleanup_unused_dbs ();
  return FALSE;
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
  GError *error = NULL;

  e_module = E_MODULE (type_module);

  e_book_backend_tp_factory_register_type (type_module);
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

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
	e_module = NULL;
}
