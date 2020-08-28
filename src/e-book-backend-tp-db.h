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

#ifndef _E_BOOK_BACKEND_TP_DB
#define _E_BOOK_BACKEND_TP_DB

#include <glib-object.h>

#include "e-book-backend-tp-contact.h"

G_BEGIN_DECLS

#define E_TYPE_BOOK_BACKEND_TP_DB e_book_backend_tp_db_get_type()

#define E_BOOK_BACKEND_TP_DB(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), E_TYPE_BOOK_BACKEND_TP_DB, EBookBackendTpDb))

#define E_BOOK_BACKEND_TP_DB_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), E_TYPE_BOOK_BACKEND_TP_DB, EBookBackendTpDbClass))

#define E_IS_BOOK_BACKEND_TP_DB(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), E_TYPE_BOOK_BACKEND_TP_DB))

#define E_IS_BOOK_BACKEND_TP_DB_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), E_TYPE_BOOK_BACKEND_TP_DB))

#define E_BOOK_BACKEND_TP_DB_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_BACKEND_TP_DB, EBookBackendTpDbClass))

#define E_BOOK_BACKEND_TP_DB_ERROR e_book_backend_tp_db_error()

typedef struct {
  GObject parent;
} EBookBackendTpDb;

typedef struct {
  GObjectClass parent_class;
} EBookBackendTpDbClass;

typedef enum
{
  E_BOOK_BACKEND_TP_DB_ERROR_FAILED
} EBookBackendTpDbError;

GType e_book_backend_tp_db_get_type (void);
GQuark e_book_backend_tp_db_error (void);

EBookBackendTpDb *e_book_backend_tp_db_new (void);

gboolean e_book_backend_tp_db_open (EBookBackendTpDb *tpdb, const gchar *account_name, 
    GError **error);
gboolean e_book_backend_tp_db_close (EBookBackendTpDb *tpdb, GError **error);

GArray *e_book_backend_tp_db_fetch_contacts (EBookBackendTpDb *tpdb, 
    GError **error);

gboolean e_book_backend_tp_db_add_contact (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error);
gboolean e_book_backend_tp_db_update_contact (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error);
gboolean e_book_backend_tp_db_delete_contact (EBookBackendTpDb *tpdb,
    const gchar *uid, GError **error);

gboolean e_book_backend_tp_db_add_contacts (EBookBackendTpDb *tpdb,
    GArray *contacts, GError **error);
gboolean e_book_backend_tp_db_update_contacts (EBookBackendTpDb *tpdb,
    GArray *contacts, GError **error);
gboolean e_book_backend_tp_db_remove_contacts (EBookBackendTpDb *tpdb,
    GArray *uids, GError **error);

gboolean e_book_backend_tp_db_delete (EBookBackendTpDb *tpdb, GError **error);

gboolean e_book_backend_tp_db_check_available_disk_space (void);

void e_book_backend_tp_db_cleanup_unused_dbs (void);

G_END_DECLS

#endif /* _E_BOOK_BACKEND_TP_DB */
