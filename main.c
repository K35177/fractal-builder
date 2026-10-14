#include <gtk/gtk.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITERATIONS 240
#define MIN_SELECTION 8.0

typedef struct {
    double left, right, bottom, top;
} Viewport;

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;
    GtkWidget *type_dropdown;
    GtkWidget *julia_controls;
    GtkWidget *real_scale;
    GtkWidget *imag_scale;
    GtkWidget *constant_label;
    GtkWidget *plane_label;
    GtkWidget *cursor_label;
    GtkWidget *status_label;

    cairo_surface_t *surface;
    Viewport view;
    int surface_width;
    int surface_height;
    guint redraw_timeout;
    gboolean viewport_initialized;
    gboolean selecting;
    double selection_x0, selection_y0;
    double selection_x1, selection_y1;
    double pointer_x, pointer_y;
} App;

static gboolean is_julia(const App *app)
{
    return gtk_drop_down_get_selected(GTK_DROP_DOWN(app->type_dropdown)) == 1;
}

static double map_value(double value, double source_a, double source_b,
                        double target_a, double target_b)
{
    return target_a + (value - source_a) / (source_b - source_a)
                    * (target_b - target_a);
}

static void default_viewport(App *app, int width, int height)
{
    const double horizontal = is_julia(app) ? 3.6 : 3.0;
    const double center = is_julia(app) ? 0.0 : -0.5;
    const double aspect = width > 0 ? (double)height / width : 0.68;

    app->view.left = center - horizontal / 2.0;
    app->view.right = center + horizontal / 2.0;
    app->view.bottom = -horizontal * aspect / 2.0;
    app->view.top = horizontal * aspect / 2.0;
    app->viewport_initialized = TRUE;
}

static void set_status(App *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text);
}

static void update_plane_label(App *app)
{
    char text[180];
    g_snprintf(text, sizeof text,
               "Вещественная: [%.8g; %.8g]   Мнимая: [%.8g; %.8g]",
               app->view.left, app->view.right,
               app->view.bottom, app->view.top);
    gtk_label_set_text(GTK_LABEL(app->plane_label), text);
}

static void pixel_to_complex(const App *app, double x, double y,
                             double *real, double *imaginary)
{
    int width = gtk_widget_get_width(app->drawing_area);
    int height = gtk_widget_get_height(app->drawing_area);
    *real = map_value(x, 0.0, width, app->view.left, app->view.right);
    *imaginary = map_value(y, 0.0, height, app->view.top, app->view.bottom);
}

/* Главная формула проекта: z(n + 1) = z(n)^2 + c. */
static int escape_iterations(double zr, double zi, double cr, double ci)
{
    int iteration = 0;
    while (iteration < MAX_ITERATIONS && zr * zr + zi * zi <= 4.0) {
        double next_real = zr * zr - zi * zi + cr;
        zi = 2.0 * zr * zi + ci;
        zr = next_real;
        ++iteration;
    }
    return iteration;
}

/* Яркая циклическая палитра. */
static uint32_t color_for(int iteration)
{
    if (iteration == MAX_ITERATIONS)
        return 0x00070910;

    double phase = iteration * 0.105;
    double brightness = 0.35 + 0.65 * (1.0 - exp(-0.055 * iteration));
    unsigned red = (unsigned)((128 + 127 * cos(phase)) * brightness);
    unsigned green = (unsigned)((128 + 127 * cos(phase + 2.094)) * brightness);
    unsigned blue = (unsigned)((128 + 127 * cos(phase + 4.188)) * brightness);
    return (red << 16) | (green << 8) | blue;
}

