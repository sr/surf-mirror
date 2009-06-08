/* See LICENSE file for copyright and license details.
 *
 * To understand surf, start reading main().
 */
#include <X11/X.h>
#include <X11/Xatom.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <webkit/webkit.h>
#include <glib/gstdio.h>

#define LENGTH(x) (sizeof x / sizeof x[0])

Display *dpy;
Atom urlprop;
typedef struct Client {
	GtkWidget *win, *scroll, *vbox, *urlbar, *searchbar;
	WebKitWebView *view;
	WebKitDownload *download;
	gchar *title;
	gint progress;
	struct Client *next;
} Client;
SoupCookieJar *cookiejar;
Client *clients = NULL;
gboolean embed = FALSE;
gboolean showxid = FALSE;
gboolean ignore_once = FALSE;
extern char *optarg;
extern int optind;

static void cleanup(void);
static void destroyclient(Client *c);
static void destroywin(GtkWidget* w, gpointer d);
static void die(char *str);
static void download(WebKitDownload *o, GParamSpec *pspec, gpointer d);
static gboolean initdownload(WebKitWebView *view, WebKitDownload *o, gpointer d);
static gchar *geturi(Client *c);
static void hidesearch(Client *c);
static void hideurl(Client *c);
static gboolean keypress(GtkWidget* w, GdkEventKey *ev, gpointer d);
static void linkhover(WebKitWebView* page, const gchar* t, const gchar* l, gpointer d);
static void loadcommit(WebKitWebView *view, WebKitWebFrame *f, gpointer d);
static void loadstart(WebKitWebView *view, WebKitWebFrame *f, gpointer d);
static void loadfile(Client *c, const gchar *f);
static void loaduri(Client *c, const gchar *uri);
static Client *newclient();
static WebKitWebView *newwindow(WebKitWebView  *v, WebKitWebFrame *f, gpointer d);
static void progresschange(WebKitWebView *view, gint p, gpointer d);
static GdkFilterReturn processx(GdkXEvent *xevent, GdkEvent *event, gpointer data);
static void setup(void);
static void showsearch(Client *c);
static void showurl(Client *c);
static void stop(Client *c);
static void titlechange(WebKitWebView* view, WebKitWebFrame* frame, const gchar* title, gpointer d);
static void usage();
static void updatetitle(Client *c, const gchar *title);

void
cleanup(void) {
	while(clients)
		destroyclient(clients);
}

void
destroyclient(Client *c) {
	Client *p;

	gtk_widget_destroy(GTK_WIDGET(webkit_web_view_new()));
	gtk_widget_destroy(c->scroll);
	gtk_widget_destroy(c->urlbar);
	gtk_widget_destroy(c->searchbar);
	gtk_widget_destroy(c->vbox);
	gtk_widget_destroy(c->win);
	for(p = clients; p && p->next != c; p = p->next);
	if(p)
		p->next = c->next;
	else
		clients = c->next;
	free(c);
	if(clients == NULL)
		gtk_main_quit();
}

void
destroywin(GtkWidget* w, gpointer d) {
	Client *c = (Client *)d;

	destroyclient(c);
}

void
die(char *str) {
	fputs(str, stderr);
	exit(EXIT_FAILURE);
}

void
download(WebKitDownload *o, GParamSpec *pspec, gpointer d) {
	Client *c = (Client *) d;
	WebKitDownloadStatus status;

	status = webkit_download_get_status(c->download);
	if(status == WEBKIT_DOWNLOAD_STATUS_STARTED || status == WEBKIT_DOWNLOAD_STATUS_CREATED) {
		c->progress = (int)(webkit_download_get_progress(c->download)*100);
	}
	else {
		stop(c);
	}
	updatetitle(c, NULL);
}

gboolean
initdownload(WebKitWebView *view, WebKitDownload *o, gpointer d) {
	Client *c = (Client *) d;
	const gchar *home, *filename;
	gchar *uri, *path;
	GString *html = g_string_new("");

	stop(c);
	c->download = o;
	home = g_get_home_dir();
	filename = webkit_download_get_suggested_filename(o);
	path = g_build_filename(home, ".surf", "dl", 
			filename, NULL);
	uri = g_strconcat("file://", path, NULL);
	webkit_download_set_destination_uri(c->download, uri);
	c->progress = 0;
	g_free(uri);
	html = g_string_append(html, "Downloading <b>");
	html = g_string_append(html, filename);
	html = g_string_append(html, "</b>...");
	webkit_web_view_load_html_string(c->view, html->str,
			webkit_download_get_uri(c->download));
	g_signal_connect(c->download, "notify::progress", G_CALLBACK(download), c);
	g_signal_connect(c->download, "notify::status", G_CALLBACK(download), c);
	webkit_download_start(c->download);
	updatetitle(c, filename);
	return TRUE;
}

