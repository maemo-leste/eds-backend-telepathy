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

#ifndef _E_BOOK_BACKEND_TP_CONTACT
#define _E_BOOK_BACKEND_TP_CONTACT

#include <telepathy-glib/handle.h>
#include <libedata-book/e-book-backend-sync.h>
#include "e-book-backend-tp-types.h"

struct _EBookBackendTpContact {
  TpHandle handle;
  gchar *name;
  gchar *alias;
  const gchar *generic_status;
  gchar *status;
  gchar *status_message;
  gchar *avatar_token;
  gchar *avatar_mime;
  guint avatar_len;
  gchar *avatar_data;
  gchar *contact_info; /* a vcard string obtained from ContatInfo interface */
  
  /* bitwise-OR'd EBookBackendTpContactFlag values describing which contact
   * list(s) the contact belongs to */
  guint32 flags;
  guint32 pending_flags;
  gchar *uid;
  GPtrArray *master_uids;
  guint32 capabilities; /* Bitwise OR of EBookBackendTpContactCapabilities */
  /* Non-normalized forms of the contact username known to be acceptable */
  GHashTable *variants; /* gchar * -> TRUE (i.e. value ignored) */

  gint ref_count;
};

EBookBackendTpContact *
e_book_backend_tp_contact_ref                  (EBookBackendTpContact *contact);

void
e_book_backend_tp_contact_unref                (EBookBackendTpContact *contact);

EBookBackendTpContact *
e_book_backend_tp_contact_new                  (void);

EBookBackendTpContact *
e_book_backend_tp_contact_dup                  (EBookBackendTpContact *contact);

gboolean
e_book_backend_tp_contact_is_visible           (EBookBackendTpContact *contact);

EContact *
e_book_backend_tp_contact_to_econtact          (EBookBackendTpContact *contact,
                                                const gchar           *vcard_field,
                                                const gchar           *profile_name);
gboolean
e_book_backend_tp_contact_update_from_econtact (EBookBackendTpContact *contact,
                                                EContact              *ec,
                                                const gchar           *vcard_field);

gboolean
e_book_backend_tp_contact_update_name          (EBookBackendTpContact *contact,
                                                const gchar           *new_name);

gboolean
e_book_backend_tp_contact_update_master_uids   (EBookBackendTpContact *contact,
                                                GPtrArray             *master_uids);

gboolean
e_book_backend_tp_contact_remove_master_uid    (EBookBackendTpContact *contact,
                                                const char            *uid);

void
e_book_backend_tp_contact_remove_all_master_uids (EBookBackendTpContact *contact);

void
e_book_backend_tp_contact_add_variants_from_contact (EBookBackendTpContact *dest,
                                                     EBookBackendTpContact *src);

#endif /* _E_BOOK_BACKEND_TP_CONTACT */
