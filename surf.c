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

typedef struct Cookie {
	char *name;
	char *value;
	char *domain;
	char *path;
	struct Cookie *next;
} Cookie;

SoupCookieJar *cookiejar;
SoupSession *session;
Client *clients = NULL;
Cookie *cookies = NULL;
gboolean embed = FALSE;
gboolean showxid = FALSE;
gboolean ignore_once = FALSE;
extern char *optarg;
extern int optind;

static void cleanup(void);
static void proccookies(SoupMessage *m, Client *c);
static void destroyclient(Client *c);
static void destroywin(GtkWidget* w, Client *c);
static void die(char *str);
static void download(WebKitDownload *o, GParamSpec *pspec, Client *c);
static gboolean initdownload(WebKitWebView *view, WebKitDownload *o, Client *c);
static gchar *geturi(Client *c);
static void hidesearch(Client *c);
static void hideurl(Client *c);
static gboolean keypress(GtkWidget* w, GdkEventKey *ev, Client *c);
static void linkhover(WebKitWebView* page, const gchar* t, const gchar* l, Client *c);
static void loadcommit(WebKitWebView *view, WebKitWebFrame *f, Client *c);
static void loadstart(WebKitWebView *view, WebKitWebFrame *f, Client *c);
static void loadfile(Client *c, const gchar *f);
static void loaduri(Client *c, const gchar *uri);
static Client *newclient();
static WebKitWebView *newwindow(WebKitWebView  *v, WebKitWebFrame *f, Client *c);
static void pasteurl(GtkClipboard *clipboard, const gchar *text, gpointer d);
static GdkFilterReturn processx(GdkXEvent *xevent, GdkEvent *event, gpointer d);
static void progresschange(WebKitWebView *view, gint p, Client *c);
static void request(SoupSession *s, SoupMessage *m, Client *c);
static void setcookie(char *name, char *val, char *dom, char *path, long exp);
static void setup(void);
static void showsearch(Client *c);
static void showurl(Client *c);
static void stop(Client *c);
static void titlechange(WebKitWebView* view, WebKitWebFrame* frame,
		const gchar* title, Client *c);
static void usage();
static void updatetitle(Client *c, const gchar *title);

void
cleanup(void) {
	while(clients)
		destroyclient(clients);
}

void
proccookies(SoupMessage *m, Client *c) {
	GSList *l;
	SoupCookie *co;
	long t;

	for (l = soup_cookies_from_response(m); l; l = l->next){
		co = (SoupCookie *)l->data;
		t = co->expires ?  soup_date_to_time_t(co->expires) : 0;
		setcookie(co->name, co->value, co->domain, co->value, t);
	}
	g_slist_free(l);
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
destroywin(GtkWidget* w, Client *c) {
	destroyclient(c);
}

void
die(char *str) {
	fputs(str, stderr);
	exit(EXIT_FAILURE);
}

void
download(WebKitDownload *o, GParamSpec *pspec, Client *c) {
	WebKitDownloadStatus status;

	status = webkit_download_get_status(c->download);
	if(status == WEBKIT_DOWNLOAD_STATUS_STARTED || status == WEBKIT_DOWNLOAD_STATUS_CREATED) {
		c->progress = (int)(webkit_download_get_progress(c->download)*100);
	}
	updatetitle(c, NULL);
}

gboolean
initdownload(WebKitWebView *view, WebKitDownload *o, Client *c) {
	const gchar *home, *filename;
	gchar *uri, *path, *html;

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
	html = g_strdup_printf("Download <b>%s</b>...", filename);
	webkit_web_view_load_html_string(c->view, html,
			webkit_download_get_uri(c->download));
	g_signal_connect(c->download, "notify::progress", G_CALLBACK(download), c);
	g_signal_connect(c->download, "notify::status", G_CALLBACK(download), c);
	webkit_download_start(c->download);
	updatetitle(c, filename);
	g_free(html);
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
keypress(GtkWidget* w, GdkEventKey *ev, Client *c) {
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
		case GDK_P:
			webkit_web_frame_print(webkit_web_view_get_main_frame(c->view));
			return TRUE;
		case GDK_p:
			gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), pasteurl, c);
		case GDK_y:
			gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), webkit_web_view_get_uri(c->view), -1);
			return TRUE;
		case GDK_r:
			webkit_web_view_reload(c->view);
			return TRUE;
		case GDK_R:
			webkit_web_view_reload_bypass_cache(c->view);
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
		case GDK_h:
			webkit_web_view_go_back(c->view);
			return TRUE;
		case GDK_l:
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
linkhover(WebKitWebView* page, const gchar* t, const gchar* l, Client *c) {
	if(l)
		gtk_window_set_title(GTK_WINDOW(c->win), l);
	else
		updatetitle(c, NULL);
}

