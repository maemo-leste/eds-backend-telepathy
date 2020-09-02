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

#include <sys/vfs.h>
#include <unistd.h>
#include <sqlite3.h>
#include <string.h>
#include <errno.h>

#include <glib/gstdio.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/account-manager.h>

#include "e-book-backend-tp-db.h"
#include "e-book-backend-tp-log.h"


#define GET_PRIVATE(o) \
  ((EBookBackendTpDbPrivate *) e_book_backend_tp_db_get_instance_private (E_BOOK_BACKEND_TP_DB (o)))

#define e_book_backend_tp_db_begin(tpdb)    (e_book_backend_tp_db_real_begin ((tpdb), G_STRFUNC))
#define e_book_backend_tp_db_commit(tpdb)   (e_book_backend_tp_db_real_commit ((tpdb), G_STRFUNC))
#define e_book_backend_tp_db_rollback(tpdb) (e_book_backend_tp_db_real_rollback ((tpdb), G_STRFUNC))

#define RESTORE_DB_EXTENSION ".restore"

typedef enum
{
  QUERY_BEGIN_TRANSACTION,
  QUERY_COMMIT_TRANSACTION,
  QUERY_ROLLBACK_TRANSACTION,

  QUERY_FETCH_CONTACTS,
  QUERY_FETCH_MASTER_UIDS,

  QUERY_INSERT_CONTACT,
  QUERY_INSERT_MASTER_UID,

  QUERY_DELETE_CONTACT,
  QUERY_DELETE_MASTER_UIDS,

  QUERY_UPDATE_CONTACT,

  /* Queries that need the new database format with support for
   * non-normalized variants */
  QUERY_FETCH_VARIANTS,
  FIRST_VARIANTS_QUERY=QUERY_FETCH_VARIANTS, /* keep in sync */

  QUERY_INSERT_VARIANT,

  QUERY_DELETE_VARIANTS,
} QueryType;

/* This syntax is for C99's Designated Initializers */
static const gchar *queries[] = {
  [QUERY_BEGIN_TRANSACTION] =
    "BEGIN TRANSACTION",
  [QUERY_COMMIT_TRANSACTION] =
    "COMMIT TRANSACTION",
  [QUERY_ROLLBACK_TRANSACTION] =
    "ROLLBACK TRANSACTION",

  [QUERY_FETCH_CONTACTS] =
    "SELECT * from `contacts` ORDER BY `uid`",
  [QUERY_FETCH_MASTER_UIDS] =
    "SELECT * from `master_uids` ORDER BY `contact_uid`",

  [QUERY_INSERT_CONTACT] =
    "INSERT INTO `contacts` "
    "  (`uid`, `name`, `alias`, `avatar_token`, `flags`, `pending_flags`, `contact_info`)"
    "  VALUES (:uid, :name, :alias, :avatar_token, :flags, :pending_flags, :contact_info)",
  [QUERY_INSERT_MASTER_UID] =
    "INSERT OR IGNORE INTO `master_uids` "
    "  (`contact_uid`, `master_uid`)"
    "  VALUES (:contact_uid, :master_uid)",

  [QUERY_DELETE_CONTACT] =
    "DELETE FROM `contacts` WHERE `uid`=:uid",
  [QUERY_DELETE_MASTER_UIDS] =
    "DELETE FROM `master_uids` WHERE `contact_uid`=:uid",

  [QUERY_UPDATE_CONTACT] =
    "UPDATE `contacts` SET `name`=:name, `alias`=:alias,"
    "  `avatar_token`=:avatar_token, `flags`=:flags,"
    "  `pending_flags`=:pending_flags, `contact_info`=:contact_info WHERE `uid`=:uid",

  [QUERY_FETCH_VARIANTS] =
    "SELECT * from `variants` ORDER BY `contact_uid`",

  [QUERY_INSERT_VARIANT] =
    "INSERT OR IGNORE INTO `variants` "
    "  (`contact_uid`, `variant`)"
    "  VALUES (:contact_uid, :variant)",

  [QUERY_DELETE_VARIANTS] =
    "DELETE FROM `variants` WHERE `contact_uid`=:uid",
};

typedef struct _EBookBackendTpDbPrivate EBookBackendTpDbPrivate;

struct _EBookBackendTpDbPrivate {
  sqlite3_stmt *statements[G_N_ELEMENTS(queries)];
  sqlite3      *db;
  gchar        *filename;
};

G_DEFINE_TYPE_WITH_PRIVATE (EBookBackendTpDb,
                            e_book_backend_tp_db,
                            G_TYPE_OBJECT)

