/** \file   soundsuspendtimewidget.c
 * \brief   Sound suspend time widget
 *
 * \author  Bas Wassink <b.wassink@ziggo.nl>
 */

/*
 * $VICERES SoundSuspendTime    all
 */

/*
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 */


#include "vice.h"

#include <gtk/gtk.h>

#include "lib.h"
#include "ui.h"
#include "resources.h"
#include "vsync.h"
#include "sound.h"
#include "basewidgets.h"
#include "widgethelpers.h"
#include "debug_gtk3.h"

#include "soundsuspendtimewidget.h"


/** \brief  Minimum value for the buffer size spin button (sec)
 */
#define SPIN_MIN    0

/** \brief  Maximum value for the buffer size spin button (sec)
 */
#define SPIN_MAX    10

/** \brief  Step size of the spin button (sec) when pushing +/-
 */
#define SPIN_STEP   1



/** \brief  Create spin button to set the buffer size
 *
 * \return  spin button
 */
static GtkWidget *create_spinbutton(void)
{
    return vice_gtk3_resource_spin_int_new("SoundSuspendTime",
            SPIN_MIN, SPIN_MAX, SPIN_STEP);
}


/** \brief  Create a widget to set the "SoundSuspendTime" resource
 *
 * \return  grid
 */
GtkWidget *sound_suspend_time_widget_create(void)
{
    GtkWidget *grid;
    GtkWidget *spin;

    grid = vice_gtk3_grid_new_spaced_with_label(
            VICE_GTK3_DEFAULT, VICE_GTK3_DEFAULT,
            "Suspend time", 1);
    spin = create_spinbutton();
    g_object_set(spin, "margin-left", 16, NULL);
    gtk_grid_attach(GTK_GRID(grid), spin, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("sec."), 1, 1, 1, 1);

    gtk_widget_show_all(grid);
    return grid;
}