gchar *
geturi(Client *c) {
	gchar *uri;

	if(!(uri = (gchar *)webkit_web_view_get_uri(c->view)))
		uri = g_strdup("about:blank");
	return uri;
}

void
hidesearch(Client *c) {
	gtk_widget_hide(c->searchbar);
	gtk_widget_grab_focus(GTK_WIDGET(c->view));
}

void
hideurl(Client *c) {
	gtk_widget_hide(c->urlbar);
	gtk_widget_grab_focus(GTK_WIDGET(c->view));
}

gboolean
keypress(GtkWidget* w, GdkEventKey *ev, gpointer d) {
	Client *c = (Client *)d;

	if(ev->type != GDK_KEY_PRESS)
		return FALSE;
	if(GTK_WIDGET_HAS_FOCUS(c->searchbar)) {
		switch(ev->keyval) {
		case GDK_Escape:
			hidesearch(c);
			return TRUE;
		case GDK_Return:
			webkit_web_view_search_text(c->view,
					gtk_entry_get_text(GTK_ENTRY(c->searchbar)),
					FALSE,
					!(ev->state & GDK_SHIFT_MASK),
					TRUE);
			return TRUE;
		case GDK_Left:
		case GDK_Right:
			return FALSE;
		}
	}
	else if(GTK_WIDGET_HAS_FOCUS(c->urlbar)) {
		switch(ev->keyval) {
		case GDK_Escape:
			hideurl(c);
			return TRUE;
		case GDK_Return:
			loaduri(c, gtk_entry_get_text(GTK_ENTRY(c->urlbar)));
			hideurl(c);
			return TRUE;
		case GDK_Left:
		case GDK_Right:
			return FALSE;
		}
	}
	if(ev->state & GDK_CONTROL_MASK) {
		switch(ev->keyval) {
		case GDK_r:
		case GDK_R:
			if((ev->state & GDK_SHIFT_MASK))
				 webkit_web_view_reload_bypass_cache(c->view);
			else
				 webkit_web_view_reload(c->view);
			return TRUE;
		case GDK_b:
			return TRUE;
		case GDK_g:
			showurl(c);
			return TRUE;
		case GDK_slash:
			showsearch(c);
			return TRUE;
		case GDK_plus:
		case GDK_equal:
			webkit_web_view_zoom_in(c->view);
			return TRUE;
		case GDK_minus:
			webkit_web_view_zoom_out(c->view);
			return TRUE;
		case GDK_0:
			webkit_web_view_set_zoom_level(c->view, 1.0);
			return TRUE;
		case GDK_n:
		case GDK_N:
			webkit_web_view_search_text(c->view,
					gtk_entry_get_text(GTK_ENTRY(c->searchbar)),
					FALSE,
					!(ev->state & GDK_SHIFT_MASK),
					TRUE);
			return TRUE;
		case GDK_Left:
			webkit_web_view_go_back(c->view);
			return TRUE;
		case GDK_Right:
			webkit_web_view_go_forward(c->view);
			return TRUE;
		}
	}
	else {
		switch(ev->keyval) {
		case GDK_Escape:
			stop(c);
			return TRUE;
		}
	}
	return FALSE;
}

void
linkhover(WebKitWebView* page, const gchar* t, const gchar* l, gpointer d) {
	Client *c = (Client *)d;

	if(l)
		gtk_window_set_title(GTK_WINDOW(c->win), l);
	else
		updatetitle(c, NULL);
}

void
loadcommit(WebKitWebView *view, WebKitWebFrame *f, gpointer d) {
	Client *c = (Client *)d;
	gchar *uri;

	uri = geturi(c);
	ignore_once = TRUE;
	XChangeProperty(dpy, GDK_WINDOW_XID(GTK_WIDGET(c->win)->window), urlprop,
			XA_STRING, 8, PropModeReplace, (unsigned char *)uri,
			strlen(uri) + 1);
}

void
loadstart(WebKitWebView *view, WebKitWebFrame *f, gpointer d) {
	Client *c = (Client *)d;

	if(c->download)
		stop(c);
}

