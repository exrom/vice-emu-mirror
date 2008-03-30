/*
 * ui_cmdline.c - Commandline output for Vice/2
 *
 * Written by
 *  Thomas Bretz <tbretz@gsi.de>
 *
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
 *
 */

#define INCL_DOSPROCESS
#define INCL_WINDIALOGS // WinProcessDlg
#include "vice.h"

#include <string.h>
#include <stdlib.h>

#include "cmdline.h"

#include "dialogs.h"
#include "utils.h"

void ui_cmdline_show_help(int num_options, cmdline_option_t *opt)
{
    int   i, j, jmax;
    int   chars;   // maximum area could be shown
    char  format[13];

    HWND hwnd;

    //
    // calculate maximum width of text
    //
    jmax = 0;
    for (i=0; i<num_options; i++)
    {
        j     =strlen(opt[i].name)+1;

        j    +=strlen((opt[i].need_arg && opt[i].param_name)?
                      opt[i].param_name:"")+1;

        jmax  = j>jmax ? j : jmax;

        j    += strlen(opt[i].description)+1;

        chars = j>chars ? j : chars;
    }

    sprintf(format, "%%-%ds%%s", jmax);

    //
    // open dialog
    //
    hwnd = cmdopt_dialog(HWND_DESKTOP);

    //
    // fill dialog with text
    //
    for (i=0; i<num_options; i++)
    {
        char *textopt = xmsprintf("%s %s", opt[i].name,
                                  (opt[i].need_arg && opt[i].param_name)?
                                  opt[i].param_name:"");
        char *text = xmsprintf(format, textopt, opt[i].description);
        free(textopt);

        WinSendMsg(hwnd, WM_INSERT, text, 0);
        free(text);
    }

    //
    // MAINLOOP
    //
    WinProcessDlg(hwnd);

    //
    // WinProcessDlg() does NOT destroy the window on return! Do it here,
    // otherwise the window procedure won't ever get a WM_DESTROY,
    // which we may want :-)
    //
    WinDestroyWindow(hwnd);
}