/* Support for non-normalized variants was added later */
#define VARIANTS_SCHEMA \
  "CREATE TABLE `variants` (" \
  "  `contact_uid`   TEXT," \
  "  `variant`       TEXT," \
  "  PRIMARY KEY     (`contact_uid`, `variant`)" \
  ");" \
  \
  "CREATE INDEX `variants_i1` ON `variants`(`contact_uid`);"

static const char complete_schema[] =
  "CREATE TABLE `contacts` ("
  "  `uid`           TEXT PRIMARY KEY,"
  "  `name`          TEXT NOT NULL,"
  "  `alias`         TEXT NULL,"
  "  `avatar_token`  TEXT NULL,"
  "  `flags`         INTEGER,"
  "  `pending_flags` INTEGER,"
  "  `contact_info`  TEXT NULL"
  ");"

  "CREATE TABLE `master_uids` ("
  "  `contact_uid`   TEXT,"
  "  `master_uid`    TEXT,"
  "  PRIMARY KEY     (`contact_uid`, `master_uid`)"
  ");"

  "CREATE INDEX `contacts_i1` ON `contacts`(`uid`);"
  "CREATE INDEX `master_uids_i1` ON `master_uids`(`contact_uid`);"

  VARIANTS_SCHEMA;

static GMutex account_cleanup_mutex;

GQuark
e_book_backend_tp_db_error (void)
{
  return g_quark_from_static_string ("e-book-backend-tp-db-error-quark");
}