static void render_fractal(App *app)
{
    int width = gtk_widget_get_width(app->drawing_area);
    int height = gtk_widget_get_height(app->drawing_area);
    if (width < 2 || height < 2)
        return;

    if (!app->viewport_initialized)
        default_viewport(app, width, height);

    if (app->surface)
        cairo_surface_destroy(app->surface);
    app->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if (cairo_surface_status(app->surface) != CAIRO_STATUS_SUCCESS) {
        set_status(app, "Ошибка создания изображения");
        return;
    }

    unsigned char *data = cairo_image_surface_get_data(app->surface);
    int stride = cairo_image_surface_get_stride(app->surface);
    gboolean julia = is_julia(app);
    double fixed_cr = gtk_range_get_value(GTK_RANGE(app->real_scale));
    double fixed_ci = gtk_range_get_value(GTK_RANGE(app->imag_scale));

    gtk_widget_set_cursor_from_name(app->drawing_area, "wait");
    for (int py = 0; py < height; ++py) {
        uint32_t *row = (uint32_t *)(data + py * stride);
        double point_i = map_value(py, 0, height - 1,
                                   app->view.top, app->view.bottom);
        for (int px = 0; px < width; ++px) {
            double point_r = map_value(px, 0, width - 1,
                                       app->view.left, app->view.right);
            double zr = julia ? point_r : 0.0;
            double zi = julia ? point_i : 0.0;
            double cr = julia ? fixed_cr : point_r;
            double ci = julia ? fixed_ci : point_i;
            row[px] = color_for(escape_iterations(zr, zi, cr, ci));
        }
    }
    cairo_surface_mark_dirty(app->surface);
    app->surface_width = width;
    app->surface_height = height;
    update_plane_label(app);
    gtk_widget_queue_draw(app->drawing_area);
    gtk_widget_set_cursor_from_name(app->drawing_area, "crosshair");

    char status[180];
    if (julia) {
        g_snprintf(status, sizeof status,
                   "Готово • Жюлиа • c = %.3f %+.3fi",
                   fixed_cr, fixed_ci);
    } else {
        g_snprintf(status, sizeof status, "Готово • множество Мандельброта");
    }
    set_status(app, status);
}

static gboolean redraw_timeout_cb(gpointer user_data)
{
    App *app = user_data;
    app->redraw_timeout = 0;
    render_fractal(app);
    return G_SOURCE_REMOVE;
}

static void schedule_redraw(App *app)
{
    if (app->redraw_timeout)
        g_source_remove(app->redraw_timeout);
    app->redraw_timeout = g_timeout_add(55, redraw_timeout_cb, app);
}

static void redraw_now(App *app)
{
    if (app->redraw_timeout) {
        g_source_remove(app->redraw_timeout);
        app->redraw_timeout = 0;
    }
    render_fractal(app);
}

