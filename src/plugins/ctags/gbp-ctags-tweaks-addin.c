/* gbp-ctags-tweaks-addin.c
 *
 * Copyright 2022 Christian Hergert <chergert@redhat.com>
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

#define G_LOG_DOMAIN "gbp-ctags-tweaks-addin"

#include "config.h"

#include "gbp-ctags-tweaks-addin.h"

struct _GbpCtagsTweaksAddin
{
  IdeTweaksAddin parent_instance;
};

G_DEFINE_FINAL_TYPE (GbpCtagsTweaksAddin, gbp_ctags_tweaks_addin, IDE_TYPE_TWEAKS_ADDIN)

static void
gbp_ctags_tweaks_addin_class_init (GbpCtagsTweaksAddinClass *klass)
{
}

static void
gbp_ctags_tweaks_addin_init (GbpCtagsTweaksAddin *self)
{
  ide_tweaks_addin_set_resource_paths (IDE_TWEAKS_ADDIN (self),
                                       IDE_STRV_INIT ("/plugins/ctags/tweaks.ui"));
}