/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *   @author Rob Bradford
 *   @author Travis Reitter <travis.reitter@maemo.org>
 *   @author Marco Barisione <marco.barisione@collabora.co.uk>
 *   @author Mathias Hasselmann <mathias.hasselmann@maemo.org>
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
#include "e-book-backend-tp-log.h"

/* DEBUG() messages for every ref/unref are really too much, but they
 * can be useful to debug crashes and memory corruptions.
 * Set VERBOSE_REF to 1 to print a message for each ref/unref. */
#define VERBOSE_REF 0

static gboolean
master_uids_differ (GPtrArray *a,
                    GPtrArray *b)
{
  guint i;

  if (a->len != b->len)
    return TRUE;

  for (i = 0; i < a->len; ++i)
    if (g_strcmp0 (a->pdata[i], b->pdata[i]))
      return TRUE;

  return FALSE;
}

static void
master_uids_free (GPtrArray *master_uids)
{
  g_ptr_array_foreach (master_uids, (GFunc) g_free, NULL);
  g_ptr_array_free (master_uids, TRUE);
}

static int
master_uids_find (GPtrArray *master_uids, const char *uid)
{
  guint i;

  for (i = 0; i < master_uids->len; ++i)
  {
    if (!g_strcmp0 (master_uids->pdata[i], uid))
      return i;
  }

  return -1;
}

static void
e_book_backend_tp_contact_free (EBookBackendTpContact *contact)
{
  g_free (contact->name);
  g_free (contact->alias);
  g_free (contact->status);
  g_free (contact->status_message);
  g_free (contact->avatar_token);
  g_free (contact->avatar_mime);
  g_free (contact->avatar_data);
  g_free (contact->contact_info);
  g_free (contact->uid);
  master_uids_free (contact->master_uids);
  g_hash_table_unref (contact->variants);
  g_slice_free (EBookBackendTpContact, contact);
}

EBookBackendTpContact *
e_book_backend_tp_contact_ref (EBookBackendTpContact *contact)
{
#if VERBOSE_REF
  DEBUG ("contact ref, uid = %s, handle = %d, old refcnt = %d",
      contact->uid, contact->handle, contact->ref_count);
#endif
  g_atomic_int_inc (&(contact->ref_count));
  return contact;
}

void
e_book_backend_tp_contact_unref (EBookBackendTpContact *contact)
{
#if VERBOSE_REF
  DEBUG ("contact unref, uid = %s, handle = %d, old refcnt = %d",
      contact->uid, contact->handle, contact->ref_count);
#endif
  if (g_atomic_int_dec_and_test (&(contact->ref_count)))
  {
    DEBUG ("freeing contact with uid=%s", contact->uid);
    e_book_backend_tp_contact_free (contact);
  }
}

EBookBackendTpContact *
e_book_backend_tp_contact_new (void)
{
  EBookBackendTpContact *contact;
  contact = g_slice_new0 (EBookBackendTpContact);
  contact->master_uids = g_ptr_array_new ();
  contact->status = g_strdup ("unknown");
  contact->generic_status = "unknown";
  contact->variants = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, NULL);
  e_book_backend_tp_contact_ref (contact);
  return contact;
}