static void
e_book_backend_tp_db_get_property (GObject *object, guint property_id,
                              GValue *value, GParamSpec *pspec)
{
  switch (property_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
e_book_backend_tp_db_set_property (GObject *object, guint property_id,
                              const GValue *value, GParamSpec *pspec)
{
  switch (property_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
e_book_backend_tp_db_dispose (GObject *object)
{
  if (G_OBJECT_CLASS (e_book_backend_tp_db_parent_class)->dispose)
    G_OBJECT_CLASS (e_book_backend_tp_db_parent_class)->dispose (object);
}

static void
e_book_backend_tp_db_finalize (GObject *object)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (object);

  g_free (priv->filename);

  if (G_OBJECT_CLASS (e_book_backend_tp_db_parent_class)->finalize)
    G_OBJECT_CLASS (e_book_backend_tp_db_parent_class)->finalize (object);
}

static void
e_book_backend_tp_db_class_init (EBookBackendTpDbClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = e_book_backend_tp_db_get_property;
  object_class->set_property = e_book_backend_tp_db_set_property;
  object_class->dispose = e_book_backend_tp_db_dispose;
  object_class->finalize = e_book_backend_tp_db_finalize;
}

static void
e_book_backend_tp_db_init (EBookBackendTpDb *self)
{
}

EBookBackendTpDb *
e_book_backend_tp_db_new (void)
{
  return g_object_new (E_TYPE_BOOK_BACKEND_TP_DB, NULL);
}

static const gchar *
get_db_directory (void)
{
  static gchar *db_directory = NULL;

  if (!db_directory)
    db_directory = g_build_filename (g_get_home_dir (), ".osso-abook",
        "db", "tp-cache", NULL);

  return db_directory;
}

static gchar *
db_filename_for_account (const gchar *account_name)
{
  gchar *converted_account_name;
  gchar *s;
  gchar *filename;

  converted_account_name = g_strdup (account_name);
  s = converted_account_name;

  while ((s = g_utf8_find_next_char (s, NULL)) != NULL)
  {
    if (s[0] == '/')
      s[0] = '_';

    if (s[0] == '\0')
      break;
  }

  filename = g_build_filename (get_db_directory (),
      converted_account_name, NULL);

  g_free (converted_account_name);

  return filename;
}

static gchar *
get_restore_db_filename (const gchar *db_name)
{
  return g_strconcat (db_name, RESTORE_DB_EXTENSION, NULL);
}

static gboolean
create_tables (sqlite3 *db, const gchar *schema)
{
  const gchar *head = schema;
  const gchar *tail = NULL;
  sqlite3_stmt *statement;
  int res;

  do
  {
    res = sqlite3_prepare_v2 (db, head, -1, &statement, &tail);

    if (res != SQLITE_OK)
    {
      WARNING ("error whilst preparing statement for schema: %s",
          sqlite3_errmsg (db));
      return FALSE;
    }

    res = sqlite3_step (statement);

    if (res != SQLITE_DONE)
    {
      WARNING ("error whilst stepping the statement for schema: %s",
          sqlite3_errmsg (db));
      return FALSE;
    }

    res = sqlite3_finalize (statement);

    if (res != SQLITE_OK)
    {
      WARNING ("error whilst finalizing the statement for schema: %s",
          sqlite3_errmsg (db));
      return FALSE;
    }

  } while (*(head = tail));

  return TRUE;
}

static gboolean
prepare_statements (EBookBackendTpDbPrivate *priv)
{
  const gchar *tail = NULL;
  int res;
  guint i;
  gboolean variants_created = FALSE;

  for (i = 0; i < G_N_ELEMENTS (queries); i++)
  {
    res = sqlite3_prepare_v2 (priv->db, queries[i], -1,
        &(priv->statements[i]), &tail);

    if (res != SQLITE_OK)
    {
      if (i == FIRST_VARIANTS_QUERY && !variants_created)
      {
        /* Migrate from the old DB format to the new one with non-normalized
         * name variants */
        create_tables (priv->db, VARIANTS_SCHEMA);
        /* Retry */
        i--;
        /* Avoid loops if this statement cannot really be prepared */
        variants_created = TRUE;
      } else {
        /* FIXME: do GError stuff */
        WARNING ("error when trying to prepare statement (i=%d): %s",
            i, sqlite3_errmsg (priv->db));
        return FALSE;
      }
    }
  }

  return TRUE;
}

static void
remove_accounts_from_hash_table (GHashTable *account_dbs,
                                 const GList *accounts)
{
  GList *l;
  gchar *db_name;

  for (l = accounts; l; l = l->next) {
    const gchar *path_suffix = tp_account_get_path_suffix(l->data);

    db_name = db_filename_for_account (path_suffix);
    g_hash_table_remove (account_dbs, db_name);
    g_free (db_name);
  }
}

static void
am_prepared_cb(GObject *object, GAsyncResult *res, gpointer user_data)
{
  TpAccountManager *manager = (TpAccountManager *)object;
  GHashTable *account_dbs = user_data;
  GHashTableIter iter;
  gpointer key, value;
  GError *error = NULL;
  GList *accounts;

  if (!tp_proxy_prepare_finish (object, res, &error))
  {
    WARNING ("error whilst retrieving list of accounts: %s",
             error->message);
    g_hash_table_unref (account_dbs);
    return;
  }

  accounts = tp_account_manager_dup_valid_accounts(manager);
  remove_accounts_from_hash_table (account_dbs, accounts);
  g_list_free_full (accounts, g_object_unref);

  /* The mutex protects us from the (very unlikely tbh) creation of a new DB
   * between the call to g_stat and g_unlink. */
  g_mutex_lock (&account_cleanup_mutex);

  g_hash_table_iter_init (&iter, account_dbs);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    time_t old_mtime;
    gchar *db_name;
    struct stat stat_buf;

    db_name = key;
    old_mtime = GPOINTER_TO_INT (value);

    if (g_stat (db_name, &stat_buf) < 0) {
      WARNING ("error whilst retrieving file modification time on %s", db_name);
      continue;
    }

    if (old_mtime == stat_buf.st_mtime) {
      /* Not changed since we listed the directory, we can delete the file */
      g_unlink (db_name);
    }
  }

  g_mutex_unlock (&account_cleanup_mutex);

  g_hash_table_unref (account_dbs);
  g_object_unref(manager);
}

void
e_book_backend_tp_db_cleanup_unused_dbs (void)
{
  GHashTable *account_dbs;
  GDir *dir;
  const gchar *db_name;
  TpDBusDaemon *dbus_daemon;
  TpAccountManager *manager;

  /* A race condition could lead us to delete some used DBs if:
   * 1. A new account (and the corresponding DB) is created
   * 3. We read the file list
   * 3. mc_account_manager_get_valid_accounts returns the non-updated list
   * 4. We delete the new DB
   *
   * Reading the list of files and storing their mtime before creating the
   * McAccountManager fixes the race.
   */

  dir = g_dir_open (get_db_directory (), 0, NULL);
  if (!dir)
    return;

  account_dbs = g_hash_table_new_full (g_str_hash, g_str_equal,
      (GDestroyNotify) g_free, NULL);

  while ((db_name = g_dir_read_name (dir))) {
    gchar *db_path;
    struct stat stat_buf;

    if (g_str_has_suffix (db_name, RESTORE_DB_EXTENSION))
      continue;

    db_path = g_build_filename (get_db_directory (), db_name, NULL);

    if (g_stat (db_path, &stat_buf) < 0) {
      WARNING ("error whilst retrieving file modification time on %s", db_name);
      g_free (db_path);
      continue;
    }

    g_hash_table_insert (account_dbs, db_path,
        GINT_TO_POINTER (stat_buf.st_mtime));
  }

  dbus_daemon = tp_dbus_daemon_dup (NULL);
  manager = tp_account_manager_new (dbus_daemon);
  tp_proxy_prepare_async (manager, NULL, am_prepared_cb, account_dbs);

  g_object_unref (dbus_daemon);
  g_dir_close (dir);
}

static gboolean
e_book_backend_tp_db_open_real (EBookBackendTpDb *tpdb,
    const gchar *account_name, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  gchar *dirname;
  gboolean mutex_locked = FALSE;
  gchar *db_restore_name;
  int res;

  g_assert (!priv->filename);
  priv->filename = db_filename_for_account (account_name);

  dirname = g_path_get_dirname (priv->filename);

  if (0 != g_mkdir_with_parents (dirname, 0700))
  {
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
        "Cannot create database folder (%s): %s", dirname,
        g_strerror (errno));

    goto failure;
  }

  g_mutex_lock (&account_cleanup_mutex);
  mutex_locked = TRUE;

  /* If there is a DB meant for restore (and created by the backup app)
   * we just rename it and open the renamed file. If there is no backup
   * then g_rename will just silently fail */
  db_restore_name = get_restore_db_filename (priv->filename);
  g_rename (db_restore_name, priv->filename);
  g_free (db_restore_name);

  DEBUG ("opening database: %s", priv->filename);

#if SQLITE_VERSION_NUMBER > 3005000
  res = sqlite3_open_v2 (priv->filename, &(priv->db), SQLITE_OPEN_READWRITE,
      NULL);

  if (res != SQLITE_OK)
  {
    if (res == SQLITE_CANTOPEN)
    {
      /* sqlite3_open_v2 sets the out db argument even in case of error */
      sqlite3_close (priv->db);

      MESSAGE ("creating new database: %s", priv->filename);
      res = sqlite3_open_v2 (priv->filename, &(priv->db),
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);

      if (res != SQLITE_OK)
      {
        WARNING ("error whilst trying to open database: %s",
            sqlite3_errmsg (priv->db));
        g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
            E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
            "Error whilst trying to open database: %s",
            sqlite3_errmsg (priv->db));
        goto failure;
      }

      if (!create_tables (priv->db, complete_schema))
      {
        WARNING ("error whilst trying to create tables");
        g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
            E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
            "Error whilst trying to create tables: %s",
            sqlite3_errmsg (priv->db));
        goto failure;
      }
    } else {
      WARNING ("error whilst trying to open database: %s",
          sqlite3_errmsg (priv->db));
      g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
          E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
          "Error whilst trying to open database: %s",
          sqlite3_errmsg (priv->db));
      goto failure;
    }
  } else {
    DEBUG ("opening pre-existing database");
  }