static void draw_cb(GtkDrawingArea *area, cairo_t *cr,
                    int width, int height, gpointer user_data)
{
    App *app = user_data;
    (void)area;
    cairo_set_source_rgb(cr, 0.02, 0.03, 0.045);
    cairo_paint(cr);

    if (app->surface) {
        cairo_save(cr);
        cairo_scale(cr, (double)width / app->surface_width,
                    (double)height / app->surface_height);
        cairo_set_source_surface(cr, app->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    if (app->selecting) {
        double x = fmin(app->selection_x0, app->selection_x1);
        double y = fmin(app->selection_y0, app->selection_y1);
        double w = fabs(app->selection_x1 - app->selection_x0);
        double h = fabs(app->selection_y1 - app->selection_y0);

        /* Только контур и угловые метки — без закрывающей фрактал заливки. */
        cairo_rectangle(cr, x, y, w, h);
        cairo_set_source_rgb(cr, 1.0, 0.47, 0.78);
        cairo_set_line_width(cr, 1.5);
        const double dashes[] = {5.0, 5.0};
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_stroke(cr);

        cairo_set_dash(cr, NULL, 0, 0);
        cairo_set_source_rgb(cr, 0.55, 0.91, 0.99);
        cairo_set_line_width(cr, 3.0);
        double mark = fmin(16.0, fmin(w, h) / 3.0);
        cairo_move_to(cr, x, y + mark); cairo_line_to(cr, x, y); cairo_line_to(cr, x + mark, y);
        cairo_move_to(cr, x + w - mark, y); cairo_line_to(cr, x + w, y); cairo_line_to(cr, x + w, y + mark);
        cairo_move_to(cr, x, y + h - mark); cairo_line_to(cr, x, y + h); cairo_line_to(cr, x + mark, y + h);
        cairo_move_to(cr, x + w - mark, y + h); cairo_line_to(cr, x + w, y + h); cairo_line_to(cr, x + w, y + h - mark);
        cairo_stroke(cr);
    }

    if (width != app->surface_width || height != app->surface_height)
        schedule_redraw(app);
}

static void update_selection(App *app, double x, double y)
{
    int width = gtk_widget_get_width(app->drawing_area);
    int height = gtk_widget_get_height(app->drawing_area);
    app->selection_x1 = fmax(0, fmin(x, width));
    app->selection_y1 = fmax(0, fmin(y, height));
}

static void drag_begin_cb(GtkGestureDrag *gesture, double x, double y,
                          gpointer user_data)
{
    App *app = user_data;
    (void)gesture;
    app->selecting = TRUE;
    app->selection_x0 = app->selection_x1 = x;
    app->selection_y0 = app->selection_y1 = y;
    gtk_widget_queue_draw(app->drawing_area);
}

static void drag_update_cb(GtkGestureDrag *gesture, double offset_x,
                           double offset_y, gpointer user_data)
{
    App *app = user_data;
    (void)gesture;
    update_selection(app, app->selection_x0 + offset_x,
                     app->selection_y0 + offset_y);
    gtk_widget_queue_draw(app->drawing_area);
}

static void drag_end_cb(GtkGestureDrag *gesture, double offset_x,
                        double offset_y, gpointer user_data)
{
    App *app = user_data;
    (void)gesture;
    update_selection(app, app->selection_x0 + offset_x,
                     app->selection_y0 + offset_y);
    app->selecting = FALSE;

    double x0 = fmin(app->selection_x0, app->selection_x1);
    double x1 = fmax(app->selection_x0, app->selection_x1);
    double y0 = fmin(app->selection_y0, app->selection_y1);
    double y1 = fmax(app->selection_y0, app->selection_y1);
    if (x1 - x0 < MIN_SELECTION || y1 - y0 < MIN_SELECTION) {
        gtk_widget_queue_draw(app->drawing_area);
        return;
    }

    double left, right, top, bottom;
    pixel_to_complex(app, x0, y0, &left, &top);
    pixel_to_complex(app, x1, y1, &right, &bottom);
    if (left == right || top == bottom) {
        set_status(app, "Слишком маленькая область");
        return;
    }
    /* Расширяем выбранную область до пропорций холста, не искажая фрактал. */
    double canvas_ratio = (double)gtk_widget_get_width(app->drawing_area) /
                          gtk_widget_get_height(app->drawing_area);
    double selected_ratio = (right - left) / (top - bottom);
    if (selected_ratio > canvas_ratio) {
        double center = (top + bottom) / 2.0;
        double new_height = (right - left) / canvas_ratio;
        top = center + new_height / 2.0;
        bottom = center - new_height / 2.0;
    } else {
        double center = (left + right) / 2.0;
        double new_width = (top - bottom) * canvas_ratio;
        left = center - new_width / 2.0;
        right = center + new_width / 2.0;
    }
    app->view = (Viewport){left, right, bottom, top};
    redraw_now(app);
}

static void motion_cb(GtkEventControllerMotion *controller, double x,
                      double y, gpointer user_data)
{
    App *app = user_data;
    (void)controller;
    app->pointer_x = x;
    app->pointer_y = y;
    if (!app->viewport_initialized)
        return;
    double real, imaginary;
    pixel_to_complex(app, x, y, &real, &imaginary);
    char text[100];
    g_snprintf(text, sizeof text, "Курсор: %.10g %+.10gi", real, imaginary);
    gtk_label_set_text(GTK_LABEL(app->cursor_label), text);
}

static gboolean scroll_cb(GtkEventControllerScroll *controller,
                          double dx, double dy, gpointer user_data)
{
    App *app = user_data;
    (void)controller;
    (void)dx;
    if (dy == 0 || !app->viewport_initialized)
        return FALSE;

    double center_r, center_i;
    pixel_to_complex(app, app->pointer_x, app->pointer_y,
                     &center_r, &center_i);
    double factor = dy > 0 ? 1.28 : 1.0 / 1.28;
    app->view.left = center_r + (app->view.left - center_r) * factor;
    app->view.right = center_r + (app->view.right - center_r) * factor;
    app->view.bottom = center_i + (app->view.bottom - center_i) * factor;
    app->view.top = center_i + (app->view.top - center_i) * factor;
    schedule_redraw(app);
    return TRUE;
}

static void update_constant_label(App *app)
{
    double real = gtk_range_get_value(GTK_RANGE(app->real_scale));
    double imaginary = gtk_range_get_value(GTK_RANGE(app->imag_scale));
    char text[80];
    g_snprintf(text, sizeof text, "c = %.3f %+.3fi", real, imaginary);
    gtk_label_set_text(GTK_LABEL(app->constant_label), text);
}

static void scale_changed_cb(GtkRange *range, gpointer user_data)
{
    App *app = user_data;
    (void)range;
    update_constant_label(app);
    if (is_julia(app)) {
        set_status(app, "Изменение c • выполняется отрисовка…");
        schedule_redraw(app);
    }
}

static void type_changed_cb(GtkDropDown *dropdown, GParamSpec *spec,
                            gpointer user_data)
{
    App *app = user_data;
    (void)dropdown;
    (void)spec;
    gtk_widget_set_sensitive(app->julia_controls, is_julia(app));
    app->viewport_initialized = FALSE;
    if (is_julia(app))
        set_status(app, "Режим Жюлиа • измените константу c");
    else
        set_status(app, "Режим Мандельброта • c задаётся точкой");
    schedule_redraw(app);
}

static void redraw_clicked_cb(GtkButton *button, gpointer user_data)
{
    (void)button;
    redraw_now(user_data);
}

static void reset_clicked_cb(GtkButton *button, gpointer user_data)
{
    App *app = user_data;
    (void)button;
    app->viewport_initialized = FALSE;
    redraw_now(app);
}

static GtkWidget *make_ascii_line(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_add_css_class(label, "ascii-line");
    return label;
}

static GtkWidget *make_scale_row(App *app, const char *title,
                                 GtkWidget **scale_out, double value)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *label = gtk_label_new(title);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                -2.0, 2.0, 0.01);
    gtk_scale_set_digits(GTK_SCALE(scale), 3);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_range_set_value(GTK_RANGE(scale), value);
    gtk_box_append(GTK_BOX(box), scale);
    g_signal_connect(scale, "value-changed", G_CALLBACK(scale_changed_cb), app);
    *scale_out = scale;
    return box;
}

