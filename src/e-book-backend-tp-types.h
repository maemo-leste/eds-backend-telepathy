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

#include <glib.h>

#ifndef _E_BOOK_BACKEND_TP_TYPES_H__
#define _E_BOOK_BACKEND_TP_TYPES_H__

/* XXX: this must match the relationship between items in
 * EBookBackendTpContactListId and EBookBackendTpContactFlag */
#define CONTACT_FLAG_FROM_ID(x)  (1 << (x))

/* XXX: this must match the relative ordering in EBookBackendTpContactListId */
#define CONTACT_LIST_ID_GET_LOCAL_FROM_CURRENT(x)  ((x) + 1)
#define CONTACT_LIST_ID_GET_REMOTE_FROM_CURRENT(x) ((x) + 2)

/* XXX: this must match the relative ordering in EBookBackendTpContactListId */
#define CONTACT_FLAG_GET_LOCAL_FROM_CURRENT(x)  ((x) << 1)
#define CONTACT_FLAG_GET_REMOTE_FROM_CURRENT(x) ((x) << 2)

/* XXX: this must match the relative ordering in the CONTACT_LIST_ID_GET_*
 * macros above */
typedef enum
{
  CL_SUBSCRIBE = 0,
  CL_SUBSCRIBE_LOCAL_PENDING,
  CL_SUBSCRIBE_REMOTE_PENDING,

  CL_PUBLISH,
  CL_PUBLISH_LOCAL_PENDING,
  CL_PUBLISH_REMOTE_PENDING,

  CL_ALLOW,
  CL_ALLOW_LOCAL_PENDING,
  CL_ALLOW_REMOTE_PENDING,

  CL_DENY,
  CL_DENY_LOCAL_PENDING,
  CL_DENY_REMOTE_PENDING,

  CL_STORED,
  CL_STORED_LOCAL_PENDING,
  CL_STORED_REMOTE_PENDING,

  CL_LAST_LIST,
} EBookBackendTpContactListId;

/* these are only the IDs associated with high-level Telepathy lists (ie, they
 * don't include the local_pending and remote_pending flattened into the same
 * namespace) */
typedef enum
{
  CL_PRIMARY_SUBSCRIBE = CL_SUBSCRIBE,
  CL_PRIMARY_PUBLISH = CL_PUBLISH,
  CL_PRIMARY_ALLOW = CL_ALLOW,
  CL_PRIMARY_DENY = CL_DENY,
  CL_PRIMARY_STORED = CL_STORED,

  CL_PRIMARY_LAST_LIST,
} EBookBackendTpPrimaryContactListId;

typedef enum
{
  /* These relate to the TP state */
  SUBSCRIBE = 1 << CL_SUBSCRIBE,
  SUBSCRIBE_LOCAL_PENDING = 1 << CL_SUBSCRIBE_LOCAL_PENDING,
  SUBSCRIBE_REMOTE_PENDING = 1 << CL_SUBSCRIBE_REMOTE_PENDING,

  PUBLISH = 1 << CL_PUBLISH,
  PUBLISH_LOCAL_PENDING = 1 << CL_PUBLISH_LOCAL_PENDING,
  PUBLISH_REMOTE_PENDING = 1 << CL_PUBLISH_REMOTE_PENDING,

  ALLOW = 1 << CL_ALLOW,
  ALLOW_LOCAL_PENDING = 1 << CL_ALLOW_LOCAL_PENDING,
  ALLOW_REMOTE_PENDING = 1 << CL_ALLOW_REMOTE_PENDING,

  DENY = 1 << CL_DENY,
  DENY_LOCAL_PENDING = 1 << CL_DENY_LOCAL_PENDING,
  DENY_REMOTE_PENDING = 1 << CL_DENY_REMOTE_PENDING,

  STORED = 1 << CL_STORED,
  STORED_LOCAL_PENDING = 1 << CL_STORED_LOCAL_PENDING,
  STORED_REMOTE_PENDING = 1 << CL_STORED_REMOTE_PENDING,

  LAST_LIST_FLAG = 1 << CL_LAST_LIST,

  CONTACT_UNSEEN = LAST_LIST_FLAG << 1,
  SCHEDULE_DELETE = LAST_LIST_FLAG << 2,
  SCHEDULE_UPDATE_FLAGS = LAST_LIST_FLAG << 3,
  UNUSED_FLAG = LAST_LIST_FLAG << 4, /* was used for updating aliases */
  SCHEDULE_ADD = LAST_LIST_FLAG << 5,
  SCHEDULE_UPDATE_MASTER_UID = LAST_LIST_FLAG << 6,
  CONTACT_INVALID = LAST_LIST_FLAG << 7,
  SCHEDULE_UNBLOCK = LAST_LIST_FLAG << 8,
  SCHEDULE_UPDATE_VARIANTS = LAST_LIST_FLAG << 9,
} EBookBackendTpContactFlag;

/* Flags corresponding only to roster lists */
#define ALL_LIST_FLAGS \
  (SUBSCRIBE | \
   SUBSCRIBE_LOCAL_PENDING | \
   SUBSCRIBE_REMOTE_PENDING | \
   PUBLISH | \
   PUBLISH_LOCAL_PENDING | \
   PUBLISH_REMOTE_PENDING | \
   ALLOW | \
   ALLOW_LOCAL_PENDING | \
   ALLOW_REMOTE_PENDING | \
   DENY | \
   DENY_LOCAL_PENDING | \
   DENY_REMOTE_PENDING | \
   STORED | \
   STORED_LOCAL_PENDING | \
   STORED_REMOTE_PENDING)

typedef enum
{
  CAP_TEXT = 1 << 1,
  CAP_VOICE = 1 << 2,
  CAP_VIDEO = 1 << 3,
  CAP_IMMUTABLE_STREAMS = 1 << 4,
} EBookBackendTpContactCapabilities;

typedef struct  _EBookBackendTpContact EBookBackendTpContact;

typedef enum
{
  E_BOOK_BACKEND_TP_ERROR_FAILED_ASSERTION
} EBookBackendTpError;

#define E_BOOK_BACKEND_TP_ERROR e_book_backend_tp_error_quark ()

GQuark e_book_backend_tp_error_quark (void);

#define e_book_backend_tp_return_val_with_error_if_fail(expr,val,error) \
  G_STMT_START{ \
    if G_LIKELY(expr) { } \
    else \
    { \
      g_set_error (error, E_BOOK_BACKEND_TP_ERROR, \
          E_BOOK_BACKEND_TP_ERROR_FAILED_ASSERTION, \
          "The expression '%s' failed", #expr); \
      /* We cannot use directly g_return_if_fail_warning as it's private */ \
      g_return_val_if_fail (expr, val); \
    }; \
  } G_STMT_END;

#endif /* _E_BOOK_BACKEND_TP_TYPES_H_ */
