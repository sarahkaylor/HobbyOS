/*
 * src/user/libreoffice/test/lo_test_tier5.c
 *
 * Tier 5: Live In-Guest X11 GUI E2E Suite for LibreOffice Applications.
 * Validates real X11 window mapping, event loop responsiveness, synthetic
 * user editing, ODF document persistence, and clean window deletion.
 */

#include "lo_test.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

extern int ho_yield_raw(void);

static int g_xserver_pid = -1;
static Display *g_harness_dpy = NULL;

static int lo_test_x_error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    return 0;
}

/* Cooperative non-blocking delay preserving PROC_STATE_READY in scheduler */
static void lo_test_delay_ms(int ms) {
    struct timeval start, now;
    gettimeofday(&start, NULL);
    while (1) {
        ho_yield_raw();
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed >= ms) break;
    }
}

/* Bounded non-blocking waitpid helper */
static int wait_child_exit(pid_t pid, int *status_out, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (status_out) *status_out = status;
            return 0;
        }
        lo_test_delay_ms(5);
        elapsed += 5;
    }
    return -1;
}

/* ---- Helper: Directory Preparation ---- */
static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0777);
    }
}

/* Polling helper to wait for saved ODF package */
static bool wait_for_odf_package(const char *path, const char *expected_mime, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (verify_odf_package(path, expected_mime)) {
            return true;
        }
        lo_test_delay_ms(30);
        elapsed += 30;
    }
    return false;
}

/* ---- Helper: Wait for Child Window by Class ---- */
static Window wait_for_app_window(Display *dpy, const char *expected_res_name) {
    Window root = DefaultRootWindow(dpy);
    for (int retry = 0; retry < 100; retry++) {
        Window root_ret, parent_ret;
        Window *children = NULL;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren) && children) {
            for (unsigned int i = 0; i < nchildren; i++) {
                XClassHint ch;
                if (XGetClassHint(dpy, children[i], &ch)) {
                    bool match = (ch.res_class && strcmp(ch.res_class, "LibreOffice") == 0);
                    if (expected_res_name && ch.res_name) {
                        match = match && (strcmp(ch.res_name, expected_res_name) == 0);
                    }
                    if (ch.res_name) XFree(ch.res_name);
                    if (ch.res_class) XFree(ch.res_class);
                    if (match) {
                        XWindowAttributes attr;
                        if (XGetWindowAttributes(dpy, children[i], &attr) && attr.map_state == IsViewable) {
                            Window found = children[i];
                            XFree(children);
                            return found;
                        }
                    }
                }
            }
            XFree(children);
        }
        lo_test_delay_ms(10);
    }
    return None;
}

/* ---- Helper: Verify Non-Blank Window Rendering ---- */
static bool verify_rendered_pixels(Display *dpy, Window win, int *non_zero_out, int *colors_out) {
    XWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    for (int retry = 0; retry < 50; retry++) {
        if (XGetWindowAttributes(dpy, win, &attr) && attr.map_state == IsViewable &&
            attr.width > 0 && attr.height > 0) {
            break;
        }
        lo_test_delay_ms(10);
    }
    if (attr.map_state != IsViewable || attr.width <= 0 || attr.height <= 0) {
        return false;
    }
    XImage *img = XGetImage(dpy, win, 0, 0, attr.width, attr.height, AllPlanes, ZPixmap);
    if (!img) return false;

    int non_zero = 0;
    unsigned long first_color = 0;
    int color_diffs = 0;

    if (img->bits_per_pixel == 32 && img->data) {
        for (int y = 0; y < img->height; y += 4) {
            const uint32_t *row = (const uint32_t *)(img->data + y * img->bytes_per_line);
            for (int x = 0; x < img->width; x += 4) {
                uint32_t p = row[x];
                if (p != 0) non_zero++;
                if (non_zero == 1) first_color = p;
                else if (p != first_color) color_diffs++;
            }
        }
    } else {
        for (int y = 0; y < img->height; y += 4) {
            for (int x = 0; x < img->width; x += 4) {
                unsigned long p = XGetPixel(img, x, y);
                if (p != 0) non_zero++;
                if (non_zero == 1) first_color = p;
                else if (p != first_color) color_diffs++;
            }
        }
    }
    XDestroyImage(img);

    if (non_zero_out) *non_zero_out = non_zero;
    if (colors_out) *colors_out = color_diffs;
    return (non_zero > 100 && color_diffs > 10);
}

