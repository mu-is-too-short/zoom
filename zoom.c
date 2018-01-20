/*
 * This program is very loosely based on xzoom by Itai Nahshon
 * <nahshon@best.com> but I've throw parts out, added new things, and
 * generally hacked it without mercy.  I really don't give a damn
 * what anyone does with this code now.
 *
 * Some of the original hacking on xzoom 0.1 was done by Mathew Francey
 * but I've hacked up that hacked up version (maybe this should be --xzoom++
 * or something but no one would want to type such a thing).  I consider this
 * a complete rewrite of xzoom 0.1.
 *
 *	Eric Howe <mu@trends.net>
 */
#include <rcs.h>
MU_ID("$Mu: zoom/zoom.c 1.16 2001/03/09 04:24:18 $")

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

/*
 * Just in case your headers have hidden this under a mess of
 * #ifdef's ("gcc -ansi" does this to me).  If you don't have
 * usleep at all, then you'll have to make your own with select()
 * or something.
 */
extern void usleep(unsigned long);

/*
 * The MIT-SHM docs claim the existence of this function and my
 * Xext library has it but there is no prototype for it in
 * the headers (sigh).
 */
extern	Status	XShmQueryExtension(Display *);

/*
 * No one seems to agree about where these live.
 */
extern	int	optind;
extern	char	*optarg;
extern	int	getopt(int, char *const *, const char *);

/*
 * Seconds before reverting to the normal titlebar.  You can change
 * this with the "-t" switch.
 */
#define	TIMEOUT	2

/*
 * Defaults.
 */
#define WIDTH	200	/* default width		*/
#define HEIGHT	200	/* default height		*/
#define MAG	2	/* default magnification	*/
#define	SCROLL	16	/* default cursor scroll	*/

/*
 * Globals.
 */
static	Display	 *dpy;
static	Screen	 *scr;
static	int	 set_title;	/* does the title bar need to be reset?	*/
static	unsigned timeout;

/*
 * Standard update delays.
 */
static int delays[] = { 200000, 100000, 50000, 10000, 0 };
#define	N_DELAYS (int)(sizeof(delays)/sizeof(delays[0]))

/*
 * One image.  The `x' and `y' fields will only be meaningful for the
 * source image (they will always be zero in the destination).  The `shm'
 * field will only have meaningful values if you're using XShm (I could
 * have used two different structs but that would be silly and more mess
 * than it would be worth).
 */
typedef struct {
	XImage		*im;
	XShmSegmentInfo	shm;
	int		x, y;
	int		w, h;
} IMAGE;

/*
 * Image constructor and destructor.  See ctor_shared(), ctor_unshared(),
 * dtor_shared(), and dtor_unshared() for details.
 */
static void (*ctor)(IMAGE *) = NULL;
static void (*dtor)(IMAGE *) = NULL;

/*
 * Signal handler for SIGALRM.  Some keyboard commands temporarily modify
 * the title bar to indicate what has been done so we use alarm(2) to
 * put the standard title back after a couple seconds.
 */
static void
timeout_func(int signum)
{
	set_title = True;
}

/*
 * Image builder for XShm.
 */
static void
ctor_shared(IMAGE *i)
{
	i->im = XShmCreateImage(dpy, DefaultVisualOfScreen(scr),
			DefaultDepthOfScreen(scr), ZPixmap, NULL, &i->shm,
			i->w, i->h);
	if(i->im == NULL) {
		perror("XShmCreateImage");
		exit(EXIT_FAILURE);
	}
	i->shm.shmid = shmget(IPC_PRIVATE, i->im->bytes_per_line*i->im->height,
							IPC_CREAT | 0777);
	if(i->shm.shmid < 0) {
		perror("shmget");
		exit(EXIT_FAILURE);
	}

	if((i->shm.shmaddr = (char *)shmat(i->shm.shmid, 0, 0)) == (char *)-1) {
		perror("shmat");
		exit(EXIT_FAILURE);
	}

	i->im->data     = i->shm.shmaddr;
	i->shm.readOnly = False;

	XShmAttach(dpy, &i->shm);
	XSync(dpy, False);

	shmctl(i->shm.shmid, IPC_RMID, 0);
}