static void install_css(void)
{
    const char *css =
        "* { font-family: 'JetBrains Mono', 'DejaVu Sans Mono', monospace; }"
        "window { background: #0b0e14; color: #cdd6f4; }"
        "headerbar { background: #11151e; color: #cdd6f4;"
        "  border-bottom: 1px solid #343b4f; box-shadow: none; }"
        ".sidebar { background: #080b10; border: 1px solid #3f6b54;"
        "  border-radius: 5px; padding: 12px; }"
        ".fractal-frame { background: #070910; border: 1px solid #3b4261;"
        "  border-radius: 8px; }"
        ".caption { font-size: 11px; font-weight: 700; color: #a6adc8; }"
        ".ascii-line { color: #7aa2f7; font-size: 11px; font-weight: 800; }"
        ".title { color: #f7768e; font-size: 12px; font-weight: 800; }"
        ".hint { color: #7f849c; font-size: 10px; }"
        ".status { color: #89dceb; font-size: 11px; }"
        ".constant { color: #f9e2af; font-size: 12px; font-weight: 800; }"
        ".info { background: rgba(17,21,30,0.92); color: #89dceb;"
        "  border: 1px solid #585b70; border-radius: 5px; padding: 6px 9px; }"
        ".primary { background: #745bba; color: #ffffff; font-weight: 800; }"
        ".primary:hover { background: #8d71d6; }"
        ".julia-box { background: #0d1017; border-left: 1px solid #bb9af7;"
        "  border-right: 1px solid #bb9af7; padding: 8px 10px; }"
        "button, dropdown { border-radius: 5px; }"
        "scale highlight { background: #cba6f7; }"
        "scale slider { background: #89dceb; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void activate(GtkApplication *application, gpointer user_data)
{
    App *app = user_data;
    install_css();

    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "Фракталы");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1320, 780);
    gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);

    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *header_title = gtk_label_new("Фракталы  •  C / GTK 4");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), header_title);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(root, 10);
    gtk_widget_set_margin_end(root, 10);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 10);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 11);
    gtk_widget_set_size_request(sidebar, 260, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_box_append(GTK_BOX(root), sidebar);

    GtkWidget *title = gtk_label_new("┌─ 1 управление ─────────────┐");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_add_css_class(title, "title");
    gtk_box_append(GTK_BOX(sidebar), title);
    gtk_box_append(GTK_BOX(sidebar),
                   make_ascii_line("├─ тип фрактала ─────────────┤"));
    const char *types[] = {"Множество Мандельброта", "Множество Жюлиа", NULL};
    app->type_dropdown = gtk_drop_down_new_from_strings(types);
    gtk_box_append(GTK_BOX(sidebar), app->type_dropdown);

    app->julia_controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(app->julia_controls, "julia-box");
    gtk_box_append(GTK_BOX(app->julia_controls),
                   make_ascii_line("├─ константа c = a + bi ─────┤"));
    GtkWidget *real_row = make_scale_row(app, "Действительная часть",
                                         &app->real_scale, -0.7);
    GtkWidget *imag_row = make_scale_row(app, "Мнимая часть",
                                         &app->imag_scale, 0.27);
    gtk_box_append(GTK_BOX(app->julia_controls), real_row);
    gtk_box_append(GTK_BOX(app->julia_controls), imag_row);
    app->constant_label = gtk_label_new(NULL);
    gtk_widget_set_halign(app->constant_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(app->constant_label, "constant");
    gtk_box_append(GTK_BOX(app->julia_controls), app->constant_label);
    update_constant_label(app);
    gtk_widget_set_sensitive(app->julia_controls, FALSE);
    gtk_box_append(GTK_BOX(sidebar), app->julia_controls);

    gtk_box_append(GTK_BOX(sidebar),
                   make_ascii_line("├─ команды ──────────────────┤"));

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *draw_button = gtk_button_new_with_label("[ отрисовать ]");
    GtkWidget *reset_button = gtk_button_new_with_label("[ сбросить ]");
    gtk_widget_set_hexpand(draw_button, TRUE);
    gtk_widget_set_hexpand(reset_button, TRUE);
    gtk_widget_add_css_class(draw_button, "primary");
    gtk_box_append(GTK_BOX(buttons), draw_button);
    gtk_box_append(GTK_BOX(buttons), reset_button);
    gtk_box_append(GTK_BOX(sidebar), buttons);

    app->status_label = gtk_label_new("Система готова");
    gtk_label_set_wrap(GTK_LABEL(app->status_label), TRUE);
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(app->status_label, "status");
    gtk_box_append(GTK_BOX(sidebar), app->status_label);
    GtkWidget *ascii_bottom = make_ascii_line("└────────────────────────────┘");
    gtk_widget_set_valign(ascii_bottom, GTK_ALIGN_END);
    gtk_widget_set_vexpand(ascii_bottom, TRUE);
    gtk_box_append(GTK_BOX(sidebar), ascii_bottom);

    GtkWidget *frame = gtk_overlay_new();
    gtk_widget_set_hexpand(frame, TRUE);
    gtk_widget_set_vexpand(frame, TRUE);
    gtk_widget_add_css_class(frame, "fractal-frame");
    gtk_box_append(GTK_BOX(root), frame);

    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(app->drawing_area, TRUE);
    gtk_widget_set_vexpand(app->drawing_area, TRUE);
    gtk_widget_set_focusable(app->drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->drawing_area),
                                   draw_cb, app, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(frame), app->drawing_area);

    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_halign(info_box, GTK_ALIGN_START);
    gtk_widget_set_valign(info_box, GTK_ALIGN_START);
    gtk_widget_set_margin_start(info_box, 14);
    gtk_widget_set_margin_top(info_box, 14);
    gtk_widget_set_can_target(info_box, FALSE);
    app->plane_label = gtk_label_new("Комплексная плоскость");
    app->cursor_label = gtk_label_new("Курсор: —");
    gtk_widget_set_halign(app->plane_label, GTK_ALIGN_START);
    gtk_widget_set_halign(app->cursor_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(info_box, "info");
    gtk_box_append(GTK_BOX(info_box), app->plane_label);
    gtk_box_append(GTK_BOX(info_box), app->cursor_label);
    gtk_overlay_add_overlay(GTK_OVERLAY(frame), info_box);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin_cb), app);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update_cb), app);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end_cb), app);
    gtk_widget_add_controller(app->drawing_area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(motion_cb), app);
    gtk_widget_add_controller(app->drawing_area, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_cb), app);
    gtk_widget_add_controller(app->drawing_area, scroll);

    g_signal_connect(app->type_dropdown, "notify::selected",
                     G_CALLBACK(type_changed_cb), app);
    g_signal_connect(draw_button, "clicked", G_CALLBACK(redraw_clicked_cb), app);
    g_signal_connect(reset_button, "clicked", G_CALLBACK(reset_clicked_cb), app);

    gtk_window_present(GTK_WINDOW(app->window));
    schedule_redraw(app);
}

static int self_test(void)
{
    int inside = escape_iterations(0, 0, 0, 0);
    int outside = escape_iterations(0, 0, 2, 2);
    if (inside != MAX_ITERATIONS || outside >= 5) {
        fputs("Ошибка самопроверки вычислений.\n", stderr);
        return EXIT_FAILURE;
    }
    puts("Самопроверка пройдена: вычисление фрактала работает.");
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();

    App *app_state = g_new0(App, 1);
    GtkApplication *application = gtk_application_new(
        "ru.school.FractalBuilder", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), app_state);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    if (app_state->redraw_timeout)
        g_source_remove(app_state->redraw_timeout);
    if (app_state->surface)
        cairo_surface_destroy(app_state->surface);
    g_object_unref(application);
    g_free(app_state);
    return status;
}
