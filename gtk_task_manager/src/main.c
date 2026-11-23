
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

enum {
    COL_NAME,
    COL_PID,
    COL_MEMORY,
    COL_STATUS,
    COL_COUNT
};

typedef struct {
    GtkWidget *window;
    GtkWidget *search;
    GtkWidget *tree;
    GtkWidget *cpu_label;
    GtkWidget *memory_label;
    GtkWidget *status_label;

    GtkListStore *store;
    GtkTreeModelFilter *filter;

    guint timer_id;

    ULONGLONG last_idle;
    ULONGLONG last_kernel;
    ULONGLONG last_user;

    gboolean have_cpu_sample;
    gboolean debug_privilege_enabled;

} App;


/* =========================================
   MEMORY FORMATTING
========================================= */

static gchar *format_bytes(SIZE_T bytes) {

    const gchar *units[] = {
        "B", "KB", "MB", "GB", "TB"
    };

    double value = (double)bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }

    return g_strdup_printf(
        unit == 0 ? "%.0f %s" : "%.1f %s",
        value,
        units[unit]
    );
}


/* =========================================
   ENABLE DEBUG PRIVILEGE
========================================= */

static gboolean enable_debug_privilege(void) {

    HANDLE token = NULL;

    if (!OpenProcessToken(
        GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        &token
    )) {
        return FALSE;
    }

    LUID luid;

    if (!LookupPrivilegeValueA(
        NULL,
        SE_DEBUG_NAME,
        &luid
    )) {
        CloseHandle(token);
        return FALSE;
    }

    TOKEN_PRIVILEGES privileges;

    memset(&privileges, 0, sizeof(privileges));

    privileges.PrivilegeCount = 1;

    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(ERROR_SUCCESS);

    BOOL result = AdjustTokenPrivileges(
        token,
        FALSE,
        &privileges,
        sizeof(privileges),
        NULL,
        NULL
    );

    DWORD error = GetLastError();

    CloseHandle(token);

    return result && error != ERROR_NOT_ALL_ASSIGNED;
}


/* =========================================
   READ PROCESS MEMORY
========================================= */

static HANDLE open_process_for_memory(
    DWORD pid,
    DWORD *error_code
) {
    *error_code = ERROR_SUCCESS;

    // First attempt
    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION,
        FALSE,
        pid
    );

    if (process != NULL) {
        return process;
    }

    *error_code = GetLastError();

    // Second attempt with limited access
    process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid
    );

    if (process != NULL) {
        return process;
    }

    *error_code = GetLastError();

    return NULL;
}



static gboolean read_process_memory(
    DWORD pid,
    SIZE_T *working_set,
    DWORD *error_code
) {
    *working_set = 0;

    HANDLE process = open_process_for_memory(pid, error_code);

    if (!process) {
        return FALSE;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);

    BOOL success = GetProcessMemoryInfo(
        process,
        (PROCESS_MEMORY_COUNTERS *)&pmc,
        sizeof(pmc)
    );

    if (success) {
        *working_set = pmc.WorkingSetSize;
        *error_code = ERROR_SUCCESS;
    } else {
        *error_code = GetLastError();
    }

    CloseHandle(process);

    return success ? TRUE : FALSE;
}

/* =========================================
   CPU USAGE
========================================= */

