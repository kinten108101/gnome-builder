/* gbp-git-buffer-change-monitor.c
 *
 * Copyright 2015-2019 Christian Hergert <christian@hergert.me>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Some code from this file is loosely based around the git-diff
 * plugin from Atom. Namely, API usage for iterating through hunks
 * containing changes. It's license is provided below.
 */

/*
 * Copyright (c) 2014 GitHub Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define G_LOG_DOMAIN "gbp-git-buffer-change-monitor"

#include <dazzle.h>
#include <glib/gi18n.h>
#include <libgit2-glib/ggit.h>
#include <stdlib.h>

#include "gbp-git-buffer-change-monitor.h"
#include "gbp-git-vcs.h"

#include "line-cache.h"

#define DELAY_CHANGED_SEC 1

/**
 * SECTION:gbp-git-buffer-change-monitor
 *
 * This module provides line change monitoring when used in conjunction with an
 * GbpGitVcs.  The changes are generated by comparing the buffer contents to
 * the version found inside of the git repository.
 *
 * To enable us to avoid blocking the main loop, the actual diff is performed
 * in a background thread. To avoid threading issues with the rest of LibGBP,
 * this module creates a copy of the loaded repository. A single thread will be
 * dispatched for the context and all reload tasks will be performed from that
 * thread.
 *
 * Upon completion of the diff, the results will be passed back to the primary
 * thread and the state updated for use by line change renderer in the source
 * view.
 *
 * Since: 3.32
 */

struct _GbpGitBufferChangeMonitor
{
  IdeBufferChangeMonitor  parent_instance;

  DzlSignalGroup         *signal_group;

  GgitRepository         *repository;
  GArray                 *lines;

  GgitBlob               *cached_blob;
  LineCache              *cache;

  GQueue                  wait_tasks;

  guint                   changed_timeout;

  guint                   state_dirty : 1;
  guint                   in_calculation : 1;
  guint                   delete_range_requires_recalculation : 1;
  guint                   is_child_of_workdir : 1;
  guint                   in_failed_state : 1;
};

typedef struct
{
  GgitRepository *repository;
  LineCache      *cache;
  GFile          *file;
  GBytes         *content;
  GgitBlob       *blob;
  IdeObject      *lock_object;
  guint           is_child_of_workdir : 1;
} DiffTask;

typedef struct
{
  gint old_start;
  gint old_lines;
  gint new_start;
  gint new_lines;
} Range;

G_DEFINE_TYPE (GbpGitBufferChangeMonitor, gbp_git_buffer_change_monitor, IDE_TYPE_BUFFER_CHANGE_MONITOR)

DZL_DEFINE_COUNTER (instances, "GbpGitBufferChangeMonitor", "Instances", "The number of git buffer change monitor instances.");

enum {
  PROP_0,
  PROP_REPOSITORY,
  LAST_PROP
};

static GParamSpec  *properties [LAST_PROP];
static GAsyncQueue *work_queue;
static GThread     *work_thread;

static void
diff_task_free (gpointer data)
{
  DiffTask *diff = data;

  if (diff)
    {
      g_clear_object (&diff->file);
      g_clear_object (&diff->blob);
      g_clear_object (&diff->repository);
      g_clear_object (&diff->lock_object);
      g_clear_pointer (&diff->cache, line_cache_free);
      g_clear_pointer (&diff->content, g_bytes_unref);
      g_slice_free (DiffTask, diff);
    }
}

static void
complete_wait_tasks (GbpGitBufferChangeMonitor *self,
                     GParamSpec                *pspec,
                     IdeTask                   *calculate_task)
{
  gpointer taskptr;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (IDE_IS_TASK (calculate_task));

  while ((taskptr = g_queue_pop_head (&self->wait_tasks)))
    {
      g_autoptr(IdeTask) task = taskptr;

      ide_task_return_boolean (task, TRUE);
    }
}

