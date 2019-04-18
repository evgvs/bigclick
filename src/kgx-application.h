/* kgx-application.h
 *
 * Copyright 2019 Zander Brown
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

#include <gtk/gtk.h>

#include "kgx-process.h"
#include "kgx-window.h"

G_BEGIN_DECLS

#define KGX_TYPE_APPLICATION (kgx_application_get_type())

G_DECLARE_FINAL_TYPE (KgxApplication, kgx_application, KGX, APPLICATION, GtkApplication)

void kgx_application_add_watch (KgxApplication *self,
                                KgxProcess     *process,
                                KgxWindow      *window);

G_END_DECLS

