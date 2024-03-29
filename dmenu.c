/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define LENGTH(X)             (sizeof X / sizeof X[0])
#define TEXTNW(X,N)           (drw_font_getexts_width(drw->fonts[0], (X), (N)))
#define TEXTW(X)              (drw_text(drw, 0, 0, 0, 0, (X), 0) + drw->fonts[0]->h)

/* enums */
enum { SchemeNorm, SchemeSel, SchemeOut, SchemeLast }; /* color schemes */

typedef struct Item Item;
struct Item {
	char *text;
	Item *left, *right;
	Bool out;
	Bool hidden;	/* item is not displayed unless autocomplete matches it */
};

static void appenditem(Item *item, Item **list, Item **last);
static void calcoffsets(void);
static char *cistrstr(const char *s, const char *sub);
static void cleanup(void);
static void drawmenu(void);
static void grabkeyboard(void);
static void insert(const char *str, ssize_t n);
static void keypress(XKeyEvent *ev);
static void match(void);
static size_t nextrune(int inc);
static void paste(void);
static void readstdin(void);
static void run(void);
static void setup(void);
static void usage(void);

static Bool setcommonpref(Bool again);

static char text[BUFSIZ] = "";
static int bh, mw, mh;
static int inputw, promptw;
static size_t cursor = 0;
static Atom clip, utf8;
static Item *items = NULL;
static Item *matches, *matchend;
static Item *prev, *curr, *next, *sel;
static Window win;
static XIC xic;
static int mon = -1;

static ClrScheme scheme[SchemeLast];
static Display *dpy;
static int screen;
static Window root;
static Drw *drw;
static int sw, sh; /* X display screen geometry width, height */

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