void
loadfile(Client *c, const gchar *f) {
	GIOChannel *chan = NULL;
	GError *e = NULL;
	GString *code = g_string_new("");
	GString *uri = g_string_new(f);
	gchar *line;

	if(strcmp(f, "-") == 0) {
		chan = g_io_channel_unix_new(STDIN_FILENO);
		if (chan) {
			while(g_io_channel_read_line(chan, &line, NULL, NULL,
						&e) == G_IO_STATUS_NORMAL) {
				g_string_append(code, line);
				g_free(line);
			}
			webkit_web_view_load_html_string(c->view, code->str,
					"file://.");
			g_io_channel_shutdown(chan, FALSE, NULL);
		}
	}
	else {
		g_string_prepend(uri, "file://");
		loaduri(c, uri->str);
	}
	updatetitle(c, uri->str);
}

void
loaduri(Client *c, const gchar *uri) {
	GString* u = g_string_new(uri);
	if(g_strrstr(u->str, ":") == NULL)
		g_string_prepend(u, "http://");
	webkit_web_view_load_uri(c->view, u->str);
	c->progress = 0;
	updatetitle(c, u->str);
	g_string_free(u, TRUE);
}

Client *
newclient(void) {
	Client *c;
	if(!(c = calloc(1, sizeof(Client))))
		die("Cannot malloc!\n");
	/* Window */
	if(embed) {
		c->win = gtk_plug_new(0);
	}
	else {
		c->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_window_set_wmclass(GTK_WINDOW(c->win), "surf", "surf");
	}
	gtk_window_set_default_size(GTK_WINDOW(c->win), 800, 600);
	g_signal_connect(G_OBJECT(c->win), "destroy", G_CALLBACK(destroywin), c);
	g_signal_connect(G_OBJECT(c->win), "key-press-event", G_CALLBACK(keypress), c);

	/* VBox */
	c->vbox = gtk_vbox_new(FALSE, 0);

	/* scrolled window */
	c->scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroll),
			GTK_POLICY_NEVER, GTK_POLICY_NEVER);

	/* webview */
	c->view = WEBKIT_WEB_VIEW(webkit_web_view_new());
	g_signal_connect(G_OBJECT(c->view), "title-changed", G_CALLBACK(titlechange), c);
	g_signal_connect(G_OBJECT(c->view), "load-progress-changed", G_CALLBACK(progresschange), c);
	g_signal_connect(G_OBJECT(c->view), "load-committed", G_CALLBACK(loadcommit), c);
	g_signal_connect(G_OBJECT(c->view), "load-started", G_CALLBACK(loadstart), c);
	g_signal_connect(G_OBJECT(c->view), "hovering-over-link", G_CALLBACK(linkhover), c);
	g_signal_connect(G_OBJECT(c->view), "create-web-view", G_CALLBACK(newwindow), c);
	g_signal_connect(G_OBJECT(c->view), "download-requested", G_CALLBACK(initdownload), c);

	/* urlbar */
	c->urlbar = gtk_entry_new();
	gtk_entry_set_has_frame(GTK_ENTRY(c->urlbar), FALSE);

	/* searchbar */
	c->searchbar = gtk_entry_new();
	gtk_entry_set_has_frame(GTK_ENTRY(c->searchbar), FALSE);

	/* downloadbar */

	/* Arranging */
	gtk_container_add(GTK_CONTAINER(c->scroll), GTK_WIDGET(c->view));
	gtk_container_add(GTK_CONTAINER(c->win), c->vbox);
	gtk_container_add(GTK_CONTAINER(c->vbox), c->scroll);
	gtk_container_add(GTK_CONTAINER(c->vbox), c->searchbar);
	gtk_container_add(GTK_CONTAINER(c->vbox), c->urlbar);

	/* Setup */
	gtk_box_set_child_packing(GTK_BOX(c->vbox), c->urlbar, FALSE, FALSE, 0, GTK_PACK_START);
	gtk_box_set_child_packing(GTK_BOX(c->vbox), c->searchbar, FALSE, FALSE, 0, GTK_PACK_START);
	gtk_box_set_child_packing(GTK_BOX(c->vbox), c->scroll, TRUE, TRUE, 0, GTK_PACK_START);
	gtk_widget_grab_focus(GTK_WIDGET(c->view));
	gtk_widget_hide_all(c->searchbar);
	gtk_widget_hide_all(c->urlbar);
	gtk_widget_show(c->vbox);
	gtk_widget_show(c->scroll);
	gtk_widget_show(GTK_WIDGET(c->view));
	gtk_widget_show(c->win);
	gdk_window_set_events(GTK_WIDGET(c->win)->window, GDK_ALL_EVENTS_MASK);
	gdk_window_add_filter(GTK_WIDGET(c->win)->window, processx, c);
	webkit_web_view_set_full_content_zoom(c->view, TRUE);
	c->download = NULL;
	c->title = NULL;
	c->next = clients;
	clients = c;
	if(showxid)
		printf("%u\n", (unsigned int)GDK_WINDOW_XID(GTK_WIDGET(c->win)->window));
	return c;
}