/* ---- Helper: Send Synthetic Key Sequence ---- */
static void send_synthetic_key(Display *dpy, Window win, KeySym sym, unsigned int state) {
    XKeyEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = KeyPress;
    ev.display = dpy;
    ev.window = win;
    ev.root = DefaultRootWindow(dpy);
    ev.time = CurrentTime;
    ev.same_screen = True;
    ev.state = state;
    ev.keycode = XKeysymToKeycode(dpy, sym);

    XSendEvent(dpy, win, True, KeyPressMask, (XEvent *)&ev);
    XFlush(dpy);
    lo_test_delay_ms(5);
}

static void send_text_string(Display *dpy, Window win, const char *text) {
    while (*text) {
        KeySym sym = (KeySym)*text;
        if (*text == '\n') sym = XK_Return;
        send_synthetic_key(dpy, win, sym, 0);
        text++;
    }
}

static void send_ctrl_key(Display *dpy, Window win, KeySym sym) {
    send_synthetic_key(dpy, win, sym, ControlMask);
}

static void send_mouse_click(Display *dpy, Window win, int x, int y, unsigned int button) {
    XButtonEvent bev;
    memset(&bev, 0, sizeof(bev));
    bev.type = ButtonPress;
    bev.display = dpy;
    bev.window = win;
    bev.root = DefaultRootWindow(dpy);
    bev.x = x;
    bev.y = y;
    bev.x_root = x;
    bev.y_root = y;
    bev.time = CurrentTime;
    bev.button = button;
    bev.same_screen = True;

    XSendEvent(dpy, win, True, ButtonPressMask, (XEvent *)&bev);
    XFlush(dpy);
    lo_test_delay_ms(5);

    bev.type = ButtonRelease;
    XSendEvent(dpy, win, True, ButtonReleaseMask, (XEvent *)&bev);
    XFlush(dpy);
    lo_test_delay_ms(5);
}

static bool verify_rendered_region(Display *dpy, Window win, int rx, int ry, int rw, int rh, int *non_zero_out, int *colors_out) {
    XImage *img = XGetImage(dpy, win, rx, ry, rw, rh, AllPlanes, ZPixmap);
    if (!img) return false;
    int non_zero = 0, color_diffs = 0;
    unsigned long first_color = 0;
    if (img->bits_per_pixel == 32 && img->data) {
        for (int y = 0; y < img->height; y += 2) {
            const uint32_t *row = (const uint32_t *)(img->data + y * img->bytes_per_line);
            for (int x = 0; x < img->width; x += 2) {
                uint32_t p = row[x];
                if (p != 0) non_zero++;
                if (non_zero == 1) first_color = p;
                else if (p != first_color) color_diffs++;
            }
        }
    } else {
        for (int y = 0; y < img->height; y += 2) {
            for (int x = 0; x < img->width; x += 2) {
                unsigned long p = XGetPixel(img, x, y);
                if (p != 0) non_zero++;
                if (non_zero == 1) first_color = p;
                else if (p != first_color) color_diffs++;
            }
        }
    }
    XDestroyImage(img);
    if (non_zero_out) *non_zero_out = non_zero;
    if (colors_out) *colors_out = color_diffs;
    return (non_zero > 50 && color_diffs > 5);
}

static unsigned char s_ppm_buf[1024 * 768 * 3 + 64];