EBookBackendTpContact *
e_book_backend_tp_contact_dup (EBookBackendTpContact *contact)
{
  EBookBackendTpContact *new_contact;
  guint i;
  GHashTableIter iter;
  gpointer key;

  new_contact = e_book_backend_tp_contact_new ();

  g_free (new_contact->status); /* allocated in new */

  new_contact->handle = contact->handle;
  new_contact->name = g_strdup (contact->name);
  new_contact->alias = g_strdup (contact->alias);
  new_contact->generic_status = contact->generic_status;
  new_contact->status = g_strdup (contact->status);
  new_contact->status_message = g_strdup (contact->status_message);
  new_contact->avatar_token = g_strdup (contact->avatar_token);
  new_contact->avatar_mime = g_strdup (contact->avatar_mime);
  new_contact->avatar_len = contact->avatar_len;
  new_contact->avatar_data = g_strdup (contact->avatar_data);
  new_contact->contact_info = g_strdup (contact->contact_info);
  new_contact->flags = contact->flags;
  new_contact->pending_flags = contact->pending_flags;
  new_contact->uid = g_strdup (contact->uid);
  new_contact->capabilities = contact->capabilities;

  g_ptr_array_set_size (new_contact->master_uids, contact->master_uids->len);
  for (i = 0; i < contact->master_uids->len; ++i)
  {
    g_ptr_array_add (new_contact->master_uids,
        g_strdup (contact->master_uids->pdata[i]));
  }

  g_hash_table_iter_init (&iter, contact->variants);
  while (g_hash_table_iter_next (&iter, &key, NULL)) 
    g_hash_table_insert (new_contact->variants, g_strdup (key),
        GUINT_TO_POINTER (TRUE));

  return new_contact;
}

gboolean
e_book_backend_tp_contact_is_visible (EBookBackendTpContact *contact)
{
    guint32 list_flags;

    if (contact->pending_flags & SCHEDULE_DELETE)
      /* The contact was deleted by the user but not deleted yet from the
       * server, for instance because we are offline. */
      return FALSE;

    /* Drop the non-list flags */
    list_flags = (contact->flags & ALL_LIST_FLAGS);
    if (list_flags == PUBLISH)
      /* The contact is *only* in the publish list, in this case we don't
       * show it to work-around some clients that don't remove deleted
       * contacts from publish. */
      return FALSE;

    return TRUE;
}

/**
 * Merge the vcard obtained from Telepathy ContactInfo interface to IM vcard
 */
static void 
e_book_backend_tp_merge_vcard_with_contact_info (EVCard *evc, 
                                                 EVCard *contactinfo)
{
  EVCardAttribute *attr = NULL;
  GList *l;
  
  for (l = e_vcard_get_attributes (contactinfo); l; l = l->next)
  {
    attr = (EVCardAttribute *)l->data;    
    e_vcard_add_attribute (evc, e_vcard_attribute_copy (attr));
  }
}