static LineCache *
gbp_git_buffer_change_monitor_calculate_finish (GbpGitBufferChangeMonitor  *self,
                                                GAsyncResult               *result,
                                                GError                    **error)
{
  IdeTask *task = (IdeTask *)result;
  DiffTask *diff;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (IDE_IS_TASK (result));

  if (ide_object_set_error_if_destroyed (IDE_OBJECT (self), error))
    return NULL;

  diff = ide_task_get_task_data (task);

  if (diff != NULL)
    {
      g_assert (GGIT_IS_REPOSITORY (diff->repository));
      g_assert (G_IS_FILE (diff->file));
      g_assert (diff->content != NULL);
      g_assert (GBP_IS_GIT_VCS (diff->lock_object));

      /* Keep the blob around for future use */
      if (diff->blob != self->cached_blob)
        g_set_object (&self->cached_blob, diff->blob);

      /* If the file is a child of the working directory, we need to know */
      self->is_child_of_workdir = diff->is_child_of_workdir;
    }

  return ide_task_propagate_pointer (task, error);
}

static void
gbp_git_buffer_change_monitor_calculate_async (GbpGitBufferChangeMonitor *self,
                                               GCancellable              *cancellable,
                                               GAsyncReadyCallback        callback,
                                               gpointer                   user_data)
{
  g_autoptr(IdeTask) task = NULL;
  g_autoptr(IdeContext) context = NULL;
  GbpGitVcs *vcs;
  IdeBuffer *buffer;
  DiffTask *diff;
  GFile *file;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_assert (self->repository != NULL);

  self->state_dirty = FALSE;

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_git_buffer_change_monitor_calculate_async);

  g_signal_connect_object (task,
                           "notify::completed",
                           G_CALLBACK (complete_wait_tasks),
                           self,
                           G_CONNECT_SWAPPED);

  buffer = ide_buffer_change_monitor_get_buffer (IDE_BUFFER_CHANGE_MONITOR (self));
  g_assert (IDE_IS_BUFFER (buffer));

  file = ide_buffer_get_file (buffer);
  g_assert (G_IS_FILE (file));

  context = ide_object_ref_context (IDE_OBJECT (self));
  vcs = ide_context_peek_child_typed (context, GBP_TYPE_GIT_VCS);

  if (!GBP_IS_GIT_VCS (vcs))
    {
      ide_task_return_new_error (task,
                                 G_IO_ERROR,
                                 G_IO_ERROR_CANCELLED,
                                 "Cannot provide changes, not connected to GbpGitVcs.");
      return;
    }

  diff = g_slice_new0 (DiffTask);
  diff->file = g_object_ref (file);
  diff->repository = g_object_ref (self->repository);
  diff->cache = line_cache_new ();
  diff->content = ide_buffer_dup_content (buffer);
  diff->blob = self->cached_blob ? g_object_ref (self->cached_blob) : NULL;
  diff->lock_object = g_object_ref (IDE_OBJECT (vcs));

  ide_task_set_task_data (task, diff, diff_task_free);

  self->in_calculation = TRUE;

  g_async_queue_push (work_queue, g_steal_pointer (&task));
}

static void
foreach_cb (gpointer data,
            gpointer user_data)
{
  const LineEntry *entry = data;
  struct {
    IdeBufferChangeMonitorForeachFunc func;
    gpointer user_data;
  } *state = user_data;
  IdeBufferLineChange change = 0;

  if (entry->mark & LINE_MARK_ADDED)
    change |= IDE_BUFFER_LINE_CHANGE_ADDED;

  if (entry->mark & LINE_MARK_REMOVED)
    change |= IDE_BUFFER_LINE_CHANGE_DELETED;

  if (entry->mark & LINE_MARK_PREVIOUS_REMOVED)
    change |= IDE_BUFFER_LINE_CHANGE_PREVIOUS_DELETED;

  if (entry->mark & LINE_MARK_CHANGED)
    change |= IDE_BUFFER_LINE_CHANGE_CHANGED;

  state->func (entry->line, change, state->user_data);
}

static void
gbp_git_buffer_change_monitor_foreach_change (IdeBufferChangeMonitor            *monitor,
                                              guint                              begin_line,
                                              guint                              end_line,
                                              IdeBufferChangeMonitorForeachFunc  callback,
                                              gpointer                           user_data)
{
  GbpGitBufferChangeMonitor *self = (GbpGitBufferChangeMonitor *)monitor;
  struct {
    IdeBufferChangeMonitorForeachFunc func;
    gpointer user_data;
  } state = { callback, user_data };

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (callback != NULL);

  if (end_line == G_MAXUINT)
    end_line--;

  if (self->cache == NULL)
    {
      /* If within working directory, synthesize line addition. */
      if (self->is_child_of_workdir)
        {
          for (guint i = begin_line; i < end_line; i++)
            callback (i, IDE_BUFFER_LINE_CHANGE_ADDED, user_data);
        }

      return;
    }

  line_cache_foreach_in_range (self->cache, begin_line, end_line, foreach_cb, &state);
}