static void save_window_ppm(Display *dpy, Window win, const char *filepath) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr) || attr.width <= 0 || attr.height <= 0) return;
    XImage *img = XGetImage(dpy, win, 0, 0, attr.width, attr.height, AllPlanes, ZPixmap);
    if (!img) return;

    int max_bytes = (int)sizeof(s_ppm_buf);
    int total_bytes = 0;
    int hlen = snprintf((char *)s_ppm_buf, 64, "P6\n%d %d\n255\n", img->width, img->height);
    total_bytes += hlen;

    if (img->bits_per_pixel == 32 && img->data) {
        for (int y = 0; y < img->height; y++) {
            const uint32_t *row = (const uint32_t *)(img->data + y * img->bytes_per_line);
            for (int x = 0; x < img->width; x++) {
                if (total_bytes + 3 > max_bytes) break;
                uint32_t p = row[x];
                s_ppm_buf[total_bytes++] = (unsigned char)((p >> 16) & 0xFF);
                s_ppm_buf[total_bytes++] = (unsigned char)((p >> 8) & 0xFF);
                s_ppm_buf[total_bytes++] = (unsigned char)(p & 0xFF);
            }
        }
    } else {
        for (int y = 0; y < img->height; y++) {
            for (int x = 0; x < img->width; x++) {
                if (total_bytes + 3 > max_bytes) break;
                unsigned long p = XGetPixel(img, x, y);
                s_ppm_buf[total_bytes++] = (unsigned char)((p >> 16) & 0xFF);
                s_ppm_buf[total_bytes++] = (unsigned char)((p >> 8) & 0xFF);
                s_ppm_buf[total_bytes++] = (unsigned char)(p & 0xFF);
            }
        }
    }

    FILE *f = fopen(filepath, "wb");
    if (f) {
        fwrite(s_ppm_buf, 1, total_bytes, f);
        fclose(f);
    }
    XDestroyImage(img);
}

static void send_wm_delete(Display *dpy, Window win) {
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    XClientMessageEvent cmev;
    memset(&cmev, 0, sizeof(cmev));
    cmev.type = ClientMessage;
    cmev.display = dpy;
    cmev.window = win;
    cmev.message_type = wm_protocols;
    cmev.format = 32;
    cmev.data.l[0] = wm_delete;
    cmev.data.l[1] = CurrentTime;

    XSendEvent(dpy, win, False, NoEventMask, (XEvent *)&cmev);
    XFlush(dpy);
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void test_x11_server_start(void) {
    ensure_dir("/tmp");
    ensure_dir("/tmp/.X11-unix");
    ensure_dir("/home");

    char *const xserver_argv[] = {
        "XHOBBY.BIN", ":0", "-nolisten", "tcp", "-noreset", "-dumbSched", "-s", "0", NULL
    };
    char *const xserver_env[] = {
        "DISPLAY=:0", "HOME=/home", "USER=hobby", "TMPDIR=/tmp", "FONTCONFIG_FILE=/X11/FONTS.CNF", NULL
    };
    g_xserver_pid = spawn_ex("/XHOBBY.BIN", xserver_argv, xserver_env, 0, 0);
    LO_TEST_ASSERT(g_xserver_pid > 0, "Failed to spawn /XHOBBY.BIN");

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, "/tmp/.X11-unix/X0", sizeof(sa.sun_path) - 1);

    bool connected = false;
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
                close(fd);
                connected = true;
                break;
            }
            close(fd);
        }
        lo_test_delay_ms(10);
    }
    LO_TEST_ASSERT(connected, "/tmp/.X11-unix/X0 socket connect failed");

    setenv("DISPLAY", ":0", 1);
    g_harness_dpy = XOpenDisplay(":0");
    LO_TEST_ASSERT(g_harness_dpy != NULL, "Test harness XOpenDisplay(:0) failed");
    XSetErrorHandler(lo_test_x_error_handler);
}

static void test_gui_soffice_startcenter(void) {
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"soffice", NULL};
        int rc = run_soffice_main(1, argv);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for soffice");

    Window w = wait_for_app_window(g_harness_dpy, "soffice");
    LO_TEST_ASSERT(w != None, "soffice Start Center window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    save_window_ppm(g_harness_dpy, w, "/home/soffice.ppm");

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "soffice blank render");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "soffice exit status != 0");
}