#else

  if (g_file_test (priv->filename, G_FILE_TEST_EXISTS))
  {
    DEBUG ("opening pre-existing database");

    res = sqlite3_open (priv->filename, &(priv->db));

    if (res != SQLITE_OK)
    {
      WARNING ("error whilst tring to open database: %s",
          sqlite3_errmsg (priv->db));
        g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
            E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
            "Error whilst trying to open database: %s",
            sqlite3_errmsg (priv->db));
      goto failure;
    }
  } else {
    MESSAGE ("creating new database: %s", priv->filename);

    res = sqlite3_open (priv->filename, &(priv->db));

    if (res != SQLITE_OK)
    {
      WARNING ("error whilst tring to open database: %s",
          sqlite3_errmsg (priv->db));
        g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
            E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
            "Error whilst trying to open database: %s",
            sqlite3_errmsg (priv->db));
      goto failure;
    } else {
      if (!create_tables (priv->db, complete_schema))
      {
        WARNING ("error whilst trying to create tables");
        g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
            E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
            "Error whilst trying to create tables: %s",
            sqlite3_errmsg (priv->db));

        goto failure;
      }
    }
  }

#endif

  if (mutex_locked) {
    g_mutex_unlock (&account_cleanup_mutex);
    mutex_locked = FALSE;
  }

  if (!prepare_statements (priv))
    goto failure;

  DEBUG ("database opened successfully");
  g_free (dirname);
  return TRUE;

failure:
  if (mutex_locked) {
    g_mutex_unlock (&account_cleanup_mutex);
    mutex_locked = FALSE;
  }

  sqlite3_close (priv->db);
  g_free (dirname);
  priv->db = NULL;
  return FALSE;
}