static IdeBufferLineChange
gbp_git_buffer_change_monitor_get_change (IdeBufferChangeMonitor *monitor,
                                          guint                   line)
{
  GbpGitBufferChangeMonitor *self = (GbpGitBufferChangeMonitor *)monitor;
  guint mark;

  /* Don't imply changes we don't know are real, in the case that
   * we failed to communicate with git properly about the blob diff.
   */
  if (self->in_failed_state)
    return IDE_BUFFER_LINE_CHANGE_NONE;

  if (self->cache == NULL)
    {
      /* If within working directory, synthesize line addition. */
      if (self->is_child_of_workdir)
        return IDE_BUFFER_LINE_CHANGE_ADDED;
      return IDE_BUFFER_LINE_CHANGE_NONE;
    }

  mark = line_cache_get_mark (self->cache, line + 1);

  if (mark & LINE_MARK_ADDED)
    return IDE_BUFFER_LINE_CHANGE_ADDED;
  else if (mark & LINE_MARK_REMOVED)
    return IDE_BUFFER_LINE_CHANGE_DELETED;
  else if (mark & LINE_MARK_CHANGED)
    return IDE_BUFFER_LINE_CHANGE_CHANGED;
  else
    return IDE_BUFFER_LINE_CHANGE_NONE;
}

void
gbp_git_buffer_change_monitor_set_repository (GbpGitBufferChangeMonitor *self,
                                              GgitRepository            *repository)
{
  gboolean do_reload;

  g_return_if_fail (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_return_if_fail (GGIT_IS_REPOSITORY (repository));

  do_reload = self->repository != NULL && repository != NULL;

  if (g_set_object (&self->repository, repository))
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_REPOSITORY]);

      if (do_reload)
        ide_buffer_change_monitor_reload (IDE_BUFFER_CHANGE_MONITOR (self));
    }
}

static void
gbp_git_buffer_change_monitor__calculate_cb (GObject      *object,
                                             GAsyncResult *result,
                                             gpointer      user_data_unused)
{
  GbpGitBufferChangeMonitor *self = (GbpGitBufferChangeMonitor *)object;
  LineCache *cache;
  g_autoptr(GError) error = NULL;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (user_data_unused == NULL);

  self->in_calculation = FALSE;

  cache = gbp_git_buffer_change_monitor_calculate_finish (self, result, &error);

  if (cache == NULL)
    {
      if (!self->in_failed_state && !g_error_matches (error, GGIT_ERROR, GGIT_ERROR_NOTFOUND))
        {
          ide_object_warning (self,
                              /* translators: %s is replaced with the error string from git */
                              _("There was a failure while calculating line changes from git. The exact error was: %s"),
                              error->message);
          self->in_failed_state = TRUE;
        }
    }
  else
    {
      g_clear_pointer (&self->cache, line_cache_free);
      self->cache = g_steal_pointer (&cache);
      self->in_failed_state = FALSE;
    }

  ide_buffer_change_monitor_emit_changed (IDE_BUFFER_CHANGE_MONITOR (self));

  /* Recalculate if the buffer has changed since last request. */
  if (self->state_dirty)
    gbp_git_buffer_change_monitor_calculate_async (self,
                                                   NULL,
                                                   gbp_git_buffer_change_monitor__calculate_cb,
                                                   NULL);
}

static void
gbp_git_buffer_change_monitor_recalculate (GbpGitBufferChangeMonitor *self)
{
  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));

  self->state_dirty = TRUE;

  if (!self->in_calculation)
    gbp_git_buffer_change_monitor_calculate_async (self,
                                                   NULL,
                                                   gbp_git_buffer_change_monitor__calculate_cb,
                                                   NULL);
}