/*
 * Image builder for non-XShm.
 */
static void
ctor_nonshared(IMAGE *i)
{
	char	*data;

	i->im = XCreateImage(dpy, DefaultVisualOfScreen(scr),
			DefaultDepthOfScreen(scr), ZPixmap, 0, NULL,
			i->w, i->h, 32, 0);
	if(i->im == NULL) {
		perror("XCreateImage");
		exit(EXIT_FAILURE);
	}

	if((data = malloc(i->h*i->im->bytes_per_line)) == NULL) {
		perror("XCreateImage");
		exit(EXIT_FAILURE);
	}
	i->im->data = data;
}

static void (*ctors[])(IMAGE *) = {
	ctor_shared,
	ctor_nonshared
};

/*
 * Image destroyer for XShm.
 */
static void
dtor_shared(IMAGE *i)
{
	if(i->im == NULL)
		return;
	XShmDetach(dpy, &i->shm);
	shmdt(i->shm.shmaddr);
	i->im->data = NULL;
	XDestroyImage(i->im);
	i->im = NULL;
}

/*
 * Image destroyer for non-XShm.
 */
static void
dtor_nonshared(IMAGE *i)
{
	if(i->im == NULL)
		return;
	free(i->im->data);
	i->im->data = NULL;
	XDestroyImage(i->im);
	i->im = NULL;
}

static void (*dtors[])(IMAGE *) = {
	dtor_shared,
	dtor_nonshared
};

static char short_help[] =
	"[-H] [-x ulx] [-y uly] [-w w] [-h h] [-m m] [-d d] [-n]";
static char *help[] = {
	"-H\tShow this help message.",
	"-x ulx\tSpecify the x-coordinate of the upper left corner.",
	"-y uly\tSpecify the y-coordinate of the upper left corner.",
	"-w w\tSpecify the initial source width.",
	"-h h\tSpecify the initial source height.",
	"-m m\tSpecify the initial magnification.",
	"-d d\tSpecify the display to open.",
	"-t t\tSet the titlebar changing timeout to t seconds.  Various things",
	"    \tuse the titlebar for status messages, the timeout is used to",
	"    \tchange the titlebar back to normal.",
	"-n\tDon't use XShm even if it is available.",
	"",
	"Window commands:",
	"+/-\tZoom in/out.",
	"d\tChange delay between frames.",
	"q\tQuit.",
	"</>\tDecrease/increase scroll step by a factor of 2.",
	"The arrow keys scroll the zoom region.",
	"Drag with mouse button one pressed to select a region.",
	"Use mouse button two to get color values displayed in the titlebar.",
	"",
	"Version:    " VER,
	"Maintainer: mu@trends.net, http://www.trends.net/~mu",
};
static int
usage(char *name, int ret)
{
	int	i;

	printf("usage: %s %s\n", name, short_help);
	for(i = 0; i < (int)(sizeof(help)/sizeof(help[0])); ++i)
		printf("%s%s\n", *help[i] == '\0' ? "" : "\t", help[i]);
	return ret;
}

static void
resize(IMAGE *src, IMAGE *dest, int mag, int new_width, int new_height)
{
	/*
	 * We don't need the old ones.
	 */
	dtor(src);
	dtor(dest);

	/*
	 * find new dimensions for source
	 */
	src->x += src->w/2;
	src->y += src->h/2;

	if((src->w = (new_width + mag - 1)/mag) > WidthOfScreen(scr))
		src->w = WidthOfScreen(scr);
	if((src->h = (new_height + mag - 1)/mag) > HeightOfScreen(scr))
		src->h = HeightOfScreen(scr);

	src->x -= src->w/2;
	src->y -= src->h/2;

	/*
	 * temporary, the dest image may be larger than the actual window
	 */
	dest->w = mag * src->w;
	dest->h = mag * src->h;

	/*
	 * Resurect the images.
	 */
	ctor(src);
	ctor(dest);

	/*
	 * remember actual window size
	 */
	if(dest->w > new_width)
		dest->w = new_width;
	if(dest->h > new_height)
		dest->h = new_height;
}