int
main(int argc, char *argv[]) {
	Bool fast = False;
	int i;

	for(i = 1; i < argc; i++)
		/* these options take no arguments */
		if(!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("dmenu-"VERSION", © 2006-2015 dmenu engineers, see LICENSE for details");
			exit(0);
		}
		else if(!strcmp(argv[i], "-b"))   /* appears at the bottom of the screen */
			topbar = False;
		else if(!strcmp(argv[i], "-f"))   /* grabs keyboard before reading stdin */
			fast = True;
		else if(!strcmp(argv[i], "-i")) { /* case-insensitive item matching */
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		}
		else if(!strcmp(argv[i], "-db"))  /* delete magic setting */
			deletebs = True;
		else if(!strcmp(argv[i], "-P"))	  /* pointer placement */
			pointermonitor = True;
		else if(!strcmp(argv[i], "-t"))   /* shell like tab completion */
			tabcomplete = True;
		else if(!strcmp(argv[i], "-U"))   /* 'unitary' token handling */
			unitary = True;
		else if(i+1 == argc)
			usage();
		/* these options take one argument */
		else if(!strcmp(argv[i], "-l"))   /* number of lines in vertical list */
			lines = atoi(argv[++i]);
		else if(!strcmp(argv[i], "-m"))
			mon = atoi(argv[++i]);
		else if(!strcmp(argv[i], "-p"))   /* adds prompt to left of input field */
			prompt = argv[++i];
		else if(!strcmp(argv[i], "-fn"))  /* font or font set */
			fonts[0] = argv[++i];
		else if(!strcmp(argv[i], "-nb"))  /* normal background color */
			normbgcolor = argv[++i];
		else if(!strcmp(argv[i], "-nf"))  /* normal foreground color */
			normfgcolor = argv[++i];
		else if(!strcmp(argv[i], "-sb"))  /* selected background color */
			selbgcolor = argv[++i];
		else if(!strcmp(argv[i], "-sf"))  /* selected foreground color */
			selfgcolor = argv[++i];
		else
			usage();

	if(!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if(!(dpy = XOpenDisplay(NULL)))
		die("dwm: cannot open display\n");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	drw = drw_create(dpy, screen, root, sw, sh);
	drw_load_fonts(drw, fonts, LENGTH(fonts));
	if(!drw->fontcount)
		die("No fonts could be loaded.\n");
	drw_setscheme(drw, &scheme[SchemeNorm]);

	if(fast) {
		grabkeyboard();
		readstdin();
	}
	else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}

void
appenditem(Item *item, Item **list, Item **last) {
	if(*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

void
calcoffsets(void) {
	int i, n;

	if(lines > 0)
		n = lines * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for(i = 0, next = curr; next; next = next->right)
		if((i += (lines > 0) ? bh : MIN(TEXTW(next->text), n)) > n)
			break;
	for(i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if((i += (lines > 0) ? bh : MIN(TEXTW(prev->left->text), n)) > n)
			break;
}

void
cleanup(void) {
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	drw_clr_free(scheme[SchemeNorm].bg);
	drw_clr_free(scheme[SchemeNorm].fg);
	drw_clr_free(scheme[SchemeSel].fg);
	drw_clr_free(scheme[SchemeSel].bg);
	drw_clr_free(scheme[SchemeOut].fg);
	drw_clr_free(scheme[SchemeOut].bg);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

char *
cistrstr(const char *s, const char *sub) {
	size_t len;

	for(len = strlen(sub); *s; s++)
		if(!strncasecmp(s, sub, len))
			return (char *)s;
	return NULL;
}

void
drawmenu(void) {
	int curpos;
	Item *item;
	int x = 0, y = 0, h = bh, w;

	drw_setscheme(drw, &scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, True, 1, 1);

	if(prompt && *prompt) {
		drw_setscheme(drw, &scheme[SchemeSel]);
		drw_text(drw, x, 0, promptw, bh, prompt, 0);
		x += promptw;
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;
	drw_setscheme(drw, &scheme[SchemeNorm]);
	drw_text(drw, x, 0, w, bh, text, 0);

	if((curpos = TEXTNW(text, cursor) + bh/2 - 2) < w) {
		drw_setscheme(drw, &scheme[SchemeNorm]);
		drw_rect(drw, x + curpos + 2, 2, 1, bh - 4, 1, 1, 0);
	}

	if(lines > 0) {
		/* draw vertical list */
		w = mw - x;
		for(item = curr; item != next; item = item->right) {
			y += h;
			if(item == sel)
				drw_setscheme(drw, &scheme[SchemeSel]);
			else if(item->out)
				drw_setscheme(drw, &scheme[SchemeOut]);
			else
				drw_setscheme(drw, &scheme[SchemeNorm]);

			drw_text(drw, x, y, w, bh, item->text, 0);
		}
	}
	else if(matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if(curr->left) {
			drw_setscheme(drw, &scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, "<", 0);
		}
		for(item = curr; item != next; item = item->right) {
			x += w;
			w = MIN(TEXTW(item->text), mw - x - TEXTW(">"));

			if(item == sel)
				drw_setscheme(drw, &scheme[SchemeSel]);
			else if(item->out)
				drw_setscheme(drw, &scheme[SchemeOut]);
			else
				drw_setscheme(drw, &scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, item->text, 0);
		}
		w = TEXTW(">");
		x = mw - w;
		if(next) {
			drw_setscheme(drw, &scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, ">", 0);
		}
	}
	drw_map(drw, win, 0, 0, mw, mh);
}

void
grabkeyboard(void) {
	int i;

	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for(i = 0; i < 1000; i++) {
		if(XGrabKeyboard(dpy, DefaultRootWindow(dpy), True,
		                 GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		usleep(1000);
	}
	die("cannot grab keyboard\n");
}

void
insert(const char *str, ssize_t n) {
	if(strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if(n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static KeySym lastksym = NoSymbol;
void
keypress(XKeyEvent *ev) {
	char buf[32];
	int len;
	KeySym ksym = NoSymbol, oldksym;
	Status status;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	if(status == XBufferOverflow)
		return;
	oldksym = lastksym;
	lastksym = ksym;
	if(ev->state & ControlMask)
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = deletebs ? XK_Escape : XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while(cursor > 0 && text[nextrune(-1)] == ' ')
				insert(NULL, nextrune(-1) - cursor);
			while(cursor > 0 && text[nextrune(-1)] != ' ')
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_y: /* paste selection */
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	else if(ev->state & Mod1Mask)
		switch(ksym) {
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	switch(ksym) {
	default:
		if(!iscntrl(*buf))
			insert(buf, len);
		break;
	case XK_Delete:
	        if(!deletebs) {
			if(text[cursor] == '\0')
				return;
			cursor = nextrune(+1);
		}
		/* fallthrough */
	case XK_BackSpace:
		if(cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
		if(text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if(next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while(next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
		if(sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
		if(cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if(lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
		if(sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XK_Next:
		if(!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
		if(!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		puts((sel && !(ev->state & ShiftMask)) ? sel->text : text);
		if(!(ev->state & ControlMask)) {
			cleanup();
			exit(0);
		}
		if(sel)
			sel->out = True;
		break;
	case XK_Right:
		if(text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if(lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
		if(sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	case XK_ISO_Left_Tab:
	case XK_Tab:
		if(!sel)
			return;
		if (!tabcomplete ||
		    !setcommonpref(oldksym == XK_Tab ? False : True))
			strncpy(text, sel->text, sizeof text - 1);
		text[sizeof text - 1] = '\0';
		cursor = strlen(text);
		match();
		break;
	}
	drawmenu();
}

void
match(void) {
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len;
	Item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for(s = strtok(buf, " "); s; tokv[tokc-1] = s, s = strtok(NULL, " "))
		if(++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %u bytes\n", tokn * sizeof *tokv);
	if (tokc && unitary) {
		strcpy(buf, text);
		tokc = 1;
		tokv[0] = buf;
	}
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	for(item = items; item && item->text; item++) {
		for(i = 0; i < tokc; i++)
			if(!fstrstr(item->text, tokv[i]))
				break;
		if(i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if(!tokc || !fstrncmp(tokv[0], item->text, len+1)) {
			if (tokc || !item->hidden)
				appenditem(item, &matches, &matchend);
		}
		else if(!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if(lprefix) {
		if(matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		}
		else
			matches = lprefix;
		matchend = prefixend;
	}
	if(lsubstr) {
		if(matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		}
		else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

Bool
setcommonpref(Bool again) {
	size_t len, t, start, maxlen;
	int c;
	Item *item, *prefitem;

	if (!curr || text[0] == 0) {
		return False;
	}

	/* Find an item that is a suffix of the current text, and the
	   minimum length of all items that are suffixes of the current
	   text. */
	maxlen = sizeof(text);
	prefitem = NULL;
	start = strlen(text);
	for (item = curr; item != next; item = item->right) {
		if (fstrncmp(item->text, text, start) != 0)
			continue;
		if (!prefitem)
			prefitem = item;
		t = strlen(item->text);
		if (maxlen > t)
			maxlen = t;
		break;
	}
	if (!prefitem)
		return False;

	/* Repeatedly attempt to lengthen the common prefix. */
	for (len = start; len < maxlen; len++) {
		c = prefitem->text[len];
		for (item = curr; item != next; item = item->right) {
			if (fstrncmp(item->text, text, start) != 0)
				continue;
			if (item->text[len] != c) {
				if (len == start)
					return again;
				else {
					strncpy(text, item->text, len);
					text[len] = 0;
				}
				return True;
			}
		}
	}
	strncpy(text, prefitem->text, len+1);
	return True;
}

size_t
nextrune(int inc) {
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for(n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc);
	return n;
}

void
paste(void) {
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                   utf8, &da, &di, &dl, &dl, (unsigned char **)&p);
	insert(p, (q = strchr(p, '\n')) ? q-p : (ssize_t)strlen(p));
	XFree(p);
	drawmenu();
}

void
readstdin(void) {
	char buf[sizeof text], *p, *maxstr = NULL;
	size_t i, max = 0, size = 0;
	Bool hidden = False;

	/* read each line from stdin and add it to the item list */
	for(i = 0; fgets(buf, sizeof buf, stdin); i++) {
		/* blank line == start hiding items from normal display */
		if(buf[0] == '\n') {
			hidden = True;
			i--;
			continue;
		}
		if(i+1 >= size / sizeof *items)
			if(!(items = realloc(items, (size += BUFSIZ))))
				die("cannot realloc %u bytes:", size);
		if((p = strchr(buf, '\n')))
			*p = '\0';
		if(!(items[i].text = strdup(buf)))
			die("cannot strdup %u bytes:", strlen(buf)+1);
		items[i].out = False;
		items[i].hidden = hidden;
		if(strlen(items[i].text) > max)
			max = strlen(maxstr = items[i].text);
	}
	if(items)
		items[i].text = NULL;
	inputw = maxstr ? TEXTW(maxstr) : 0;
	lines = MIN(lines, i);
}

void
run(void) {
	XEvent ev;

	while(!XNextEvent(dpy, &ev)) {
		if(XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case Expose:
			if(ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case SelectionNotify:
			if(ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if(ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

void
setup(void) {
	int x, y;
	XSetWindowAttributes swa;
	XIM xim;
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window w, pw, dw, *dws;
	XWindowAttributes wa;
	int a, j, di, n, i = -1, area = 0;
	unsigned int du;
#endif

	/* init appearance */
	scheme[SchemeNorm].bg = drw_clr_create(drw, normbgcolor);
	scheme[SchemeNorm].fg = drw_clr_create(drw, normfgcolor);
	scheme[SchemeSel].bg = drw_clr_create(drw, selbgcolor);
	scheme[SchemeSel].fg = drw_clr_create(drw, selfgcolor);
	scheme[SchemeOut].bg = drw_clr_create(drw, outbgcolor);
	scheme[SchemeOut].fg = drw_clr_create(drw, outfgcolor);

	clip = XInternAtom(dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts[0]->h + 2;
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;
#ifdef XINERAMA
	if((info = XineramaQueryScreens(dpy, &n))) {
		XGetInputFocus(dpy, &w, &di);
		if(mon != -1 && mon < n && mon >= 0)
			i = mon;
		if(i < 0 && !pointermonitor && w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if(XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while(w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if(XGetWindowAttributes(dpy, pw, &wa))
				for(j = 0; j < n; j++)
					if((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if(mon == -1 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for(i = 0; i < n; i++)
				if(INTERSECT(x, y, 1, 1, info[i]))
					break;

		/* everything has failed, go with monitor 0. in theory
		   should never happen. */
		if (i < 0 || i == n)
			i = 0;

		x = info[i].x_org;
		y = info[i].y_org + (topbar ? 0 : info[i].height - mh);
		mw = info[i].width;
		XFree(info);
	}
	else
#endif
	{
		x = 0;
		y = topbar ? 0 : sh - mh;
		mw = sw;
	}
	promptw = (prompt && *prompt) ? TEXTW(prompt) : 0;
	inputw = MIN(inputw, mw/3);
	match();

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm].bg->pix;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;
	win = XCreateWindow(dpy, root, x, y, mw, mh, 0,
	                    DefaultDepth(dpy, screen), CopyFromParent,
	                    DefaultVisual(dpy, screen),
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);

	/* open input methods */
	xim = XOpenIM(dpy, NULL, NULL, NULL);
	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dpy, win);
	drw_resize(drw, mw, mh);
	drawmenu();
}

void
usage(void) {
	fputs("usage: dmenu [-b] [-db] [-f] [-i] [-P] [-t] [-U] [-l lines] [-p prompt] [-fn font] [-m monitor]\n"
	      "             [-nb color] [-nf color] [-sb color] [-sf color] [-v]\n", stderr);
	exit(1);
}
