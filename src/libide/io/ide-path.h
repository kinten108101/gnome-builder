/* ide-path.h
 *
 * Copyright 2018-2019 Christian Hergert <chergert@redhat.com>
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

#pragma once

#if !defined (IDE_IO_INSIDE) && !defined (IDE_IO_COMPILATION)
# error "Only <libide-io.h> can be included directly."
#endif

#include <libide-core.h>

G_BEGIN_DECLS

IDE_AVAILABLE_IN_ALL
gboolean  ide_path_is_c_like            (const gchar *path);
IDE_AVAILABLE_IN_ALL
gboolean  ide_path_is_cpp_like          (const gchar *path);
IDE_AVAILABLE_IN_ALL
gchar    *ide_find_program_in_host_path (const gchar *program);

G_END_DECLS