gboolean
e_book_backend_tp_db_open (EBookBackendTpDb *tpdb, const gchar *account_name,
    GError **error_in)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  GError *error = NULL;
  gboolean ret;

  if (e_book_backend_tp_db_open_real (tpdb, account_name, &error))
    return TRUE;

  WARNING ("error when opening database for account %s: %s",
    account_name, error ? error->message : "unknown error");
  g_clear_error (&error);

  /* The opening of the DB could, for instance, fail because the schema
   * changed, so we just throw it away (after all it's just a cache) and
   * retry. */
  g_unlink (priv->filename);

  /* e_book_backend_tp_db_open_real wants a NULL filename but doesn't clear
   * it so we can use it for g_unlink. */
  g_free (priv->filename);
  priv->filename = NULL;

  MESSAGE ("retrying to open the database after deleting the cache");

  ret = e_book_backend_tp_db_open_real (tpdb, account_name, &error);
  if (error)
    g_propagate_error (error_in, error);

  g_free (priv->filename);
  priv->filename = NULL;

  return ret;
}

gboolean
e_book_backend_tp_db_close (EBookBackendTpDb *tpdb, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  guint i = 0;
  int res;

  for (i = 0; i < G_N_ELEMENTS (queries); i++)
  {
    if (priv->statements[i])
    {
      res = sqlite3_finalize (priv->statements[i]);

      if (res != SQLITE_OK)
      {
        WARNING ("error whilst finalising statement: %s",
            sqlite3_errmsg (priv->db));
        goto error;
      }
    }
  }

  res = sqlite3_close (priv->db);

  if (res != SQLITE_OK)
  {
    WARNING ("error whilst closing database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  priv->db = NULL;
  return FALSE;

error:
  priv->db = NULL;
  return FALSE;
}

gboolean
e_book_backend_tp_db_delete (EBookBackendTpDb *tpdb, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  gint res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->filename, FALSE, error);

  if (priv->db)
    /* FIXME: the error is ignored and FALSE is always returned */
    e_book_backend_tp_db_close (tpdb, NULL);

  res = g_unlink (priv->filename);

  g_free (priv->filename);
  priv->filename = NULL;

  if (res != 0)
    g_set_error_literal (error, G_FILE_ERROR, g_file_error_from_errno (res),
        g_strerror (res));

  return res == 0;
}

static void
e_book_backend_tp_db_real_begin (EBookBackendTpDb *tpdb,
                                 const char       *strfunc)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res = 0;

  g_return_if_fail (priv->db);

  DEBUG ("beginning transaction for %s", strfunc);

  statement = priv->statements[QUERY_BEGIN_TRANSACTION];
  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error executing statement for begin: %s",
        sqlite3_errmsg (priv->db));
    return;
  }

  sqlite3_reset (statement);
}

static void
e_book_backend_tp_db_real_commit (EBookBackendTpDb *tpdb,
                                  const char       *strfunc)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res = 0;

  g_return_if_fail (priv->db);

  DEBUG ("commiting transaction for %s", strfunc);

  statement = priv->statements[QUERY_COMMIT_TRANSACTION];
  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error executing statement for end: %s",
        sqlite3_errmsg (priv->db));
    return;
  }

  sqlite3_reset (statement);
}

static void
e_book_backend_tp_db_real_rollback (EBookBackendTpDb *tpdb,
                                    const char       *strfunc)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res = 0;

  g_return_if_fail (priv->db);

  DEBUG ("rolling back transaction for %s", strfunc);

  statement = priv->statements[QUERY_ROLLBACK_TRANSACTION];
  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error executing statement for rollback: %s",
        sqlite3_errmsg (priv->db));
    return;
  }

  sqlite3_reset (statement);
}