static double read_cpu_usage(App *app) {

    FILETIME idle_ft;
    FILETIME kernel_ft;
    FILETIME user_ft;

    if (!GetSystemTimes(
        &idle_ft,
        &kernel_ft,
        &user_ft
    )) {
        return -1.0;
    }

    ULARGE_INTEGER idle;
    ULARGE_INTEGER kernel;
    ULARGE_INTEGER user;

    idle.LowPart = idle_ft.dwLowDateTime;
    idle.HighPart = idle_ft.dwHighDateTime;

    kernel.LowPart = kernel_ft.dwLowDateTime;
    kernel.HighPart = kernel_ft.dwHighDateTime;

    user.LowPart = user_ft.dwLowDateTime;
    user.HighPart = user_ft.dwHighDateTime;

    if (!app->have_cpu_sample) {

        app->last_idle = idle.QuadPart;
        app->last_kernel = kernel.QuadPart;
        app->last_user = user.QuadPart;

        app->have_cpu_sample = TRUE;

        return 0.0;
    }

    ULONGLONG idle_delta =
        idle.QuadPart - app->last_idle;

    ULONGLONG kernel_delta =
        kernel.QuadPart - app->last_kernel;

    ULONGLONG user_delta =
        user.QuadPart - app->last_user;

    ULONGLONG total =
        kernel_delta + user_delta;

    app->last_idle = idle.QuadPart;
    app->last_kernel = kernel.QuadPart;
    app->last_user = user.QuadPart;

    if (total == 0) {
        return 0.0;
    }

    return 100.0 *
        (double)(total - idle_delta) /
        (double)total;
}


/* =========================================
   SYSTEM STATISTICS
========================================= */

static void update_system_stats(App *app) {

    double cpu = read_cpu_usage(app);

    MEMORYSTATUSEX mem;

    memset(&mem, 0, sizeof(mem));

    mem.dwLength = sizeof(mem);

    if (cpu >= 0) {

        gchar *cpu_text = g_strdup_printf(
            "CPU  %.1f%%",
            cpu
        );

        gtk_label_set_text(
            GTK_LABEL(app->cpu_label),
            cpu_text
        );

        g_free(cpu_text);
    }

    if (GlobalMemoryStatusEx(&mem)) {

        gchar *used = format_bytes(
            (SIZE_T)(mem.ullTotalPhys - mem.ullAvailPhys)
        );

        gchar *total = format_bytes(
            (SIZE_T)mem.ullTotalPhys
        );

        gchar *label = g_strdup_printf(
            "Memory  %s / %s  (%lu%%)",
            used,
            total,
            (unsigned long)mem.dwMemoryLoad
        );

        gtk_label_set_text(
            GTK_LABEL(app->memory_label),
            label
        );

        g_free(used);
        g_free(total);
        g_free(label);
    }
}


/* =========================================
   PROCESS STATUS
========================================= */

static gchar *get_process_status(
    DWORD pid,
    gboolean memory_available,
    DWORD error_code
) {

    if (pid == 0) {
        return g_strdup("System process");
    }

    if (memory_available) {
        return g_strdup("Running");
    }

    if (error_code == ERROR_ACCESS_DENIED) {
        return g_strdup("Access denied");
    }

    if (error_code == ERROR_INVALID_PARAMETER) {
        return g_strdup("Exited / unavailable");
    }

    return g_strdup_printf(
        "Unavailable (%lu)",
        (unsigned long)error_code
    );
}


/* =========================================
   REFRESH PROCESS LIST
========================================= */

