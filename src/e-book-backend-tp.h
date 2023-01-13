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

#ifndef _E_BOOK_BACKEND_TP_H__
#define _E_BOOK_BACKEND_TP_H__

#include <dbus/dbus-glib.h>
#include <libedata-book/libedata-book.h>

extern DBusGConnection *e_book_backend_tp_system_bus_connection;

#define E_TYPE_BOOK_BACKEND_TP (e_book_backend_tp_get_type ())
#define E_BOOK_BACKEND_TP(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_BOOK_BACKEND_TP, EBookBackendTp))
#define E_BOOK_BACKEND_TP_CLASS(k) (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_BOOK_BACKEND_TP, EBookBackendTpClass))
#define E_IS_BOOK_BACKEND_TP(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_BOOK_BACKEND_TP))
#define E_IS_BOOK_BACKEND_TP_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_BOOK_BACKEND_TP))
#define E_BOOK_BACKEND_TP_GET_CLASS(k) (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_BACKEND_TP, EBookBackendTpClass))

typedef struct _EBookBackendTpPrivate EBookBackendTpPrivate;

typedef struct {
  EBookBackendSync parent;
  EBookBackendTpPrivate *priv;
} EBookBackendTp;

typedef struct {
  EBookBackendSyncClass parent_class;
  void (*ready) (EBookBackendTp *backend);
  void (*members_ready) (EBookBackendTp *backend);
} EBookBackendTpClass;

EBookBackend *e_book_backend_tp_new (void);

GType e_book_backend_tp_get_type (void);


#endif /* _E_BOOK_BACKEND_TP_H_ */