EContact *
e_book_backend_tp_contact_to_econtact (EBookBackendTpContact *contact, 
    const gchar *vcard_field, const gchar *profile_name)
{
  EVCardAttributeParam *param;
  EVCardAttribute *attr;
  EContact *econtact;
  gchar *avatar_path;
  EVCard *evc;
  gchar *tmp;
  guint i;

  econtact = e_contact_new ();
  evc = E_VCARD (econtact);

  attr = e_vcard_attribute_new (NULL, "UID");
  e_vcard_add_attribute_with_value (evc, attr, contact->uid);

  attr = e_vcard_attribute_new (NULL, vcard_field);
  e_vcard_attribute_add_param_with_value (attr,
      e_vcard_attribute_param_new (EVC_TYPE), profile_name);
  e_vcard_attribute_add_param_with_value (attr,
      e_vcard_attribute_param_new ("X-OSSO-VALID"),
      contact->flags & CONTACT_INVALID ? "no" : "yes");
  e_vcard_add_attribute_with_value (evc, attr, contact->name);

  if (g_hash_table_size (contact->variants))
  {
    GHashTableIter iter;
    gpointer key;

    param = e_vcard_attribute_param_new ("X-OSSO-VARIANTS");

    g_hash_table_iter_init (&iter, contact->variants);
    while (g_hash_table_iter_next (&iter, &key, NULL)) 
      e_vcard_attribute_param_add_value (param, key);

    e_vcard_attribute_add_param (attr, param);
  }

  /* If the contact has no alias then the connection manager will just
   * return the contact ID as alias. Showing it in the UI is unneeded
   * (the contact ID is available in other fields) and ugly, so in this
   * case we don't set it. */
  if (contact->alias && !g_str_equal (contact->alias, contact->name))
  {
    attr = e_vcard_attribute_new (NULL, "NICKNAME");
    e_vcard_add_attribute_with_value (evc, attr, contact->alias);
  }

  for (i = 0; i < contact->master_uids->len; ++i)
  {
    attr = e_vcard_attribute_new (NULL, "X-OSSO-MASTER-UID");
    e_vcard_add_attribute_with_value (evc, attr,
        contact->master_uids->pdata[i]);
  }

  if (contact->handle)
  {
    tmp = g_strdup_printf ("%d", contact->handle);
    attr = e_vcard_attribute_new (NULL, "X-TELEPATHY-HANDLE");
    e_vcard_add_attribute_with_value (evc, attr, tmp);
    g_free (tmp);
  }

  attr = e_vcard_attribute_new (NULL, "X-TELEPATHY-SUBSCRIBED");
  if (contact->flags & SUBSCRIBE)
  {
    e_vcard_add_attribute_with_value (evc, attr, "yes");
  } else if (contact->flags & SUBSCRIBE_LOCAL_PENDING) {
    e_vcard_add_attribute_with_value (evc, attr, "local-pending");
  } else if (contact->flags & SUBSCRIBE_REMOTE_PENDING) {
    e_vcard_add_attribute_with_value (evc, attr, "remote-pending");
  } else {
    e_vcard_add_attribute_with_value (evc, attr, "no");
  }

  attr = e_vcard_attribute_new (NULL, "X-TELEPATHY-PUBLISHED");
  if (contact->flags & PUBLISH)
  {
    e_vcard_add_attribute_with_value (evc, attr, "yes");
  } else if (contact->flags & PUBLISH_LOCAL_PENDING) {
    e_vcard_add_attribute_with_value (evc, attr, "local-pending");
  } else if (contact->flags & PUBLISH_REMOTE_PENDING) {
    e_vcard_add_attribute_with_value (evc, attr, "remote-pending");
  } else {
    e_vcard_add_attribute_with_value (evc, attr, "no");
  }

  attr = e_vcard_attribute_new (NULL, "X-TELEPATHY-BLOCKED");
  if (contact->flags & DENY)
  {
    e_vcard_add_attribute_with_value (evc, attr, "yes");
  } else if (contact->flags & DENY_LOCAL_PENDING) {
    e_vcard_add_attribute_with_value (evc, attr, "local-pending");
  } else if (contact->flags & DENY_REMOTE_PENDING) {
    e_vcard_add_attribute_with_value (evc, attr, "remote-pending");
  } else {
    e_vcard_add_attribute_with_value (evc, attr, "no");
  }

  if (contact->status)
  {
    attr = e_vcard_attribute_new (NULL, "X-TELEPATHY-PRESENCE");

    e_vcard_attribute_add_value (attr, contact->status);

    if (!g_str_equal (contact->generic_status, contact->status))
    {
      e_vcard_attribute_add_value (attr, contact->generic_status);
    }

    if (contact->status_message)
    {
      e_vcard_attribute_add_value (attr, contact->status_message);
    }

    e_vcard_add_attribute (evc, attr);
  }

  if (contact->capabilities > 0)
  {
    DEBUG ("including capabilities");
    attr = e_vcard_attribute_new (NULL, "X-TELEPATHY-CAPABILITIES");

    e_vcard_add_attribute (evc, attr);

    if (contact->capabilities & CAP_TEXT)
      e_vcard_attribute_add_value (attr, "text");

    if (contact->capabilities & CAP_VOICE)
      e_vcard_attribute_add_value (attr, "voice");

    if (contact->capabilities & CAP_VIDEO)
      e_vcard_attribute_add_value (attr, "video");

    if (contact->capabilities & CAP_IMMUTABLE_STREAMS)
      e_vcard_attribute_add_value (attr, "immutable-streams");
  }

  if (contact->avatar_token && contact->avatar_token[0])
  {
    avatar_path = g_build_filename (g_get_home_dir (), ".osso-abook", "avatars",
        contact->avatar_token, NULL);

    if (g_file_test (avatar_path, G_FILE_TEST_EXISTS))
    {
      tmp = g_filename_to_uri (avatar_path, NULL, NULL);
      if (tmp)
      {
        attr = e_vcard_attribute_new (NULL, "PHOTO");
        e_vcard_add_attribute_with_value (evc, attr, tmp);
        param = e_vcard_attribute_param_new ("VALUE");
        e_vcard_attribute_add_param_with_value (attr, param, "URI");
        g_free (tmp);
      }
    }
    g_free (avatar_path);
  }

  if (contact->contact_info)
  {
    EVCard *contactinfo = e_vcard_new_from_string (contact->contact_info);
    e_book_backend_tp_merge_vcard_with_contact_info (evc, contactinfo);
    g_object_unref (contactinfo);
  }

  /* FIXME - do not call e_vcard_to_string if tmp is not going to be
     printed */
  tmp = e_vcard_to_string (E_VCARD (econtact), EVC_FORMAT_VCARD_30);
  DEBUG ("generated vcard: %s", tmp);
  g_free (tmp);

  return econtact;
}