static void
gbp_git_buffer_change_monitor__buffer_delete_range_after_cb (GbpGitBufferChangeMonitor *self,
                                                             GtkTextIter               *begin,
                                                             GtkTextIter               *end,
                                                             IdeBuffer                 *buffer)
{
  IDE_ENTRY;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (begin);
  g_assert (end);
  g_assert (IDE_IS_BUFFER (buffer));

  if (self->delete_range_requires_recalculation)
    {
      self->delete_range_requires_recalculation = FALSE;
      gbp_git_buffer_change_monitor_recalculate (self);
    }

  IDE_EXIT;
}

static void
gbp_git_buffer_change_monitor__buffer_delete_range_cb (GbpGitBufferChangeMonitor *self,
                                                       GtkTextIter               *begin,
                                                       GtkTextIter               *end,
                                                       IdeBuffer                 *buffer)
{
  IdeBufferLineChange change;

  IDE_ENTRY;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (begin != NULL);
  g_assert (end != NULL);
  g_assert (IDE_IS_BUFFER (buffer));

  /*
   * We need to recalculate the diff when text is deleted if:
   *
   * 1) The range includes a newline.
   * 2) The current line change is set to NONE.
   *
   * Technically we need to do it on every change to be more correct, but that wastes a lot of
   * power. So instead, we'll be a bit lazy about it here and pick up the other changes on a much
   * more conservative timeout, generated by gbp_git_buffer_change_monitor__buffer_changed_cb().
   */

  if (gtk_text_iter_get_line (begin) != gtk_text_iter_get_line (end))
    IDE_GOTO (recalculate);

  change = gbp_git_buffer_change_monitor_get_change (IDE_BUFFER_CHANGE_MONITOR (self),
                                                     gtk_text_iter_get_line (begin));
  if (change == IDE_BUFFER_LINE_CHANGE_NONE)
    IDE_GOTO (recalculate);

  IDE_EXIT;

recalculate:
  /*
   * We need to wait for the delete to occur, so mark it as necessary and let
   * gbp_git_buffer_change_monitor__buffer_delete_range_after_cb perform the operation.
   */
  self->delete_range_requires_recalculation = TRUE;

  IDE_EXIT;
}

static void
gbp_git_buffer_change_monitor__buffer_insert_text_after_cb (GbpGitBufferChangeMonitor *self,
                                                            GtkTextIter               *location,
                                                            gchar                     *text,
                                                            gint                       len,
                                                            IdeBuffer                 *buffer)
{
  IdeBufferLineChange change;

  IDE_ENTRY;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (location);
  g_assert (text);
  g_assert (IDE_IS_BUFFER (buffer));

  /*
   * We need to recalculate the diff when text is inserted if:
   *
   * 1) A newline is included in the text.
   * 2) The line currently has flags of NONE.
   *
   * Technically we need to do it on every change to be more correct, but that wastes a lot of
   * power. So instead, we'll be a bit lazy about it here and pick up the other changes on a much
   * more conservative timeout, generated by gbp_git_buffer_change_monitor__buffer_changed_cb().
   */

  if (NULL != memmem (text, len, "\n", 1))
    IDE_GOTO (recalculate);

  change = gbp_git_buffer_change_monitor_get_change (IDE_BUFFER_CHANGE_MONITOR (self),
                                                     gtk_text_iter_get_line (location));
  if (change == IDE_BUFFER_LINE_CHANGE_NONE)
    IDE_GOTO (recalculate);

  IDE_EXIT;

recalculate:
  gbp_git_buffer_change_monitor_recalculate (self);

  IDE_EXIT;
}

static gboolean
gbp_git_buffer_change_monitor__changed_timeout_cb (gpointer user_data)
{
  GbpGitBufferChangeMonitor *self = user_data;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));

  self->changed_timeout = 0;
  gbp_git_buffer_change_monitor_recalculate (self);

  return G_SOURCE_REMOVE;
}

static void
gbp_git_buffer_change_monitor__buffer_changed_after_cb (GbpGitBufferChangeMonitor *self,
                                                        IdeBuffer                 *buffer)
{
  g_assert (IDE_IS_BUFFER_CHANGE_MONITOR (self));
  g_assert (IDE_IS_BUFFER (buffer));

  self->state_dirty = TRUE;

  if (self->in_calculation)
    return;

  dzl_clear_source (&self->changed_timeout);
  self->changed_timeout = g_timeout_add_seconds (DELAY_CHANGED_SEC,
                                                 gbp_git_buffer_change_monitor__changed_timeout_cb,
                                                 self);
}