/**
 ** The copyX() functions are for handling different depths (any depth
 ** should work).  The only real difference between them is in the
 ** inner for-loop (the loop on 'i').
 **/

static void
copy1(IMAGE *src, int mag, IMAGE *dest)
{
static	unsigned char lsb[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
static	unsigned char hsb[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
	int	i, j, k, m;
	char	*d, *s, *ord;
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;

	ord = (char *)(src->im->bitmap_bit_order == LSBFirst ? lsb : hsb);
	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		for(m = 0, i = 0; i < src->w; ++i) {
			if(s[i/8] & ord[i & 7]) {
				for(k = mag; --k >= 0;) {
					*d |= ord[m & 7];
					if(++m == 8)
						m = 0, ++d;
				}
			}
			else {
				for(k = mag; --k >= 0;) {
					*d &= ~ord[m & 7];
					if(++m == 8)
						m = 0, ++d;
				}
			}
		}
		d += (m != 0);

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
copy4(IMAGE *src, int mag, IMAGE *dest)
{
static	unsigned char lsb[] = {0x0f, 0xf0};
static	unsigned char hsb[] = {0xf0, 0x0f};
	int	i, j, k, m;
	char	*d, *s, *ord, c[2];
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;

	ord = (char *)(src->im->bitmap_bit_order == LSBFirst ? lsb : hsb);
	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		for(m = 0, i = 0; i < src->w; ++i) {
			c[k = i & 1] = s[i/2] & ord[i & 1];
			c[1 - k] = ord[1-k]
				 & (ord[1-k] == 0x0f ? (c[k]>>4) : (c[k]<<4));
			for(k = mag; --k >= 0;) {
				*d &= ~ord[m & 1];
				*d |=    c[m & 1];
				if(++m == 2)
					m = 0, ++d;
			}
		}
		d += (m != 0);

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
copy8(IMAGE *src, int mag, IMAGE *dest)
{
	int	i, j, k;
	char	*d, *s;
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;

	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		switch(mag) {
		case 1:
			memcpy((void *)d, (void *)s, src->w);
			d += src->w;
			break;
		case 2:
			for(i = src->w; --i >= 0; ++s) {
				*d++ = *s;
				*d++ = *s;
			}
			break;
		default:
			for(i = src->w; --i >= 0; ++s)
				for(k = mag; --k >= 0;)
					*d++ = *s;
			break;
		}

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
copy16(IMAGE *src, int mag, IMAGE *dest)
{
	int	i, j, k;
	char	*d, *s;
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;

	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		for(i = src->w; --i >= 0; s += 2) {
			for(k = mag; --k >= 0; ) {
				*d++ = s[0];
				*d++ = s[1];
			}
		}

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
copy24(IMAGE *src, int mag, IMAGE *dest)
{
	int	i, j, k;
	char	*d, *s;
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;

	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		for(i = src->w; --i >= 0; s += 4) {
			for(k = mag; --k >= 0;) {
				*d++ = s[0];
				*d++ = s[1];
				*d++ = s[2];
				*d++ = s[3];
			}
		}

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
copy32(IMAGE *src, int mag, IMAGE *dest)
{
	int	i, j, k;
	char	*d, *s;
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;

	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		for(i = src->w; --i >= 0; s += 4) {
			for(k = mag; --k >= 0;) {
				*d++ = s[0];
				*d++ = s[1];
				*d++ = s[2];
				*d++ = s[3];
			}
		}

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
copyn(IMAGE *src, int mag, IMAGE *dest)
{
	int	i, j, k, m;
	char	*d, *s;
	char	*s0 = src->im->data  + src->im->xoffset;
	char	*d0 = dest->im->data + dest->im->xoffset;
	int	bpp = src->im->bits_per_pixel;

	for(j = 0; j < src->h; ++j) {
		d = d0;
		s = s0;

		for(i = src->w; --i >= 0; s += (bpp + 7)/8) {
			for(k = mag; --k >= 0;)
				for(m = 0; m < (bpp + 7)/8; ++m)
					*d++ = s[m];
		}

		s0 += src->im->bytes_per_line;
		for(s = d0, k = 1; k < mag; ++k)
			memcpy(s += dest->im->bytes_per_line, d0, d - d0);
		d0 = s + dest->im->bytes_per_line;
	}
}

static void
get_shared(IMAGE *s)
{
	XShmGetImage(dpy, RootWindowOfScreen(scr), s->im, s->x, s->y,AllPlanes);
}

static void
get_unshared(IMAGE *s)
{
	XGetSubImage(dpy, RootWindowOfScreen(scr), s->x, s->y, s->w, s->h,
					AllPlanes, ZPixmap, s->im, 0, 0);
}

static void (*get_image[])(IMAGE *) = {
	get_shared,
	get_unshared
};

static void
put_shared(Window w, GC gc, IMAGE *i)
{
	XShmPutImage(dpy, w, gc, i->im, 0, 0, 0, 0, i->w, i->h, True);
}

static void
put_unshared(Window w, GC gc, IMAGE *i)
{
	XPutImage(dpy, w, gc, i->im, 0, 0, 0, 0, i->w, i->h);
}

static void (*put_image[])(Window, GC, IMAGE *) = {
	put_shared,
	put_unshared
};

/*
 * for the frame "cursor"
 */
static char frame_bitmap_data[] = { 0 };
static XColor frame_col = { 0 };

static void
class_name(Window win)
{
	char	*cls = "zoom";
	int	len  = strlen(cls);
	XChangeProperty(dpy, win, XA_WM_CLASS, XA_STRING, 8,
				PropModeReplace, (unsigned char *)cls, len);
}

/*
 * Set the window title.
 */
static void
title_bar(Window win, const char *fmt, ...)
{
	va_list	ap;
	char	title[80];
	int	len;

	va_start(ap, fmt);
	vsprintf(title, fmt, ap);
	va_end(ap);
	len = strlen(title);

	XChangeProperty(dpy, win, XA_WM_NAME, XA_STRING, 8,
				PropModeReplace, (unsigned char *)title, len);
	XChangeProperty(dpy, win, XA_WM_ICON_NAME, XA_STRING, 8,
				PropModeReplace, (unsigned char *)title, len);
}

/*
 * Show the color at the specified coordinates.
 * The color will be displayed in the titlebar as an RGB triple.
 * The coordinates should be relative to `win'.
 */
static void
show_color(Window win, IMAGE *im, int x, int y)
{
	XColor            xc;
	XWindowAttributes wa;

	if(im == NULL
	|| x < 0 || x > im->w
	|| y < 0 || y > im->h)
		return;

	/*
	 * I suppose I could cache the colormap but asking for it each
	 * time doesn't appear to have much impact.
	 */
	XGetWindowAttributes(dpy, win, &wa);

	memset((void *)&xc, '\0', sizeof(xc));
	xc.pixel = XGetPixel(im->im, x, y);
	XQueryColor(dpy, wa.colormap, &xc);
	title_bar(win, "%4.4x,%4.4x,%4.4x", (unsigned)xc.red,
					(unsigned)xc.green, (unsigned)xc.blue);
}

int
main(int argc, char **argv)
{
	XSetWindowAttributes	xswa;
	XGCValues		gcv;
	struct sigaction	catch_alarm;
	int	i, mask, button, unmapped, scroll;
	XEvent	ev;
	char	*progname;
	Atom	dw;
	Pixmap	curs;
	IMAGE	src, dest, *image = NULL;
	GC	gc, framegc;
	Cursor	invisible, cursor;
	int	delay_index = 0;
	int	delay       = delays[delay_index];
	char	*dpyname    = NULL;
	int	xshm;
	int	mag;
	Window	win;
	void	(*i_get)(IMAGE *)              = NULL;
	void	(*i_put)(Window, GC, IMAGE *)  = NULL;
	void	(*copy)(IMAGE *, int, IMAGE *) = NULL;

	if((progname = strrchr(argv[0], '/')) != NULL)
		++progname;
	else
		progname = argv[0];

	/*
	 * Initialize our SIGALRM catcher.
	 */
	memset((void *)&catch_alarm, '\0', sizeof(catch_alarm));
	sigemptyset(&catch_alarm.sa_mask);
	catch_alarm.sa_handler = timeout_func;
	sigaction(SIGALRM, &catch_alarm, NULL);

	memset((void *)&src,  '\0', sizeof(src));
	memset((void *)&dest, '\0', sizeof(dest));
	dest.w = WIDTH;
	dest.h = HEIGHT;

	timeout = TIMEOUT;
	xshm    = True;
	mag     = MAG;
	while((i = getopt(argc, argv, "t:x:y:w:h:Hm:d:n")) != EOF) {
		switch(i) {
		case 'x':
			if((src.x = atoi(optarg)) < 0)
				src.x = 0;
			break;
		case 'y':
			if((src.y = atoi(optarg)) < 0)
				src.y = 0;
			break;
		case 'w':
			if((dest.w = atoi(optarg)) < 0)
				dest.w = WIDTH;
			break;
		case 'h':	
			if((dest.h = atoi(optarg)) < 0)
				dest.h = HEIGHT;
			break;
		case 'm':
			if((mag = atoi(optarg)) < 1)
				mag = 1;
			break;
		case 'd':
			dpyname = optarg;
			break;
		case 'n':
			xshm = False;
			break;
		case 't':
			timeout = (unsigned)atoi(optarg);
			break;
		case 'H':
			return usage(progname, EXIT_SUCCESS);
			break;
		default:
			return usage(progname, EXIT_FAILURE);
			break;
		}
	}

	if((dpy = XOpenDisplay(dpyname)) == NULL) {
		perror("Cannot open display\n");
		exit(EXIT_FAILURE);
	}
	scr = DefaultScreenOfDisplay(dpy);

	/*
	 * We could just use XListExtensions() and search for "MIT-SHM"
	 * _but_ that leaves out the possibility that the server and
	 * our process are not on the same machine so XShm won't work
	 * even if the server has the extension; XShmQueryExtension()
	 * should check for both conditions.  Of course I have seen
	 * XShmQueryExtension() fail to detect a remote display, sigh.
	 */
	if(xshm && XShmQueryExtension(dpy) == 0)
		xshm = False;

	/*
	 * Set up all our function pointers.
	 */
	ctor  = ctors[xshm == 0];
	dtor  = dtors[xshm == 0];
	i_get = get_image[xshm == 0];
	i_put = put_image[xshm == 0];
	switch(DefaultDepthOfScreen(scr)) {
	case 1:		copy = copy1;	break;
	case 4:		copy = copy4;	break;
	case 8:		copy = copy8;	break;
	case 16:	copy = copy16;	break;
	case 24:	copy = copy24;	break;
	case 32:	copy = copy32;	break;
	default:	copy = copyn;	break;
	}

	mask = 0;
	xswa.event_mask = ButtonPressMask | ButtonReleaseMask | ButtonMotionMask
			| StructureNotifyMask | KeyPressMask | KeyReleaseMask;
	mask |= CWEventMask;
	xswa.background_pixel = BlackPixelOfScreen(scr);
	mask |= CWBackPixel;
	win = XCreateWindow(dpy, RootWindowOfScreen(scr),
				0, 0, dest.w, dest.h, 0,
				DefaultDepthOfScreen(scr), InputOutput,
				DefaultVisualOfScreen(scr), mask, &xswa);
	class_name(win);
	set_title = True;

	/*
	 * WM_DELETE_WINDOW protocol support
	 */
	if((dw = XInternAtom(dpy, "WM_DELETE_WINDOW", False)) != (Atom)0)
		XSetWMProtocols(dpy, win, &dw, 1);

	mask = 0;
	gcv.plane_mask     = AllPlanes;
	mask              |= GCPlaneMask;
	gcv.subwindow_mode = IncludeInferiors;
	mask              |= GCSubwindowMode;
	gcv.function       = GXcopy;
	mask              |= GCFunction;
	gcv.foreground     = WhitePixelOfScreen(scr);
	mask              |= GCForeground;
	gcv.background     = BlackPixelOfScreen(scr);
	mask              |= GCBackground;
	gc = XCreateGC(dpy, RootWindowOfScreen(scr), mask, &gcv);

	mask = 0;
	gcv.foreground     = AllPlanes;
	mask              |= GCForeground;
	gcv.plane_mask     = WhitePixelOfScreen(scr) ^ BlackPixelOfScreen(scr);
	mask              |= GCPlaneMask;
	gcv.subwindow_mode = IncludeInferiors;
	mask              |= GCSubwindowMode;
	gcv.function       = GXxor;
	mask              |= GCFunction;
	framegc = XCreateGC(dpy, RootWindowOfScreen(scr), mask, &gcv);

	resize(&src, &dest, mag, dest.w, dest.h);

	curs = XCreatePixmapFromBitmapData(dpy, RootWindowOfScreen(scr),
					frame_bitmap_data, 1, 1, 0, 0, 1);
	invisible = XCreatePixmapCursor(dpy, curs, curs,
					&frame_col, &frame_col, 0, 0);

	cursor = XCreateFontCursor(dpy, XC_crosshair);
	XDefineCursor(dpy, win, cursor);

	XMapWindow(dpy, win);

	unmapped = True;
	button   = 0;
	scroll   = SCROLL;
	for(;;) {
		while(unmapped || XPending(dpy) != 0) {
			XNextEvent(dpy, &ev);
			switch(ev.type) {
			case ConfigureNotify:
				if(ev.xconfigure.width  != dest.w
				|| ev.xconfigure.height != dest.h)
					resize(&src, &dest, mag,
						ev.xconfigure.width,
						ev.xconfigure.height);
				break;

			case MapNotify:
				unmapped = False;
				break;

			case UnmapNotify:
				unmapped = True;
				break;

			case ClientMessage:
				/*
				 * WM_DELETE_WINDOW?
				 */
				if(ev.xclient.data.l[0] == dw)
					exit(EXIT_SUCCESS);
				break;

			case KeyPress:
				switch(XKeycodeToKeysym(dpy,ev.xkey.keycode,0)){
				case '+':
				case '=':
					++mag;
					resize(&src, &dest, mag, dest.w,dest.h);
					set_title = True;
					break;

				case '-':
					if(--mag < 1)
						mag = 1;
					resize(&src, &dest, mag, dest.w, dest.h);
					set_title = True;
					break;

				case XK_Left:
					src.x -= scroll;
					break;

				case XK_Right:
					src.x += scroll;
					break;

				case XK_Up:
					src.y -= scroll;
					break;

				case XK_Down:
					src.y += scroll;
					break;

				case 'd':
					if(++delay_index >= N_DELAYS)
						delay_index = 0;
					delay = delays[delay_index];
					title_bar(win, "delay = %d ms",
								delay/1000);
					alarm(timeout);
					break;

				case ',': /* '<' */
					if((scroll /= 2) < 1)
						scroll = 1;
					title_bar(win, "scroll = %d pixel%s",
						scroll, scroll == 1 ? "" : "s");
					signal(SIGALRM, timeout_func);
					alarm(TIMEOUT);
					break;

				case '.': /* '>' */
					scroll *= 2;
					title_bar(win, "scroll = %d pixel%s",
						scroll, scroll == 1 ? "" : "s");
					signal(SIGALRM, timeout_func);
					alarm(2);
					break;

				case 'q':
					exit(EXIT_SUCCESS);
					break;

				default:
					break;
				}
				break;

			case ButtonPress:
				if(ev.xbutton.button == 2) {
					show_color(win, image, ev.xbutton.x,
								ev.xbutton.y);
					button = 2;
					break;
				}
				if(ev.xbutton.button != 1)
					break;
				src.x = ev.xbutton.x_root - src.w/2;
				src.y = ev.xbutton.y_root - src.h/2;
				XDefineCursor(dpy, win, invisible);
				button = 1;
				break;

			case ButtonRelease:
				if(button == 1)
					XDefineCursor(dpy, win, cursor);
				else if(button == 2)
					alarm(timeout);
				button = 0;
				break;

			case MotionNotify:
				if(button == 2) {
					show_color(win, image, ev.xmotion.x,
								ev.xmotion.y);
					break;
				}
				if(button != 1)
					break;
				src.x = ev.xmotion.x_root - src.w/2;
				src.y = ev.xmotion.y_root - src.h/2;
				break;

			default:
				break;
			}

			/*
			 * Clip the source region.  We don't want to try
			 * and grab something that isn't on the screen.
			 */
			if(src.x < 0)
				src.x = 0;
			else if(src.x > WidthOfScreen(scr) - src.w)
				src.x = WidthOfScreen(scr) - src.w;
			if(src.y < 0)
				src.y = 0;
			else if(src.y > HeightOfScreen(scr) - src.h)
				src.y = HeightOfScreen(scr) - src.h;
		}

		if(unmapped)
			continue;

		/*
		 * Yank image.
		 */
		i_get(&src);

		/*
		 * Show the frame.
		 */
		if(button == 1) {
			XDrawRectangle(dpy, RootWindowOfScreen(scr), framegc,
					src.x, src.y, src.w - 1, src.h - 1);
			XSync(dpy, False);
		}

		/*
		 * Copy/zoom from src image to dest ("One small optimization
		 * for man, ...").  This little hack should make the "cool
		 * acid trip zoom level one" effect a little better and that
		 * is certainly a worthy goal.
		 */
		if(mag != 1)
			copy(&src, mag, image = &dest);
		else
			image = &src;

		/*
		 * Plop expanded image.
		 */
		i_put(win, gc, image);

		if(set_title) {
			title_bar(win, "%s%s x%d", progname,
						xshm ? "(shm)" : "", mag);
			set_title = False;
		}
		XSync(dpy, 0);

		/*
		 * This used to be "if(button == 0 && delay > 0)" but
		 * the delay is always non-negative so I changed the
		 * ">" to "!="; this little tweak appears to make things
		 * go a little faster.  The only difference (yes, I checked)
		 * is a "jle" (old version) versus a "jne" (new version)
		 * at the assembler level.
		 *
		 * Maybe this is no surprise but hey, I'm not an assembler
		 * geek and I thought this was kinda' cool so don't laugh
		 * at my pink bicycle buddy.
		 */
		if(button == 0 && delay != 0)
			usleep(delay);

		/*
		 * erase the frame
		 */
		if(button == 1)
			XDrawRectangle(dpy, RootWindowOfScreen(scr), framegc,
					src.x, src.y, src.w - 1, src.h - 1);
	}

	/*
	 * This should never be reached but we must satisfy the angry
	 * compiler gods.
	 */
	return EXIT_FAILURE;
}
