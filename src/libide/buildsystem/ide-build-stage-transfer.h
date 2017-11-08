/* ide-build-stage-transfer.h
 *
 * Copyright © 2016 Christian Hergert <chergert@redhat.com>
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
 */

#pragma once

#include "ide-version-macros.h"

#include "buildsystem/ide-build-stage.h"

G_BEGIN_DECLS

#define IDE_TYPE_BUILD_STAGE_TRANSFER (ide_build_stage_transfer_get_type())

G_DECLARE_FINAL_TYPE (IdeBuildStageTransfer, ide_build_stage_transfer, IDE, BUILD_STAGE_TRANSFER, IdeBuildStage)

IDE_AVAILABLE_IN_ALL
IdeBuildStageTransfer *ide_build_stage_transfer_new (IdeContext  *context,
                                                     IdeTransfer *transfer);

G_END_DECLS
