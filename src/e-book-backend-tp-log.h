/* vim: set ts=2 sw=2 cino= et: */
/*
 * This file is part of eds-backend-telepathy
 *
 * Copyright (C) 2008-2009 Nokia Corporation
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

#ifndef _E_BOOK_BACKEND_TP_LOG_H__
#define _E_BOOK_BACKEND_TP_LOG_H__

#include <glib.h>

extern const gchar *e_book_backend_tp_log_domain_id;

#define DEBUG(format, ...) \
  g_log (e_book_backend_tp_log_domain_id, G_LOG_LEVEL_DEBUG, format, \
      ##__VA_ARGS__)

#define MESSAGE(format, ...) \
  g_log (e_book_backend_tp_log_domain_id, G_LOG_LEVEL_MESSAGE, format, \
      ##__VA_ARGS__)

#define WARNING(format, ...) \
  g_log (e_book_backend_tp_log_domain_id, G_LOG_LEVEL_WARNING, format, \
      ##__VA_ARGS__)

#endif /* _E_BOOK_BACKEND_TP_LOG_H_ */