WebKitWebView *
newwindow(WebKitWebView  *v, WebKitWebFrame *f, gpointer d) {
	Client *c = newclient();
	return c->view;
}

void
progresschange(WebKitWebView* view, gint p, gpointer d) {
	Client *c = (Client *)d;

	c->progress = p;
	updatetitle(c, NULL);
}

GdkFilterReturn
processx(GdkXEvent *e, GdkEvent *event, gpointer d) {
	XPropertyEvent *ev;
	Client *c = (Client *)d;
	Atom adummy;
	int idummy;
	unsigned long ldummy;
	unsigned char *buf = NULL;
	if(((XEvent *)e)->type == PropertyNotify) {
		ev = &((XEvent *)e)->xproperty;
		if(ignore_once == FALSE && ev->atom == urlprop && ev->state == PropertyNewValue) {
			XGetWindowProperty(dpy, ev->window, urlprop, 0L, BUFSIZ, False, XA_STRING,
				&adummy, &idummy, &ldummy, &ldummy, &buf);
			loaduri(c, (gchar *)buf);
			XFree(buf);
			return GDK_FILTER_REMOVE;
		}
	}
	return GDK_FILTER_CONTINUE;
}

void setup(void) {
	dpy = GDK_DISPLAY();
	urlprop = XInternAtom(dpy, "_SURF_URL", False);
}

void
showsearch(Client *c) {
	hideurl(c);
	gtk_widget_show(c->searchbar);
	gtk_widget_grab_focus(c->searchbar);
}

void
showurl(Client *c) {
	gchar *uri;

	hidesearch(c);
	uri = geturi(c);
	gtk_entry_set_text(GTK_ENTRY(c->urlbar), uri);
	gtk_widget_show(c->urlbar);
	gtk_widget_grab_focus(c->urlbar);
}

void
stop(Client *c) {
	if(c->download)
		webkit_download_cancel(c->download);
	else
		webkit_web_view_stop_loading(c->view);
	c->download = NULL;
}

void
titlechange(WebKitWebView *v, WebKitWebFrame *f, const gchar *t, gpointer d) {
	Client *c = (Client *)d;

	updatetitle(c, t);
}

void
usage() {
	fputs("surf - simple browser\n", stderr);
	die("usage: surf [-e] [-x] [-u uri] [-f file]\n");
}

void
updatetitle(Client *c, const char *title) {
	char t[512];

	if(title) {
		if(c->title)
			g_free(c->title);
		c->title = g_strdup(title);
	}
	if(c->progress == 100)
		snprintf(t, LENGTH(t), "%s", c->title);
	else
		snprintf(t, LENGTH(t), "%s [%i%%]", c->title, c->progress);
	gtk_window_set_title(GTK_WINDOW(c->win), t);
}

int main(int argc, char *argv[]) {
	SoupSession *s;
	Client *c;
	int o;
	const gchar *home, *filename;

	gtk_init(NULL, NULL);
	if (!g_thread_supported())
		g_thread_init(NULL);
	setup();
	while((o = getopt(argc, argv, "vhxeu:f:")) != -1)
		switch(o) {
		case 'x':
			showxid = TRUE;
			break;
		case 'e':
			showxid = TRUE;
			embed = TRUE;
			break;
		case 'u':
			c = newclient();
			loaduri(c, optarg);
			break;
		case 'f':
			c = newclient();
			loadfile(c, optarg);
			break;
		case 'v':
			die("surf-"VERSION", © 2009 surf engineers, see LICENSE for details\n");
			break;
		default:
			usage();
		}
	if(optind != argc)
		usage();
	if(!clients)
		newclient();

	/* make dirs */
	home = g_get_home_dir();
	filename = g_build_filename(home, ".surf", NULL);
	g_mkdir_with_parents(filename, 0711);
	filename = g_build_filename(home, ".surf", "dl", NULL);
	g_mkdir_with_parents(filename, 0755);

	/* cookie persistance */
	s = webkit_get_default_session();
	filename = g_build_filename(home, ".surf", "cookies", NULL);
	cookiejar = soup_cookie_jar_text_new(filename, FALSE);
	soup_session_add_feature(s, SOUP_SESSION_FEATURE(cookiejar));

	gtk_main();
	cleanup();
	return EXIT_SUCCESS;
}