static void
e_book_backend_tp_contact_update_tp_attribute (EBookBackendTpContact *contact,
    EVCardAttribute *attr, const char *flag_name, guint32 contact_flag)
{
  gchar *tmp;

  tmp = e_vcard_attribute_get_value (attr);

  if (g_str_equal (tmp, "yes"))
  {
    if (!(contact->flags & contact_flag))
    {
      contact->pending_flags |= contact_flag;
      contact->pending_flags |= SCHEDULE_UPDATE_FLAGS;
      DEBUG ("setting previously unset %s flag", flag_name);
    }
  } else if (g_str_equal (tmp, "no")) {
    if (contact->flags & contact_flag)
    {
      contact->pending_flags &= contact_flag;
      contact->pending_flags |= SCHEDULE_UPDATE_FLAGS;
      DEBUG ("clearing previously set %s flag", flag_name);
    }
  }

  g_free (tmp);
}

/* Returns TRUE for success, FALSE otherwise.
 *
 * If this function returns FALSE, the caller is responsible for freeing any
 * memory associated with @contact (including @contact itself). */
gboolean
e_book_backend_tp_contact_update_from_econtact (EBookBackendTpContact *contact,
    EContact *ec, const gchar *vcard_field)
{
  EVCard *evc = E_VCARD (ec);
  EVCardAttribute *attr = NULL;
  const gchar *attr_name;
  GPtrArray *master_uids;
  GList *l;

  contact->pending_flags = contact->flags;
  master_uids = g_ptr_array_new ();

  for (l = e_vcard_get_attributes (evc); l; l = l->next)
  {
    attr = (EVCardAttribute *)l->data;
    attr_name = e_vcard_attribute_get_name (attr);

    if (g_str_equal (attr_name, "X-OSSO-MASTER-UID")) {
      g_ptr_array_add (master_uids, e_vcard_attribute_get_value (attr));
      continue;
    }

    if (g_str_equal (attr_name, vcard_field)) {
      char *new_name;

      new_name = e_vcard_attribute_get_value (attr);

      /* Assume we are only in the case of creating a new contact. I don't
       * think the user is going to be able to update the IM id.
       * Don't warn when setting the name to the same value (this happens when
       * merging contacts)
       */
      g_warn_if_fail (contact->name == NULL ||
                      !g_strcmp0 (contact->name, new_name));

      g_free (contact->name);
      contact->name = new_name;
      continue;
    }

    if (g_str_has_prefix (attr_name, "X-TELEPATHY"))
    {
      if (g_str_equal (attr_name, "X-TELEPATHY-SUBSCRIBED")) {
        e_book_backend_tp_contact_update_tp_attribute
            (contact, attr, "SUBSCRIBE", SUBSCRIBE);
      } else if (g_str_equal (attr_name, "X-TELEPATHY-PUBLISHED")) {
        e_book_backend_tp_contact_update_tp_attribute
            (contact, attr, "PUBLISH", PUBLISH);
      } else if (g_str_equal (attr_name, "X-TELEPATHY-BLOCKED")) {
        e_book_backend_tp_contact_update_tp_attribute
            (contact, attr, "DENY", DENY);
      }

      continue;
    }
  }

  if (master_uids_differ (master_uids, contact->master_uids))
  {
    DEBUG ("master UIDs changed, marking for update: %s", contact->name);
    contact->pending_flags |= SCHEDULE_UPDATE_MASTER_UID;
    master_uids_free (contact->master_uids);
    contact->master_uids = master_uids;
    master_uids = NULL;
  } else {
    master_uids_free (master_uids);
  }

  if (!contact->name || g_str_equal (contact->name, ""))
  {
    WARNING ("new Telepathy contact has invalid value for "
        "required attribute %s: '%s'", vcard_field, contact->name);

    return FALSE;
  }

  return TRUE;
}