static void refresh_processes(App *app) {

    gtk_list_store_clear(app->store);

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    );

    if (snapshot == INVALID_HANDLE_VALUE) {

        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Unable to read process list."
        );

        return;
    }

    PROCESSENTRY32W entry;

    memset(&entry, 0, sizeof(entry));

    entry.dwSize = sizeof(entry);

    int count = 0;

    if (Process32FirstW(snapshot, &entry)) {

        do {

            DWORD pid = entry.th32ProcessID;

            gchar *name = g_utf16_to_utf8(
                (const gunichar2 *)entry.szExeFile,
                -1,
                NULL,
                NULL,
                NULL
            );

            if (!name) {
                name = g_strdup("Unknown");
            }

            /* Read memory */

            SIZE_T working_set = 0;

            DWORD error_code = ERROR_SUCCESS;

            gboolean memory_available =
                read_process_memory(
                    pid,
                    &working_set,
                    &error_code
                );

            /* Format memory */

            gchar *memory;

            if (memory_available) {

                memory = format_bytes(working_set);

            } else {

                memory = g_strdup("—");
            }

            /* Determine status */

            gchar *process_status = get_process_status(
                pid,
                memory_available,
                error_code
            );

            /* Add process to GTK table */

            GtkTreeIter iter;

            gtk_list_store_append(
                app->store,
                &iter
            );

            gtk_list_store_set(
                app->store,
                &iter,

                COL_NAME, name,
                COL_PID, (guint)pid,
                COL_MEMORY, memory,
                COL_STATUS, process_status,

                -1
            );

            g_free(name);
            g_free(memory);
            g_free(process_status);

            count++;

        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    /* Update footer */

    gchar *status = g_strdup_printf(
        "%d processes | Debug privilege: %s",
        count,
        app->debug_privilege_enabled
            ? "Enabled"
            : "Not enabled"
    );

    gtk_label_set_text(
        GTK_LABEL(app->status_label),
        status
    );

    g_free(status);

    /* Update CPU and RAM */

    update_system_stats(app);
}


/* =========================================
   SEARCH FILTER
========================================= */

static gboolean filter_process(
    GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer data
) {

    App *app = data;

    const gchar *query = gtk_entry_get_text(
        GTK_ENTRY(app->search)
    );

    if (!query || !*query) {
        return TRUE;
    }

    gchar *name = NULL;

    guint pid = 0;

    gtk_tree_model_get(
        model,
        iter,
        COL_NAME, &name,
        COL_PID, &pid,
        -1
    );

    gchar *pid_text = g_strdup_printf(
        "%u",
        pid
    );

    gchar *name_lower = g_utf8_strdown(
        name ? name : "",
        -1
    );

    gchar *query_lower = g_utf8_strdown(
        query,
        -1
    );

    gboolean match =
        strstr(name_lower, query_lower) != NULL ||
        strstr(pid_text, query) != NULL;

    g_free(name);
    g_free(pid_text);
    g_free(name_lower);
    g_free(query_lower);

    return match;
}


static void on_search_changed(
    GtkEditable *editable,
    gpointer data
) {

    App *app = data;

    gtk_tree_model_filter_refilter(
        app->filter
    );
}


/* =========================================
   GET SELECTED PROCESS
========================================= */

static gboolean get_selected_process(
    App *app,
    guint *pid,
    gchar **name
) {

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(
            GTK_TREE_VIEW(app->tree)
        );

    GtkTreeModel *model;

    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(
        selection,
        &model,
        &iter
    )) {
        return FALSE;
    }

    gtk_tree_model_get(
        model,
        &iter,
        COL_PID, pid,
        COL_NAME, name,
        -1
    );

    return TRUE;
}


/* =========================================
   END TASK
========================================= */

static void on_end_task(
    GtkButton *button,
    gpointer data
) {

    (void)button;

    App *app = data;

    guint pid = 0;

    gchar *name = NULL;

    if (!get_selected_process(app, &pid, &name)) {

        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Select a process first."
        );

        return;
    }

    gchar *message = g_strdup_printf(
        "End process \"%s\" (PID %u)?\n"
        "Unsaved work may be lost.",
        name,
        pid
    );

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),

        GTK_DIALOG_MODAL |
        GTK_DIALOG_DESTROY_WITH_PARENT,

        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_CANCEL,

        "%s",
        message
    );

    gtk_dialog_add_button(
        GTK_DIALOG(dialog),
        "End Task",
        GTK_RESPONSE_ACCEPT
    );

    gint response = gtk_dialog_run(
        GTK_DIALOG(dialog)
    );

    gtk_widget_destroy(dialog);

    g_free(message);

    if (response == GTK_RESPONSE_ACCEPT) {

        HANDLE process = OpenProcess(
            PROCESS_TERMINATE,
            FALSE,
            pid
        );

        if (process) {

            BOOL ok = TerminateProcess(
                process,
                1
            );

            DWORD error_code = GetLastError();

            CloseHandle(process);

            if (ok) {

                gtk_label_set_text(
                    GTK_LABEL(app->status_label),
                    "Process termination requested."
                );

            } else {

                gchar *error = g_strdup_printf(
                    "Windows could not terminate process. Error: %lu",
                    (unsigned long)error_code
                );

                gtk_label_set_text(
                    GTK_LABEL(app->status_label),
                    error
                );

                g_free(error);
            }

        } else {

            DWORD error_code = GetLastError();

            gchar *error = g_strdup_printf(
                "Cannot access process. Error: %lu",
                (unsigned long)error_code
            );

            gtk_label_set_text(
                GTK_LABEL(app->status_label),
                error
            );

            g_free(error);
        }

        g_free(name);

        refresh_processes(app);

        return;
    }

    g_free(name);
}


