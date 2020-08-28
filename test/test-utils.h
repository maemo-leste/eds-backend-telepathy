/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
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

#ifndef _TEST_UTILS_H
#define _TEST_UTILS_H

#include "e-book-backend-tp-cl.h"

#define USERNAME_VALID_CHARS \
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

extern const gchar const *ALIASES[];

typedef struct
{
  GMainLoop *loop;
  gchar *account_name;
  McAccount *account;
  EBookBackendTpCl *tpcl;
  GHashTable *name_to_contact_hash;
} TestUserData;

gchar* alias_to_username (const gchar *alias);
gboolean contacts_equal (EBookBackendTpContact *a, EBookBackendTpContact *b);

#endif /* _TEST_UTILS_H */