gboolean
e_book_backend_tp_contact_update_name (EBookBackendTpContact *contact,
                                       const gchar           *new_name)
{
  g_return_val_if_fail (new_name, FALSE);

  if (g_strcmp0 (contact->name, new_name) == 0)
    return FALSE;

  DEBUG ("updating name for %s from %s to %s\n", contact->uid, contact->name,
      new_name);

  if (contact->name)
  {
    DEBUG ("adding name variant %s to %s", contact->name, new_name);

    g_hash_table_insert (contact->variants, g_strdup (contact->name),
        GUINT_TO_POINTER (TRUE));
    contact->pending_flags |= SCHEDULE_UPDATE_VARIANTS;

    g_free (contact->name);
  }

  contact->name = g_strdup (new_name);

  return TRUE;
}

gboolean
e_book_backend_tp_contact_update_master_uids (EBookBackendTpContact *contact,
                                              GPtrArray             *master_uids)
{
  gboolean changed = FALSE;
  guint i;

  for (i = 0; i < master_uids->len; ++i)
  {
    const char *uid = master_uids->pdata[i];

    if (master_uids_find (contact->master_uids, uid) < 0)
    {
      DEBUG ("adding master UID %s to %s", uid, contact->name);
      g_ptr_array_add (contact->master_uids, g_strdup (uid));
      contact->pending_flags |= SCHEDULE_UPDATE_MASTER_UID;
      changed = TRUE;
    }
  }

  return changed;
}

gboolean
e_book_backend_tp_contact_remove_master_uid (EBookBackendTpContact *contact,
                                             const char            *uid)
{
  int i;
  gchar *master_uid;

  i = master_uids_find (contact->master_uids, uid);

  if (i < 0)
    return FALSE;

  master_uid = g_ptr_array_remove_index_fast (contact->master_uids, i);
  contact->pending_flags |= SCHEDULE_UPDATE_MASTER_UID;
  g_free (master_uid);

  return TRUE;
}

void
e_book_backend_tp_contact_remove_all_master_uids (EBookBackendTpContact *contact)
{
  if (contact->master_uids->len > 0)
  {
    contact->pending_flags |= SCHEDULE_UPDATE_MASTER_UID;
    g_ptr_array_foreach (contact->master_uids, (GFunc) g_free, NULL);
    g_ptr_array_remove_range (contact->master_uids, 0, contact->master_uids->len);
  }
}

void
e_book_backend_tp_contact_add_variants_from_contact (
    EBookBackendTpContact *dest, EBookBackendTpContact *src)
{
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, src->variants);
  while (g_hash_table_iter_next (&iter, &key, NULL)) 
    g_hash_table_insert (dest->variants, g_strdup (key),
        GUINT_TO_POINTER (TRUE));

  if (g_hash_table_size (src->variants))
    dest->pending_flags |= SCHEDULE_UPDATE_VARIANTS;
}
