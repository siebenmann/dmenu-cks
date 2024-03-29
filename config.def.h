/* See LICENSE file for copyright and license details. */
/* vim: expandtab
 */
/* Default settings; can be overrided by command line. */

static Bool topbar = True;                  /* -b  option; if False, dmenu appears at bottom */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"monospace:size=10"
};
static const char *prompt      = NULL;      /* -p  option; prompt to the elft of input field */
static const char *normbgcolor = "#222222"; /* -nb option; normal background                 */
static const char *normfgcolor = "#bbbbbb"; /* -nf option; normal foreground                 */
static const char *selbgcolor  = "#005577"; /* -sb option; selected background               */
static const char *selfgcolor  = "#eeeeee"; /* -sf option; selected foreground               */
static const char *outbgcolor  = "#00ffff";
static const char *outfgcolor  = "#000000";
/* -l option; if nonzero, dmenu uses vertical list with given number of lines */
static unsigned int lines      = 0;

/* cks */
static Bool pointermonitor = False;
static Bool deletebs = False;
static Bool tabcomplete = False;

/*
 * Unitary completion handling: if you have 'one two' and 'on twitch'
 * as options and you enter 'on tw' as your entered text, this
 * normally completes to both options because each word you entered
 * matches part of both options. With -U, this completes to only 'on
 * twitch'.
 */
static Bool unitary = False;