GArray *
e_book_backend_tp_db_fetch_contacts (EBookBackendTpDb *tpdb, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  EBookBackendTpContact *contact;
  sqlite3_stmt *statement;
  const char *contact_uid;
  const char *master_uid;
  const char *variant;
  GArray *contacts;
  int res, cmp;
  guint i;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, NULL, error);

  e_book_backend_tp_db_begin (tpdb);

  /* Contacts */
  statement = priv->statements[QUERY_FETCH_CONTACTS];
  contacts = g_array_new (TRUE, TRUE, sizeof (EBookBackendTpContact *));

  while ((res = sqlite3_step (statement)) == SQLITE_ROW)
  {
    contact = e_book_backend_tp_contact_new ();
    contact->uid = g_strdup ((gchar *)sqlite3_column_text (statement, 0));
    contact->name = g_strdup ((gchar *)sqlite3_column_text (statement, 1));
    contact->alias = g_strdup ((gchar *)sqlite3_column_text (statement, 2));
    contact->avatar_token = g_strdup ((gchar *)sqlite3_column_text (statement, 3));

    contact->flags = sqlite3_column_int (statement, 4);
    contact->pending_flags = sqlite3_column_int (statement, 5);

    contact->contact_info = g_strdup ((gchar *)sqlite3_column_text (statement, 6));

    g_array_append_val (contacts, contact);
  }

  if (res != SQLITE_DONE)
  {
    WARNING ("error whilst iterating the contacts table: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst fetching contacts from database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  sqlite3_reset (statement);

  /* Master UIDs */
  statement = priv->statements[QUERY_FETCH_MASTER_UIDS];
  contact = g_array_index (contacts, EBookBackendTpContact *, 0);
  i = 0;

  while ((res = sqlite3_step (statement)) == SQLITE_ROW)
  {
    contact_uid = (const gchar *)sqlite3_column_text (statement, 0);
    master_uid = (const gchar *)sqlite3_column_text (statement, 1);

    while (contact && (cmp = g_strcmp0 (contact_uid, contact->uid)) > 0)
    {
      if (i < contacts->len)
        contact = g_array_index (contacts, EBookBackendTpContact *, ++i);
    }

    if (contact && 0 == cmp)
      g_ptr_array_add (contact->master_uids, g_strdup (master_uid));
  }

  if (res != SQLITE_DONE)
  {
    WARNING ("error whilst iterating the master UID table: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst fetching master UIDS from database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  sqlite3_reset (statement);

  /* Non-normalized variants */
  statement = priv->statements[QUERY_FETCH_VARIANTS];
  contact = g_array_index (contacts, EBookBackendTpContact *, 0);
  i = 0;

  while ((res = sqlite3_step (statement)) == SQLITE_ROW)
  {
    contact_uid = (const gchar *)sqlite3_column_text (statement, 0);
    variant = (const gchar *)sqlite3_column_text (statement, 1);

    while (contact && (cmp = g_strcmp0 (contact_uid, contact->uid)) > 0)
    {
      if (i < contacts->len)
        contact = g_array_index (contacts, EBookBackendTpContact *, ++i);
    }

    if (contact && 0 == cmp)
      g_hash_table_insert (contact->variants, g_strdup (variant),
        GUINT_TO_POINTER (TRUE));
  }

  if (res != SQLITE_DONE)
  {
    WARNING ("error whilst iterating the non-normalized variants table: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst fetching non-normalized variants from database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  sqlite3_reset (statement);

  /* Success */
  e_book_backend_tp_db_commit (tpdb);

  return contacts;

error:
  e_book_backend_tp_db_rollback (tpdb);
  sqlite3_reset (statement);

  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    e_book_backend_tp_contact_unref (contact);
  }

  g_array_free (contacts, TRUE);

  return NULL;
}

static void
bind_add_update_contact_query (sqlite3_stmt *statement,
    EBookBackendTpContact *contact)
{
  sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":uid"),
      contact->uid, -1, SQLITE_TRANSIENT);

  if (contact->name)
  {
    sqlite3_bind_text (statement,
        sqlite3_bind_parameter_index (statement, ":name"),
        contact->name, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null (statement, sqlite3_bind_parameter_index (statement, ":name"));
  }

  if (contact->alias)
  {
    sqlite3_bind_text (statement,
        sqlite3_bind_parameter_index (statement, ":alias"),
        contact->alias, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null (statement, sqlite3_bind_parameter_index (statement, ":alias"));
  }

  if (contact->avatar_token)
  {
    sqlite3_bind_text (statement,
        sqlite3_bind_parameter_index (statement, ":avatar_token"),
        contact->avatar_token, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null (statement, sqlite3_bind_parameter_index (statement, ":avatar_token"));
  }

  sqlite3_bind_int (statement,
      sqlite3_bind_parameter_index (statement, ":flags"), contact->flags);

  sqlite3_bind_int (statement,
      sqlite3_bind_parameter_index (statement, ":pending_flags"), contact->pending_flags);

  if (contact->contact_info)
  {
    sqlite3_bind_text (statement,
        sqlite3_bind_parameter_index (statement, ":contact_info"),
        contact->contact_info, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null (statement, sqlite3_bind_parameter_index (statement, ":contact_info"));
  }
}

static gboolean
e_book_backend_tp_db_add_master_uids (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res;
  guint i;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  for (i = 0; i < contact->master_uids->len; ++i)
  {
    statement = priv->statements[QUERY_INSERT_MASTER_UID];

    sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":contact_uid"),
      contact->uid, -1, SQLITE_TRANSIENT);

    sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":master_uid"),
      contact->master_uids->pdata[i], -1, SQLITE_TRANSIENT);

    res = sqlite3_step (statement);

    if (res != SQLITE_DONE)
    {
      WARNING ("error when executing statement for inserting master uid: %s",
          sqlite3_errmsg (priv->db));
      g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
          E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
          "Error whilst adding contact to the database: %s",
          sqlite3_errmsg (priv->db));
      goto error;
    }

    sqlite3_reset (statement);
  }

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

static gboolean
e_book_backend_tp_db_add_variants (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  GHashTableIter iter;
  gpointer key;
  sqlite3_stmt *statement;
  int res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  g_hash_table_iter_init (&iter, contact->variants);
  while (g_hash_table_iter_next (&iter, &key, NULL))
  {
    statement = priv->statements[QUERY_INSERT_VARIANT];

    sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":contact_uid"),
      contact->uid, -1, SQLITE_TRANSIENT);

    sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":variant"),
      key, -1, SQLITE_TRANSIENT);

    res = sqlite3_step (statement);

    if (res != SQLITE_DONE)
    {
      WARNING ("error when executing statement for inserting variants: %s",
          sqlite3_errmsg (priv->db));
      g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
          E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
          "Error whilst adding contact to the database: %s",
          sqlite3_errmsg (priv->db));
      goto error;
    }

    sqlite3_reset (statement);
  }

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