/* =========================================
   PROCESS DETAILS
========================================= */

static void on_details(
    GtkButton *button,
    gpointer data
) {

    (void)button;

    App *app = data;

    guint pid = 0;

    gchar *name = NULL;

    if (!get_selected_process(app, &pid, &name)) {

        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Select a process to view details."
        );

        return;
    }

    gchar *text = g_strdup_printf(
        "Process: %s\n"
        "Process ID: %u\n\n"
        "Memory figures may be unavailable for "
        "protected or system processes.",
        name,
        pid
    );

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),

        GTK_DIALOG_MODAL |
        GTK_DIALOG_DESTROY_WITH_PARENT,

        GTK_MESSAGE_INFO,
        GTK_BUTTONS_CLOSE,

        "%s",
        text
    );

    gtk_window_set_title(
        GTK_WINDOW(dialog),
        "Process details"
    );

    gtk_dialog_run(GTK_DIALOG(dialog));

    gtk_widget_destroy(dialog);

    g_free(text);
    g_free(name);
}


/* =========================================
   AUTOMATIC REFRESH
========================================= */

static gboolean periodic_refresh(gpointer data) {

    refresh_processes((App *)data);

    return G_SOURCE_CONTINUE;
}


/* =========================================
   MANUAL REFRESH
========================================= */

static void on_refresh_clicked(
    GtkButton *button,
    gpointer data
) {

    (void)button;

    refresh_processes((App *)data);
}


/* =========================================
   GTK TABLE COLUMNS
========================================= */

static void add_text_column(
    GtkTreeView *tree,
    const gchar *title,
    int column,
    int width
) {

    GtkCellRenderer *renderer =
        gtk_cell_renderer_text_new();

    GtkTreeViewColumn *view_column =
        gtk_tree_view_column_new_with_attributes(
            title,
            renderer,
            "text",
            column,
            NULL
        );

    gtk_tree_view_column_set_resizable(
        view_column,
        TRUE
    );

    gtk_tree_view_column_set_min_width(
        view_column,
        width
    );

    gtk_tree_view_append_column(
        tree,
        view_column
    );
}


/* =========================================
   WINDOW DESTROY
========================================= */

static void on_destroy(
    GtkWidget *widget,
    gpointer data
) {

    (void)widget;

    App *app = data;

    if (app->timer_id) {

        g_source_remove(app->timer_id);
    }

    gtk_main_quit();
}


/* =========================================
   MAIN FUNCTION
========================================= */

