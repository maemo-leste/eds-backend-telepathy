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

#ifndef _E_BOOK_BACKEND_TP_CL
#define _E_BOOK_BACKEND_TP_CL

#include <glib-object.h>
#include <libmcclient/mc-account.h>
#include "e-book-backend-tp-types.h"

G_BEGIN_DECLS

#define E_TYPE_BOOK_BACKEND_TP_CL e_book_backend_tp_cl_get_type()

#define E_BOOK_BACKEND_TP_CL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), E_TYPE_BOOK_BACKEND_TP_CL, EBookBackendTpCl))

#define E_BOOK_BACKEND_TP_CL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), E_TYPE_BOOK_BACKEND_TP_CL, EBookBackendTpClClass))

#define E_IS_BOOK_BACKEND_TP_CL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), E_TYPE_BOOK_BACKEND_TP_CL))

#define E_IS_BOOK_BACKEND_TP_CL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), E_TYPE_BOOK_BACKEND_TP_CL))

#define E_BOOK_BACKEND_TP_CL_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_BACKEND_TP_CL, EBookBackendTpClClass))

#define E_BOOK_BACKEND_TP_CL_ERROR e_book_backend_tp_cl_error()

typedef struct {
  GObject parent;
} EBookBackendTpCl;

typedef enum {
  E_BOOK_BACKEND_TP_CL_OFFLINE = 0,
  E_BOOK_BACKEND_TP_CL_ONLINE
} EBookBackendTpClStatus;

typedef struct {
  GObjectClass parent_class;
  void (*status_changed) (EBookBackendTpCl *tpcl, EBookBackendTpClStatus status);

  void (*contacts_added) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*contacts_removed) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*flags_changed) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*aliases_changed) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*presences_changed) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*avatar_tokens_changed) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*avatar_data_changed) (EBookBackendTpCl *tpcl, EBookBackendTpContact *contact);
  void (*capabilities_changed) (EBookBackendTpCl *tpcl, GArray *contacts);
  void (*contact_info_changed) (EBookBackendTpCl *tpcl, GArray *contacts);
} EBookBackendTpClClass;

typedef enum
{
  E_BOOK_BACKEND_TP_CL_ERROR_FAILED,
  E_BOOK_BACKEND_TP_CL_ERROR_INVALID_ACCOUNT
} EBookBackendTpClError;

typedef void (*EBookBackendTpClGetMembersCallback) (EBookBackendTpCl *tpcl,
    GArray *contacts, const GError *error, gpointer userdata);

/*
typedef void (*EBookBackendTpClRemoveMembersCallback) (EBookBackendTpCl *tpcl,
    EBookBackendTpContactFlag flag, const GError *error, gpointer userdata);
*/

GType e_book_backend_tp_cl_get_type (void);
GQuark e_book_backend_tp_cl_error (void);

EBookBackendTpCl *e_book_backend_tp_cl_new (void);
gboolean e_book_backend_tp_cl_load (EBookBackendTpCl *tpcl, 
    McAccount *account, GError **error);
EBookBackendTpClStatus e_book_backend_tp_cl_get_status (EBookBackendTpCl *tpcl);

gboolean e_book_backend_tp_cl_get_members (EBookBackendTpCl *tpcl, 
    EBookBackendTpClGetMembersCallback cb, gpointer userdata, GError **error);

gboolean e_book_backend_tp_cl_run_update_flags (EBookBackendTpCl *tpcl,
    EBookBackendTpContact *updated_contact, GError **error);

gboolean e_book_backend_tp_cl_run_add_contact (EBookBackendTpCl *tpcl,
    EBookBackendTpContact *new_contact, GError **error);

gboolean e_book_backend_tp_cl_run_remove_contact (EBookBackendTpCl *tpcl, 
    EBookBackendTpContact *contact, GError **error_out);

gboolean e_book_backend_tp_cl_run_unblock_contact (EBookBackendTpCl *tpcl, 
    EBookBackendTpContact *contact, GError **error_out);

gboolean e_book_backend_tp_cl_request_avatar_data (EBookBackendTpCl *tpcl,
    GArray *contacts, GError **error_out);

G_END_DECLS

#endif /* _E_BOOK_BACKEND_TP_CL */