static void test_gui_writer(void) {
    unlink("/home/document.odt");
    unlink("/home/swriter_menu.ppm");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"swriter", NULL};
        int rc = run_writer_main(1, argv, false);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for swriter");

    Window w = wait_for_app_window(g_harness_dpy, "swriter");
    LO_TEST_ASSERT(w != None, "Writer window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    /* 1. Click File menu header (x=20, y=12) */
    send_mouse_click(g_harness_dpy, w, 20, 12, Button1);
    lo_test_delay_ms(10);

    /* 2. Verify File dropdown popup opens with non-blank pixels */
    int menu_nz = 0, menu_cd = 0;
    bool dropdown_ok = verify_rendered_region(g_harness_dpy, w, 5, 24, 200, 150, &menu_nz, &menu_cd);
    LO_TEST_ASSERT(dropdown_ok, "Writer File dropdown failed to open");

    /* 3. Capture screenshot of swriter with open File dropdown */
    save_window_ppm(g_harness_dpy, w, "/home/swriter_menu.ppm");

    /* 4. Click toolbar button (x=76, y=38) */
    send_mouse_click(g_harness_dpy, w, 76, 38, Button1);
    lo_test_delay_ms(10);

    /* 5. Enter text and save document */
    send_text_string(g_harness_dpy, w, "E2E Writer\n");
    lo_test_delay_ms(10);

    send_ctrl_key(g_harness_dpy, w, XK_s);
    lo_test_delay_ms(80);
    bool saved_ok = wait_for_odf_package("/home/document.odt", ODF_MIME_TEXT, 1500);

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "Writer blank render");
    LO_TEST_ASSERT(saved_ok, "Saved /home/document.odt invalid");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "swriter exit status != 0");
}

static void test_gui_calc(void) {
    unlink("/home/sheet.ods");
    unlink("/home/scalc.ppm");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"scalc", NULL};
        int rc = run_calc_main(1, argv, false);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for scalc");

    Window w = wait_for_app_window(g_harness_dpy, "scalc");
    LO_TEST_ASSERT(w != None, "Calc window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    save_window_ppm(g_harness_dpy, w, "/home/scalc.ppm");

    send_mouse_click(g_harness_dpy, w, 60, 140, Button1);
    send_text_string(g_harness_dpy, w, "=10+20\n");
    lo_test_delay_ms(10);

    send_ctrl_key(g_harness_dpy, w, XK_s);
    lo_test_delay_ms(80);
    bool saved_ok = wait_for_odf_package("/home/sheet.ods", ODF_MIME_SPREADSHEET, 1500);

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "Calc blank render");
    LO_TEST_ASSERT(saved_ok, "Saved /home/sheet.ods invalid");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "scalc exit status != 0");
}

static void test_gui_impress(void) {
    unlink("/home/slides.odp");
    unlink("/home/simpress.ppm");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"simpress", NULL};
        int rc = run_impress_main(1, argv, false);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for simpress");

    Window w = wait_for_app_window(g_harness_dpy, "simpress");
    LO_TEST_ASSERT(w != None, "Impress window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    save_window_ppm(g_harness_dpy, w, "/home/simpress.ppm");

    send_text_string(g_harness_dpy, w, "Slides\n");
    lo_test_delay_ms(10);

    send_ctrl_key(g_harness_dpy, w, XK_s);
    lo_test_delay_ms(80);
    bool saved_ok = wait_for_odf_package("/home/slides.odp", ODF_MIME_PRESENTATION, 1500);

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "Impress blank render");
    LO_TEST_ASSERT(saved_ok, "Saved /home/slides.odp invalid");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "simpress exit status != 0");
}

static void test_gui_draw(void) {
    unlink("/home/drawing.odg");
    unlink("/home/sdraw.ppm");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"sdraw", NULL};
        int rc = run_draw_main(1, argv, false);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for sdraw");

    Window w = wait_for_app_window(g_harness_dpy, "sdraw");
    LO_TEST_ASSERT(w != None, "Draw window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    save_window_ppm(g_harness_dpy, w, "/home/sdraw.ppm");

    send_mouse_click(g_harness_dpy, w, 150, 150, Button1);
    lo_test_delay_ms(10);

    send_ctrl_key(g_harness_dpy, w, XK_s);
    lo_test_delay_ms(80);
    bool saved_ok = wait_for_odf_package("/home/drawing.odg", ODF_MIME_GRAPHICS, 1500);

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "Draw blank render");
    LO_TEST_ASSERT(saved_ok, "Saved /home/drawing.odg invalid");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "sdraw exit status != 0");
}