int main(int argc, char **argv) {

    gtk_init(&argc, &argv);

    App app = {0};

    /* Enable debug privilege */

    app.debug_privilege_enabled =
        enable_debug_privilege();

    /* Main window */

    app.window = gtk_window_new(
        GTK_WINDOW_TOPLEVEL
    );

    gtk_window_set_title(
        GTK_WINDOW(app.window),
        "Windows Task Manager"
    );

    gtk_window_set_default_size(
        GTK_WINDOW(app.window),
        1100,
        650
    );

    gtk_window_set_position(
        GTK_WINDOW(app.window),
        GTK_WIN_POS_CENTER
    );

    g_signal_connect(
        app.window,
        "destroy",
        G_CALLBACK(on_destroy),
        &app
    );


    /* Root layout */

    GtkWidget *root = gtk_box_new(
        GTK_ORIENTATION_VERTICAL,
        0
    );

    gtk_container_add(
        GTK_CONTAINER(app.window),
        root
    );


    /* Header */

    GtkWidget *header = gtk_box_new(
        GTK_ORIENTATION_HORIZONTAL,
        12
    );

    gtk_widget_set_margin_start(header, 18);
    gtk_widget_set_margin_end(header, 18);
    gtk_widget_set_margin_top(header, 16);
    gtk_widget_set_margin_bottom(header, 14);

    gtk_box_pack_start(
        GTK_BOX(root),
        header,
        FALSE,
        FALSE,
        0
    );


    /* Heading */

    GtkWidget *heading_box = gtk_box_new(
        GTK_ORIENTATION_VERTICAL,
        2
    );

    GtkWidget *heading = gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(heading),
        "<span size='x-large' weight='bold'>Task Manager</span>"
    );

    gtk_widget_set_halign(
        heading,
        GTK_ALIGN_START
    );

    GtkWidget *subtitle = gtk_label_new(
        "Processes and system activity"
    );

    gtk_widget_set_halign(
        subtitle,
        GTK_ALIGN_START
    );

    gtk_style_context_add_class(
        gtk_widget_get_style_context(subtitle),
        "dim-label"
    );

    gtk_box_pack_start(
        GTK_BOX(heading_box),
        heading,
        FALSE,
        FALSE,
        0
    );

    gtk_box_pack_start(
        GTK_BOX(heading_box),
        subtitle,
        FALSE,
        FALSE,
        0
    );

    gtk_box_pack_start(
        GTK_BOX(header),
        heading_box,
        TRUE,
        TRUE,
        0
    );


    /* CPU and Memory labels */

    app.cpu_label = gtk_label_new("CPU  --");

    app.memory_label = gtk_label_new("Memory  --");

    gtk_box_pack_start(
        GTK_BOX(header),
        app.cpu_label,
        FALSE,
        FALSE,
        4
    );

    gtk_box_pack_start(
        GTK_BOX(header),
        app.memory_label,
        FALSE,
        FALSE,
        4
    );


    /* Toolbar */

    GtkWidget *toolbar = gtk_box_new(
        GTK_ORIENTATION_HORIZONTAL,
        8
    );

    gtk_widget_set_margin_start(toolbar, 18);
    gtk_widget_set_margin_end(toolbar, 18);
    gtk_widget_set_margin_bottom(toolbar, 12);

    gtk_box_pack_start(
        GTK_BOX(root),
        toolbar,
        FALSE,
        FALSE,
        0
    );


    /* Search */

    app.search = gtk_search_entry_new();

    gtk_entry_set_placeholder_text(
        GTK_ENTRY(app.search),
        "Search by process name or PID..."
    );

    gtk_widget_set_hexpand(
        app.search,
        TRUE
    );

    gtk_box_pack_start(
        GTK_BOX(toolbar),
        app.search,
        TRUE,
        TRUE,
        0
    );


    /* Buttons */

    GtkWidget *refresh = gtk_button_new_with_label(
        "Refresh"
    );

    GtkWidget *details = gtk_button_new_with_label(
        "Details"
    );

    GtkWidget *end = gtk_button_new_with_label(
        "End Task"
    );

    gtk_box_pack_start(
        GTK_BOX(toolbar),
        refresh,
        FALSE,
        FALSE,
        0
    );

    gtk_box_pack_start(
        GTK_BOX(toolbar),
        details,
        FALSE,
        FALSE,
        0
    );

    gtk_box_pack_start(
        GTK_BOX(toolbar),
        end,
        FALSE,
        FALSE,
        0
    );


    /* Process data store */

    app.store = gtk_list_store_new(
        COL_COUNT,

        G_TYPE_STRING,
        G_TYPE_UINT,
        G_TYPE_STRING,
        G_TYPE_STRING
    );


    /* Search filter */

    app.filter = GTK_TREE_MODEL_FILTER(
        gtk_tree_model_filter_new(
            GTK_TREE_MODEL(app.store),
            NULL
        )
    );

    gtk_tree_model_filter_set_visible_func(
        app.filter,
        filter_process,
        &app,
        NULL
    );


    /* Scroll container */

    GtkWidget *scrolled = gtk_scrolled_window_new(
        NULL,
        NULL
    );

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_AUTOMATIC,
        GTK_POLICY_AUTOMATIC
    );

    gtk_widget_set_margin_start(scrolled, 18);
    gtk_widget_set_margin_end(scrolled, 18);

    gtk_widget_set_vexpand(
        scrolled,
        TRUE
    );

    gtk_box_pack_start(
        GTK_BOX(root),
        scrolled,
        TRUE,
        TRUE,
        0
    );


    /* Process table */

    app.tree = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(app.filter)
    );

    gtk_tree_view_set_headers_clickable(
        GTK_TREE_VIEW(app.tree),
        TRUE
    );

    gtk_tree_view_set_rules_hint(
        GTK_TREE_VIEW(app.tree),
        TRUE
    );

    gtk_tree_view_set_search_column(
        GTK_TREE_VIEW(app.tree),
        COL_NAME
    );


    /* Columns */

    add_text_column(
        GTK_TREE_VIEW(app.tree),
        "Process",
        COL_NAME,
        300
    );

    add_text_column(
        GTK_TREE_VIEW(app.tree),
        "PID",
        COL_PID,
        90
    );

    add_text_column(
        GTK_TREE_VIEW(app.tree),
        "Memory",
        COL_MEMORY,
        130
    );

    add_text_column(
        GTK_TREE_VIEW(app.tree),
        "Status",
        COL_STATUS,
        200
    );


    gtk_container_add(
        GTK_CONTAINER(scrolled),
        app.tree
    );


    /* Footer */

    app.status_label = gtk_label_new("Ready");

    gtk_widget_set_halign(
        app.status_label,
        GTK_ALIGN_START
    );

    gtk_widget_set_margin_start(
        app.status_label,
        18
    );

    gtk_widget_set_margin_end(
        app.status_label,
        18
    );

    gtk_widget_set_margin_top(
        app.status_label,
        8
    );

    gtk_widget_set_margin_bottom(
        app.status_label,
        10
    );

    gtk_box_pack_start(
        GTK_BOX(root),
        app.status_label,
        FALSE,
        FALSE,
        0
    );


    /* Signal connections */

    g_signal_connect(
        app.search,
        "search-changed",
        G_CALLBACK(on_search_changed),
        &app
    );

    g_signal_connect(
        refresh,
        "clicked",
        G_CALLBACK(on_refresh_clicked),
        &app
    );

    g_signal_connect(
        details,
        "clicked",
        G_CALLBACK(on_details),
        &app
    );

    g_signal_connect(
        end,
        "clicked",
        G_CALLBACK(on_end_task),
        &app
    );


    /* CSS styling */

    const gchar *css =
        "window { background: #f5f7fb; }"
        "treeview { background: white; }"
        "treeview.view:selected {"
        " background: #dbeafe;"
        " color: #111827;"
        "}"
        "button { padding: 7px 13px; }"
        "entry { padding: 8px; }";

    GtkCssProvider *provider =
        gtk_css_provider_new();

    gtk_css_provider_load_from_data(
        provider,
        css,
        -1,
        NULL
    );

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    g_object_unref(provider);


    /* Initial refresh */

    refresh_processes(&app);


    /* Automatic refresh every 3 seconds */

    app.timer_id = g_timeout_add_seconds(
        3,
        periodic_refresh,
        &app
    );


    /* Show application */

    gtk_widget_show_all(app.window);

    gtk_main();


    /* Cleanup */

    g_object_unref(app.filter);

    g_object_unref(app.store);

    return 0;
}
