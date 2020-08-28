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

#include "e-book-backend-tp-contact.h"
#include "test-utils.h"

/* TODO: this list could be much better (especially longer, include non-ASCII
 * characters, perhaps generated) */
const gchar const *ALIASES[] =
{ 
  "Jenny Serra",
  "Brandon Cooper",
  "Gerald Xu",
  "Mandy",
  "Robert Schild",
  "Sarah Green",
  NULL,
};

/* A fairly dumb way to mangle an arbitrary alias into something that looks like
 * it *could* be a username; purely for aesthetics */
gchar*
alias_to_username (const gchar *alias)
{
  gchar *username;
  gchar *username_final;
  const gchar subst_char = '_';

  username = g_strdup (alias);
  g_strcanon (username, USERNAME_VALID_CHARS, subst_char);
  username_final = g_ascii_strdown (username, -1);

  g_free (username);

  return username_final;
}

/* Compare contacts very shallowly for the sake of tests where few attributes
 * will have been set in time for comparison; Tests that expect more fields to
 * be valid at the time they would call this function should define a more
 * exhaustive comparison */
gboolean
contacts_equal (EBookBackendTpContact *a, EBookBackendTpContact *b)
{
  if (!a && !b) return TRUE;

  /* ensure we can safely deref these below */
  if (!a || !b) return FALSE;

  g_return_val_if_fail (a->handle == b->handle, FALSE);
  g_return_val_if_fail (g_str_equal (a->name, b->name), FALSE);

  /* fields intentionally ignored (set in the opaque addition process or later):
   *    alias (may get set after 'contacts-removed' signal is emitted)
   *    status
   *    status_message
   *    avatar_token
   *    avatar_mime
   *    avatar_len
   *    avatar_data
   *    flags
   *    pending_flags
   *    uid
   *    master_uid
   *    ref_count (per-instance anyway)
   */

  return TRUE;
}