static void test_gui_math(void) {
    unlink("/home/formula.odf");
    unlink("/home/smath.ppm");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"smath", NULL};
        int rc = run_math_main(1, argv, false);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for smath");

    Window w = wait_for_app_window(g_harness_dpy, "smath");
    LO_TEST_ASSERT(w != None, "Math window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    save_window_ppm(g_harness_dpy, w, "/home/smath.ppm");

    send_text_string(g_harness_dpy, w, "x^2\n");
    lo_test_delay_ms(10);

    send_ctrl_key(g_harness_dpy, w, XK_s);
    lo_test_delay_ms(80);
    bool saved_ok = wait_for_odf_package("/home/formula.odf", ODF_MIME_FORMULA, 1500);

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "Math blank render");
    LO_TEST_ASSERT(saved_ok, "Saved /home/formula.odf invalid");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "smath exit status != 0");
}

static void test_gui_base(void) {
    unlink("/home/database.odb");
    unlink("/home/sbase.ppm");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_harness_dpy) {
            close(ConnectionNumber(g_harness_dpy));
            g_harness_dpy = NULL;
        }
        char *argv[] = {"sbase", NULL};
        int rc = run_base_main(1, argv, false);
        exit(rc);
    }
    LO_TEST_ASSERT(pid > 0, "fork() failed for sbase");

    Window w = wait_for_app_window(g_harness_dpy, "sbase");
    LO_TEST_ASSERT(w != None, "Base window not found");
    lo_test_delay_ms(10);

    int non_zero = 0, colors = 0;
    bool rendered_ok = verify_rendered_pixels(g_harness_dpy, w, &non_zero, &colors);

    save_window_ppm(g_harness_dpy, w, "/home/sbase.ppm");

    send_text_string(g_harness_dpy, w, "CREATE TABLE t(x);\n");
    lo_test_delay_ms(10);

    send_ctrl_key(g_harness_dpy, w, XK_s);
    lo_test_delay_ms(80);
    bool saved_ok = wait_for_odf_package("/home/database.odb", ODF_MIME_DATABASE, 1500);

    send_wm_delete(g_harness_dpy, w);
    int status = 0;
    int wr = wait_child_exit(pid, &status, 3000);
    if (wr != 0) {
        kill(pid, SIGKILL);
        wait_child_exit(pid, &status, 1000);
    }
    LO_TEST_ASSERT(rendered_ok, "Base blank render");
    LO_TEST_ASSERT(saved_ok, "Saved /home/database.odb invalid");
    LO_TEST_ASSERT(wr == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "sbase exit status != 0");
}

static void test_x11_server_teardown(void) {
    if (g_harness_dpy) {
        XCloseDisplay(g_harness_dpy);
        g_harness_dpy = NULL;
    }
    if (g_xserver_pid > 0) {
        kill(g_xserver_pid, SIGTERM);
        int status = 0;
        wait_child_exit(g_xserver_pid, &status, 3000);
        g_xserver_pid = -1;
    }
}

void run_tests_tier5_gui(void) {
    LO_LOG("[LO_TEST] TIER 5 LIVE X11 GUI E2E SUITE: START");
    g_current_suite = "TIER5_LIVE_GUI";
    int failed_before = g_tests_failed;

    lo_vcl_preload_fonts();

    run_single_test("tier5_x11_server_start", test_x11_server_start);
    if (!g_harness_dpy) {
        LO_LOG("[LO_TEST] TIER 5: X11 server failed to connect, aborting GUI tests");
        run_single_test("tier5_x11_server_teardown", test_x11_server_teardown);
        return;
    }
    run_single_test("tier5_gui_soffice_startcenter", test_gui_soffice_startcenter);
    run_single_test("tier5_gui_writer", test_gui_writer);
    run_single_test("tier5_gui_calc", test_gui_calc);
    run_single_test("tier5_gui_impress", test_gui_impress);
    run_single_test("tier5_gui_draw", test_gui_draw);
    run_single_test("tier5_gui_math", test_gui_math);
    run_single_test("tier5_gui_base", test_gui_base);
    run_single_test("tier5_x11_server_teardown", test_x11_server_teardown);

    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] TIER 5 LIVE X11 GUI E2E: PASS (7/7 apps)");
    }
}