static void
gbp_git_buffer_change_monitor_reload (IdeBufferChangeMonitor *monitor)
{
  GbpGitBufferChangeMonitor *self = (GbpGitBufferChangeMonitor *)monitor;

  IDE_ENTRY;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));

  g_clear_object (&self->cached_blob);
  gbp_git_buffer_change_monitor_recalculate (self);

  IDE_EXIT;
}

static void
gbp_git_buffer_change_monitor_load (IdeBufferChangeMonitor *monitor,
                                    IdeBuffer              *buffer)
{
  GbpGitBufferChangeMonitor *self = (GbpGitBufferChangeMonitor *)monitor;

  IDE_ENTRY;

  g_return_if_fail (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_return_if_fail (IDE_IS_BUFFER (buffer));

  dzl_signal_group_set_target (self->signal_group, buffer);

  IDE_EXIT;
}

static gint
diff_hunk_cb (GgitDiffDelta *delta,
              GgitDiffHunk  *hunk,
              gpointer       user_data)
{
  GArray *ranges = user_data;
  Range range;

  g_assert (delta != NULL);
  g_assert (hunk != NULL);
  g_assert (ranges != NULL);

  range.old_start = ggit_diff_hunk_get_old_start (hunk);
  range.old_lines = ggit_diff_hunk_get_old_lines (hunk);
  range.new_start = ggit_diff_hunk_get_new_start (hunk);
  range.new_lines = ggit_diff_hunk_get_new_lines (hunk);

  g_array_append_val (ranges, range);

  return 0;
}

static gboolean
gbp_git_buffer_change_monitor_calculate_threaded (GbpGitBufferChangeMonitor  *self,
                                                  DiffTask                   *diff,
                                                  GError                    **error)
{
  g_autofree gchar *relative_path = NULL;
  g_autoptr(GgitDiffOptions) options = NULL;
  g_autoptr(GFile) workdir = NULL;
  g_autoptr(GArray) ranges = NULL;
  LineCache *cache;
  const guint8 *data;
  gsize data_len = 0;

  g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (diff != NULL);
  g_assert (G_IS_FILE (diff->file));
  g_assert (diff->cache != NULL);
  g_assert (GGIT_IS_REPOSITORY (diff->repository));
  g_assert (diff->content != NULL);
  g_assert (!diff->blob || GGIT_IS_BLOB (diff->blob));
  g_assert (error != NULL);
  g_assert (*error == NULL);

  workdir = ggit_repository_get_workdir (diff->repository);

  if (!workdir)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_FILENAME,
                   _("Repository does not have a working directory."));
      return FALSE;
    }

  relative_path = g_file_get_relative_path (workdir, diff->file);

  if (!relative_path)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_FILENAME,
                   _("File is not under control of git working directory."));
      return FALSE;
    }

  diff->is_child_of_workdir = TRUE;

  /*
   * Find the blob if necessary. This will be cached by the main thread for
   * us on the way out of the async operation.
   */
  if (diff->blob == NULL)
    {
      GgitOId *entry_oid = NULL;
      GgitOId *oid = NULL;
      GgitObject *blob = NULL;
      GgitObject *commit = NULL;
      GgitRef *head = NULL;
      GgitTree *tree = NULL;
      GgitTreeEntry *entry = NULL;

      head = ggit_repository_get_head (diff->repository, error);
      if (!head)
        goto cleanup;

      oid = ggit_ref_get_target (head);
      if (!oid)
        goto cleanup;

      commit = ggit_repository_lookup (diff->repository, oid, GGIT_TYPE_COMMIT, error);
      if (!commit)
        goto cleanup;

      tree = ggit_commit_get_tree (GGIT_COMMIT (commit));
      if (!tree)
        goto cleanup;

      entry = ggit_tree_get_by_path (tree, relative_path, error);
      if (!entry)
        goto cleanup;

      entry_oid = ggit_tree_entry_get_id (entry);
      if (!entry_oid)
        goto cleanup;

      blob = ggit_repository_lookup (diff->repository, entry_oid, GGIT_TYPE_BLOB, error);
      if (!blob)
        goto cleanup;

      diff->blob = g_object_ref (GGIT_BLOB (blob));

    cleanup:
      g_clear_object (&blob);
      g_clear_pointer (&entry_oid, ggit_oid_free);
      g_clear_pointer (&entry, ggit_tree_entry_unref);
      g_clear_object (&tree);
      g_clear_object (&commit);
      g_clear_pointer (&oid, ggit_oid_free);
      g_clear_object (&head);
    }

  if (diff->blob == NULL)
    {
      if (*error == NULL)
        g_set_error (error,
                     G_IO_ERROR,
                     G_IO_ERROR_NOT_FOUND,
                     _("The requested file does not exist within the git index."));
      return FALSE;
    }

  data = g_bytes_get_data (diff->content, &data_len);

  ranges = g_array_new (FALSE, FALSE, sizeof (Range));
  options = ggit_diff_options_new ();

  ggit_diff_options_set_n_context_lines (options, 0);

  ggit_diff_blob_to_buffer (diff->blob,
                            relative_path,
                            data,
                            data_len,
                            relative_path,
                            options,
                            NULL,         /* File Callback */
                            NULL,         /* Binary Callback */
                            diff_hunk_cb, /* Hunk Callback */
                            NULL,
                            ranges,
                            error);

  cache = diff->cache;

  for (guint i = 0; i < ranges->len; i++)
    {
      const Range *range = &g_array_index (ranges, Range, i);
      gint start_line = range->new_start - 1;
      gint end_line = range->new_start + range->new_lines - 1;

      if (range->old_lines == 0 && range->new_lines > 0)
        {
          line_cache_mark_range (cache, start_line, end_line, LINE_MARK_ADDED);
        }
      else if (range->new_lines == 0 && range->old_lines > 0)
        {
          if (start_line < 0)
            line_cache_mark_range (cache, 0, 0, LINE_MARK_PREVIOUS_REMOVED);
          else
            line_cache_mark_range (cache, start_line + 1, start_line + 1, LINE_MARK_REMOVED);
        }
      else
        {
          line_cache_mark_range (cache, start_line, end_line, LINE_MARK_CHANGED);
        }
    }

  return *error == NULL;
}