void
loadcommit(WebKitWebView *view, WebKitWebFrame *f, Client *c) {
	gchar *uri;

	ignore_once = TRUE;
	uri = geturi(c);
	XChangeProperty(dpy, GDK_WINDOW_XID(GTK_WIDGET(c->win)->window), urlprop,
			XA_STRING, 8, PropModeReplace, (unsigned char *)uri,
			strlen(uri) + 1);
}

void
loadstart(WebKitWebView *view, WebKitWebFrame *f, Client *c) {
	c->progress = 0;
	updatetitle(c, NULL);
}

void
loadfile(Client *c, const gchar *f) {
	GIOChannel *chan = NULL;
	GError *e = NULL;
	GString *code;
	gchar *line, *uri;

	if(strcmp(f, "-") == 0) {
		chan = g_io_channel_unix_new(STDIN_FILENO);
		if (chan) {
			code = g_string_new("");
			while(g_io_channel_read_line(chan, &line, NULL, NULL,
						&e) == G_IO_STATUS_NORMAL) {
				g_string_append(code, line);
				g_free(line);
			}
			webkit_web_view_load_html_string(c->view, code->str,
					"file://.");
			g_io_channel_shutdown(chan, FALSE, NULL);
			g_string_free(code, TRUE);
		}
		uri = g_strdup("stdin");
	}
	else {
		uri = g_strdup_printf("file://%s", f);
		loaduri(c, uri);
	}
	updatetitle(c, uri);
	g_free(uri);
}

void
loaduri(Client *c, const gchar *uri) {
	gchar *u;
	u = g_strrstr(uri, "://") ? g_strdup(uri)
		: g_strdup_printf("http://%s", uri);
	webkit_web_view_load_uri(c->view, u);
	c->progress = 0;
	updatetitle(c, u);
	g_free(u);
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
	g_signal_connect_after(session, "request-started", G_CALLBACK(request), c);

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
newwindow(WebKitWebView  *v, WebKitWebFrame *f, Client *c) {
	Client *n = newclient();
	return n->view;
}

 
void
pasteurl(GtkClipboard *clipboard, const gchar *text, gpointer d) {
	if(text != NULL)
		loaduri((Client *) d, text);
}

GdkFilterReturn
processx(GdkXEvent *e, GdkEvent *event, gpointer d) {
	Client *c = (Client *)d;
	XPropertyEvent *ev;
	Atom adummy;
	int idummy;
	unsigned long ldummy;
	unsigned char *buf = NULL;

	if(((XEvent *)e)->type == PropertyNotify) {
		ev = &((XEvent *)e)->xproperty;
		if(ev->atom == urlprop && ev->state == PropertyNewValue) {
			if(ignore_once)
			       ignore_once = FALSE;
			else {
				XGetWindowProperty(dpy, ev->window, urlprop, 0L, BUFSIZ, False, XA_STRING,
					&adummy, &idummy, &ldummy, &ldummy, &buf);
				loaduri(c, (gchar *)buf);
				XFree(buf);
			}
			return GDK_FILTER_REMOVE;
		}
	}
	return GDK_FILTER_CONTINUE;
}

void
progresschange(WebKitWebView* view, gint p, Client *c) {
	c->progress = p;
	updatetitle(c, NULL);
}

void
request(SoupSession *s, SoupMessage *m, Client *c) {
	soup_message_add_header_handler(m, "got-headers", "Set-Cookie",
			G_CALLBACK(proccookies), c);
}

void
setcookie(char *name, char *val, char *dom, char *path, long exp) {
	printf("%s %s %s %s %li\n", name, val, dom, path, exp);
}

void
setup(void) {
	dpy = GDK_DISPLAY();
	session = webkit_get_default_session();
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
titlechange(WebKitWebView *v, WebKitWebFrame *f, const gchar *t, Client *c) {
	updatetitle(c, t);
}

void
usage() {
	fputs("surf - simple browser\n", stderr);
	die("usage: surf [-e] [-x] [-u uri] [-f file]\n");
}

void
updatetitle(Client *c, const char *title) {
	gchar *t;

	if(title) {
		if(c->title)
			g_free(c->title);
		c->title = g_strdup(title);
	}
	if(c->progress == 100)
		t = g_strdup(c->title);
	else
		t = g_strdup_printf("%s [%i%%]", c->title, c->progress);
	gtk_window_set_title(GTK_WINDOW(c->win), t);
	g_free(t);

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
