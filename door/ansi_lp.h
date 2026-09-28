/*
 * ansi_lp.h - the Listening Post as a 24-bit ANSI screen (79x24), for callers
 * without TRACE: a live half-block map, a node list, the public chat.
 */
#ifndef ANSI_LP_H
#define ANSI_LP_H

#include "lp.h"

void ansi_lp_run(LpUserPrefs *prefs);

#endif