static gpointer
gbp_git_buffer_change_monitor_worker (gpointer data)
{
  GAsyncQueue *queue = data;
  gpointer taskptr;

  g_assert (queue != NULL);

  /*
   * This is a single thread worker that dispatches the particular
   * change to the given change monitor. We require a single thread
   * so that we can mantain the invariant that only a single thread
   * can access a GgitRepository at a time (and change monitors all
   * share the same GgitRepository amongst themselves).
   */

  while ((taskptr = g_async_queue_pop (queue)))
    {
      GbpGitBufferChangeMonitor *self;
      g_autoptr(GError) error = NULL;
      g_autoptr(IdeTask) task = taskptr;
      DiffTask *diff;
      gboolean ret;

      self = ide_task_get_source_object (task);
      g_assert (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));

      diff = ide_task_get_task_data (task);
      g_assert (diff != NULL);

      /* Acquire the lock for the parent to ensure we have access to repository */
      ide_object_lock (diff->lock_object);
      ret = gbp_git_buffer_change_monitor_calculate_threaded (self, diff, &error);
      ide_object_unlock (diff->lock_object);

      if (!ret)
        ide_task_return_error (task, g_steal_pointer (&error));
      else
        ide_task_return_pointer (task,
                                 g_steal_pointer (&diff->cache),
                                 (GDestroyNotify)line_cache_free);
    }

  return NULL;
}

static void
gbp_git_buffer_change_monitor_destroy (IdeObject *object)
{
  GbpGitBufferChangeMonitor *self = (GbpGitBufferChangeMonitor *)object;

  dzl_clear_source (&self->changed_timeout);

  if (self->signal_group)
    {
      dzl_signal_group_set_target (self->signal_group, NULL);
      g_clear_object (&self->signal_group);
    }

  g_clear_object (&self->cached_blob);
  g_clear_object (&self->repository);
  g_clear_pointer (&self->cache, line_cache_free);

  IDE_OBJECT_CLASS (gbp_git_buffer_change_monitor_parent_class)->destroy (object);
}

static void
gbp_git_buffer_change_monitor_finalize (GObject *object)
{
  G_OBJECT_CLASS (gbp_git_buffer_change_monitor_parent_class)->finalize (object);

  DZL_COUNTER_DEC (instances);
}

