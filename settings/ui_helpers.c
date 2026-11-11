#include "settings.h"

bool run_cmd_capture(const char *cmd, char *out, size_t size) {
    if (!out || size < 2) return false;
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    size_t n = fread(out, 1, size - 1, fp);
    out[n] = '\0';
    int status = pclose(fp);
    if (status != 0 || n == 0) return false;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return true;
}

void run_cmd_async(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

GtkWidget *ui_make_badge(const char *icon_name, const char *color_class, const char *size_class) {
    GtkWidget *badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(badge, size_class ? size_class : "sidebar-badge");
    if (color_class) gtk_widget_add_css_class(badge, color_class);
    gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(badge, FALSE);
    gtk_widget_set_vexpand(badge, FALSE);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(badge), icon);
    return badge;
}

GtkWidget *ui_page_container(const char *title, const char *lead) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 24);

    GtkWidget *h = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(h), 0.0f);
    gtk_widget_add_css_class(h, "headline");
    gtk_box_append(GTK_BOX(box), h);

    if (lead) {
        GtkWidget *l = gtk_label_new(lead);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_widget_add_css_class(l, "lead");
        gtk_box_append(GTK_BOX(box), l);
    }
    return box;
}

GtkWidget *ui_card(GtkWidget *parent, const char *section_title) {
    if (section_title) {
        GtkWidget *lbl = gtk_label_new(section_title);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_add_css_class(lbl, "section");
        gtk_box_append(GTK_BOX(parent), lbl);
    }
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    gtk_box_append(GTK_BOX(parent), card);
    return card;
}

GtkWidget *ui_add_row(GtkWidget *card, const char *title, const char *subtitle,
                      GtkWidget *control, const char *icon_name, const char *badge_class) {
    if (gtk_widget_get_first_child(card) != NULL) {
        GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(sep, "mac-sep");
        gtk_box_append(GTK_BOX(card), sep);
    }

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "row");

    if (icon_name) {
        GtkWidget *badge = ui_make_badge(icon_name, badge_class ? badge_class : "badge-wifi", "row-badge");
        gtk_box_append(GTK_BOX(row), badge);
    }

    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(label_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label_box, TRUE);

    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(t), TRUE);
    gtk_widget_add_css_class(t, "title");
    gtk_box_append(GTK_BOX(label_box), t);

    if (subtitle) {
        GtkWidget *s = gtk_label_new(subtitle);
        gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_widget_add_css_class(s, "subtitle");
        gtk_box_append(GTK_BOX(label_box), s);
    }
    gtk_box_append(GTK_BOX(row), label_box);

    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), control);
    }

    gtk_box_append(GTK_BOX(card), row);
    return row;
}

typedef struct {
    void (*cb)(bool, gpointer);
    gpointer data;
} switch_closure_t;

static void free_closure(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void on_switch_notify(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    switch_closure_t *cl = user_data;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(obj));
    if (cl && cl->cb) {
        cl->cb(active, cl->data);
    }
    settings_schedule_save();
}

GtkWidget *ui_switch(bool initial, void (*callback)(bool active, gpointer user_data), gpointer user_data) {
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), initial);
    if (callback) {
        switch_closure_t *cl = g_new0(switch_closure_t, 1);
        cl->cb = callback;
        cl->data = user_data;
        g_signal_connect_data(sw, "notify::active", G_CALLBACK(on_switch_notify), cl, free_closure, 0);
    }
    return sw;
}

typedef struct {
    void (*cb)(double, gpointer);
    gpointer data;
} scale_closure_t;

static void on_scale_value_changed(GtkRange *range, gpointer user_data) {
    scale_closure_t *cl = user_data;
    double val = gtk_range_get_value(range);
    if (cl && cl->cb) {
        cl->cb(val, cl->data);
    }
    settings_schedule_save();
}

GtkWidget *ui_scale(double val, double min, double max, double step, int digits,
                    void (*callback)(double value, gpointer user_data), gpointer user_data) {
    GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_scale_set_digits(GTK_SCALE(sc), digits);
    gtk_scale_set_draw_value(GTK_SCALE(sc), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(sc), GTK_POS_RIGHT);
    gtk_widget_set_size_request(sc, 140, -1);
    gtk_range_set_value(GTK_RANGE(sc), val);

    if (callback) {
        scale_closure_t *cl = g_new0(scale_closure_t, 1);
        cl->cb = callback;
        cl->data = user_data;
        g_signal_connect_data(sc, "value-changed", G_CALLBACK(on_scale_value_changed), cl, free_closure, 0);
    }
    return sc;
}

typedef struct {
    void (*cb)(int, gpointer);
    gpointer data;
} dropdown_closure_t;

static void on_dropdown_notify(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    dropdown_closure_t *cl = user_data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (cl && cl->cb) {
        cl->cb((int)idx, cl->data);
    }
    settings_schedule_save();
}

GtkWidget *ui_dropdown(const char *const *labels, int count, int selected,
                       void (*callback)(int selected_idx, gpointer user_data), gpointer user_data) {
    (void)count;
    GtkWidget *dd = gtk_drop_down_new_from_strings(labels);
    if (selected >= 0) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), selected);
    }
    if (callback) {
        dropdown_closure_t *cl = g_new0(dropdown_closure_t, 1);
        cl->cb = callback;
        cl->data = user_data;
        g_signal_connect_data(dd, "notify::selected", G_CALLBACK(on_dropdown_notify), cl, free_closure, 0);
    }
    return dd;
}

typedef struct {
    void (*cb)(const char *, gpointer);
    gpointer data;
} entry_closure_t;

static void on_entry_changed(GtkEditable *editable, gpointer user_data) {
    entry_closure_t *cl = user_data;
    const char *text = gtk_editable_get_text(editable);
    if (cl && cl->cb) {
        cl->cb(text, cl->data);
    }
    settings_schedule_save();
}

GtkWidget *ui_entry(const char *text, const char *placeholder,
                    void (*callback)(const char *text, gpointer user_data), gpointer user_data) {
    GtkWidget *entry = gtk_entry_new();
    if (text) gtk_editable_set_text(GTK_EDITABLE(entry), text);
    if (placeholder) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_size_request(entry, 160, -1);

    if (callback) {
        entry_closure_t *cl = g_new0(entry_closure_t, 1);
        cl->cb = callback;
        cl->data = user_data;
        g_signal_connect_data(entry, "changed", G_CALLBACK(on_entry_changed), cl, free_closure, 0);
    }
    return entry;
}