static gboolean
e_book_backend_tp_db_real_add_contact (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  statement = priv->statements[QUERY_INSERT_CONTACT];
  bind_add_update_contact_query (statement, contact);

  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error when executing statement for adding: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst adding contact to the database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  if (!e_book_backend_tp_db_add_master_uids (tpdb, contact, error))
    goto error;

  if (!e_book_backend_tp_db_add_variants (tpdb, contact, error))
    goto error;

  sqlite3_reset (statement);

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

gboolean
e_book_backend_tp_db_add_contact (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  e_book_backend_tp_db_begin (tpdb);

  if (e_book_backend_tp_db_real_add_contact (tpdb, contact, error))
  {
    e_book_backend_tp_db_commit (tpdb);
    return TRUE;
  }

  e_book_backend_tp_db_rollback (tpdb);
  return FALSE;
}

static gboolean
e_book_backend_tp_db_delete_master_uids
    (EBookBackendTpDb *tpdb, const gchar *uid, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  statement = priv->statements[QUERY_DELETE_MASTER_UIDS];

  sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":uid"),
      uid, -1, SQLITE_TRANSIENT);

  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error when executing statement for inserting master uid: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst adding contact to the database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  sqlite3_reset (statement);

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

static gboolean
e_book_backend_tp_db_delete_variants
    (EBookBackendTpDb *tpdb, const gchar *uid, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  statement = priv->statements[QUERY_DELETE_VARIANTS];

  sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":uid"),
      uid, -1, SQLITE_TRANSIENT);

  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error when executing statement for deleting variants: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst adding contact to the database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  sqlite3_reset (statement);

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