static void
gbp_git_buffer_change_monitor_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  GbpGitBufferChangeMonitor *self = GBP_GIT_BUFFER_CHANGE_MONITOR (object);

  switch (prop_id)
    {
    case PROP_REPOSITORY:
      gbp_git_buffer_change_monitor_set_repository (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gbp_git_buffer_change_monitor_class_init (GbpGitBufferChangeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  IdeObjectClass *i_object_class = IDE_OBJECT_CLASS (klass);
  IdeBufferChangeMonitorClass *parent_class = IDE_BUFFER_CHANGE_MONITOR_CLASS (klass);

  object_class->finalize = gbp_git_buffer_change_monitor_finalize;
  object_class->set_property = gbp_git_buffer_change_monitor_set_property;

  i_object_class->destroy = gbp_git_buffer_change_monitor_destroy;

  parent_class->load = gbp_git_buffer_change_monitor_load;
  parent_class->get_change = gbp_git_buffer_change_monitor_get_change;
  parent_class->reload = gbp_git_buffer_change_monitor_reload;
  parent_class->foreach_change = gbp_git_buffer_change_monitor_foreach_change;

  properties [PROP_REPOSITORY] =
    g_param_spec_object ("repository",
                         "Repository",
                         "The repository to use for calculating diffs.",
                         GGIT_TYPE_REPOSITORY,
                         (G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  /* Note: We use a single worker thread so that we can maintain the
   *       invariant that only a single thread is touching the GgitRepository
   *       at a time. (Also, you can only type in one editor at a time, so
   *       on worker thread for interactive blob changes is fine.
   */
  work_queue = g_async_queue_new ();
  work_thread = g_thread_new ("GbpGitBufferChangeMonitorWorker",
                              gbp_git_buffer_change_monitor_worker,
                              work_queue);
}

static void
gbp_git_buffer_change_monitor_init (GbpGitBufferChangeMonitor *self)
{
  DZL_COUNTER_INC (instances);

  self->signal_group = dzl_signal_group_new (IDE_TYPE_BUFFER);
  dzl_signal_group_connect_object (self->signal_group,
                                   "insert-text",
                                   G_CALLBACK (gbp_git_buffer_change_monitor__buffer_insert_text_after_cb),
                                   self,
                                   G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  dzl_signal_group_connect_object (self->signal_group,
                                   "delete-range",
                                   G_CALLBACK (gbp_git_buffer_change_monitor__buffer_delete_range_cb),
                                   self,
                                   G_CONNECT_SWAPPED);
  dzl_signal_group_connect_object (self->signal_group,
                                   "delete-range",
                                   G_CALLBACK (gbp_git_buffer_change_monitor__buffer_delete_range_after_cb),
                                   self,
                                   G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  dzl_signal_group_connect_object (self->signal_group,
                                   "changed",
                                   G_CALLBACK (gbp_git_buffer_change_monitor__buffer_changed_after_cb),
                                   self,
                                   G_CONNECT_SWAPPED | G_CONNECT_AFTER);
}

void
gbp_git_buffer_change_monitor_wait_async (GbpGitBufferChangeMonitor *self,
                                          GCancellable              *cancellable,
                                          GAsyncReadyCallback        callback,
                                          gpointer                   user_data)
{
  g_autoptr(IdeTask) task = NULL;

  g_return_if_fail (IDE_IS_MAIN_THREAD ());
  g_return_if_fail (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_git_buffer_change_monitor_wait_async);

  if (!self->state_dirty && !self->in_calculation)
    {
      ide_task_return_boolean (task, TRUE);
      return;
    }

  g_queue_push_tail (&self->wait_tasks, g_steal_pointer (&task));

  if (!self->in_calculation)
    gbp_git_buffer_change_monitor_recalculate (self);
}

gboolean
gbp_git_buffer_change_monitor_wait_finish (GbpGitBufferChangeMonitor  *self,
                                           GAsyncResult               *result,
                                           GError                    **error)
{
  g_return_val_if_fail (IDE_IS_MAIN_THREAD (), FALSE);
  g_return_val_if_fail (GBP_IS_GIT_BUFFER_CHANGE_MONITOR (self), FALSE);
  g_return_val_if_fail (IDE_IS_TASK (result), FALSE);

  return ide_task_propagate_boolean (IDE_TASK (result), error);
}