static gboolean
e_book_backend_tp_db_real_update_contact (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  statement = priv->statements[QUERY_UPDATE_CONTACT];
  bind_add_update_contact_query (statement, contact);

  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error when executing statement for updating: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst updating contact in the database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  if (!e_book_backend_tp_db_delete_master_uids (tpdb, contact->uid, error) ||
      !e_book_backend_tp_db_add_master_uids (tpdb, contact, error))
    goto error;

  if (!e_book_backend_tp_db_delete_variants (tpdb, contact->uid, error) ||
      !e_book_backend_tp_db_add_variants (tpdb, contact, error))
    goto error;

  sqlite3_reset (statement);

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

gboolean
e_book_backend_tp_db_update_contact (EBookBackendTpDb *tpdb,
    EBookBackendTpContact *contact, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  e_book_backend_tp_db_begin (tpdb);

  if (e_book_backend_tp_db_real_update_contact (tpdb, contact, error))
  {
    e_book_backend_tp_db_commit (tpdb);
    return TRUE;
  }

  e_book_backend_tp_db_rollback (tpdb);
  return FALSE;
}

static gboolean
e_book_backend_tp_db_real_delete_contact (EBookBackendTpDb *tpdb,
    const gchar *uid, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  sqlite3_stmt *statement;
  int res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  statement = priv->statements[QUERY_DELETE_CONTACT];

  sqlite3_bind_text (statement,
      sqlite3_bind_parameter_index (statement, ":uid"),
      uid, -1, SQLITE_TRANSIENT);

  res = sqlite3_step (statement);

  if (res != SQLITE_DONE)
  {
    WARNING ("error executing statement for deleting: %s",
        sqlite3_errmsg (priv->db));
    g_set_error (error, E_BOOK_BACKEND_TP_DB_ERROR,
        E_BOOK_BACKEND_TP_DB_ERROR_FAILED,
        "Error whilst deleting contact in the database: %s",
        sqlite3_errmsg (priv->db));
    goto error;
  }

  if (!e_book_backend_tp_db_delete_master_uids (tpdb, uid, error))
    goto error;

  if (!e_book_backend_tp_db_delete_variants (tpdb, uid, error))
    goto error;

  sqlite3_reset (statement);

  return TRUE;

error:
  sqlite3_reset (statement);

  return FALSE;
}

gboolean
e_book_backend_tp_db_delete_contact (EBookBackendTpDb *tpdb, const gchar *uid,
    GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  e_book_backend_tp_db_begin (tpdb);

  if (e_book_backend_tp_db_real_delete_contact (tpdb, uid, error))
  {
    e_book_backend_tp_db_commit (tpdb);
    return TRUE;
  }

  e_book_backend_tp_db_rollback (tpdb);
  return FALSE;
}


gboolean
e_book_backend_tp_db_add_contacts (EBookBackendTpDb *tpdb,
    GArray *contacts, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  guint i = 0;
  EBookBackendTpContact *contact = NULL;
  gboolean res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  e_book_backend_tp_db_begin (tpdb);

  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    res = e_book_backend_tp_db_real_add_contact (tpdb, contact, error);
    if (!res)
    {
      e_book_backend_tp_db_rollback (tpdb);
      return res;
    }
  }
  e_book_backend_tp_db_commit (tpdb);

  return TRUE;
}

gboolean
e_book_backend_tp_db_update_contacts (EBookBackendTpDb *tpdb,
    GArray *contacts, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  guint i = 0;
  EBookBackendTpContact *contact = NULL;
  gboolean res;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  e_book_backend_tp_db_begin (tpdb);
  for (i = 0; i < contacts->len; i++)
  {
    contact = g_array_index (contacts, EBookBackendTpContact *, i);
    res = e_book_backend_tp_db_real_update_contact (tpdb, contact, error);
    if (!res)
    {
      e_book_backend_tp_db_rollback (tpdb);
      return res;
    }
  }
  e_book_backend_tp_db_commit (tpdb);

  return TRUE;
}

gboolean
e_book_backend_tp_db_remove_contacts (EBookBackendTpDb *tpdb,
    GArray *uids, GError **error)
{
  EBookBackendTpDbPrivate *priv = GET_PRIVATE (tpdb);
  const gchar *uid;
  gboolean res;
  guint i = 0;

  e_book_backend_tp_return_val_with_error_if_fail (priv->db, FALSE, error);

  e_book_backend_tp_db_begin (tpdb);

  for (i = 0; i < uids->len; i++)
  {
    uid = g_array_index (uids, gchar *, i);
    res = e_book_backend_tp_db_real_delete_contact (tpdb, uid, error);

    if (!res)
    {
      e_book_backend_tp_db_rollback (tpdb);
      return res;
    }
  }

  e_book_backend_tp_db_commit (tpdb);

  return TRUE;
}

/* Copied from the accounts UI.
 * Writing to the DB is made in an async way when a timeout fires, so we
 * don't have a way to properly report errors.
 * Functions that could modify the DB can call this function, being so
 * able to properly report errors.
 * TODO: if we open source this component remove this function or add an
 * #ifdef for it, as it's not appropriate in a general case.
 */

#define DISK_FULL_THRESHOLD_PERCENT   3
#define DISK_FULL_THRESHOLD_ABSOLUTE (8 << 20) /* 8 MB */

gboolean
e_book_backend_tp_db_check_available_disk_space (void)
{
  gint err = 0;
  gboolean result = FALSE;
  struct statfs buf;

  err = statfs (get_db_directory (), &buf);

  if (err == 0)
  {
    gulong avail = (buf.f_bavail * buf.f_bsize);
    gulong total = (buf.f_blocks * buf.f_bsize);

    gulong limit = MIN (DISK_FULL_THRESHOLD_ABSOLUTE,
                        total * DISK_FULL_THRESHOLD_PERCENT / 100);

    result = (avail > limit);
  }
  else
  {
    g_warning ("Error while checking file system space: %s",
        strerror (errno));
  }

  return result;
}
