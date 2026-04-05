/*
 * camera-bridge.cpp — Surface Laptop 2 webcam bridge
 *
 * ov9734 IPU3 RAW10 (ip3G) → debayer → pipewiresink (PipeWire node)
 *                                     → /dev/video20 YUYV (V4L2 loopback)
 *
 * Build:
 *   pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0
 *   g++ -O2 -std=c++17 -o camera-bridge camera-bridge.cpp \
 *       $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) \
 *       -lpthread
 *
 * Install deps (Ubuntu/Debian):
 *   sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
 *       gstreamer1.0-plugins-good gstreamer1.0-pipewire v4l-utils
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── Sensor / output geometry ────────────────────────────────────────
static constexpr int SENSOR_W   = 1296;
static constexpr int SENSOR_H   = 734;
static constexpr int STRIDE     = 1664;
static constexpr int FRAME_IN   = STRIDE * SENSOR_H;
static constexpr int OUT_W      = SENSOR_W / 2;   // 648
static constexpr int OUT_H      = SENSOR_H / 2;   // 367
static constexpr int FRAMERATE  = 30;
static constexpr double STOP_GRACE = 5.0;
static constexpr const char* V4L2_DEV = "/dev/video20";

// ── RAW10 unpack lookup table (built once) ──────────────────────────
struct Raw10Col {
    int word;       // u32 index within the 32-byte group
    int shift;      // bit shift
    bool spans;     // crosses a u32 boundary?
    int lo_bits;    // bits available in first word (only if spans)
    uint32_t hi_mask; // mask for second word (only if spans)
};

static std::array<Raw10Col, 25> g_raw10_lut;

static void build_raw10_lut() {
    for (int k = 0; k < 25; ++k) {
        int bit = k * 10;
        auto& c = g_raw10_lut[k];
        c.word  = bit >> 5;
        c.shift = bit & 31;
        int lo  = 32 - c.shift;
        c.spans = lo < 10;
        c.lo_bits = lo;
        c.hi_mask = (1u << (10 - lo)) - 1;
    }
}

// ── Global state ────────────────────────────────────────────────────
static std::atomic<bool> g_quit{false};
static std::mutex g_lock;
static std::thread g_capture_thread;
static std::atomic<bool> g_capture_running{false};
static std::atomic<bool> g_capture_stop{false};
static GMainLoop* g_main_loop = nullptr;
static GstElement* g_appsrc = nullptr;
static int g_v4l2_fd = -1;

// Brightness: 0.0 = black, 1.0 = normal, 2.0 = 2× gain.  Default 1.0.
// Stored as fixed-point ×256 for use in the hot path.
static std::atomic<int> g_brightness_fp{256};  // 1.0 × 256

// ── D-Bus interface for external control ────────────────────────────
static const char* DBUS_NAME = "com.surface.CameraBridge";
static const char* DBUS_PATH = "/com/surface/CameraBridge";

static const gchar dbus_introspection_xml[] =
    "<node>"
    "  <interface name='com.surface.CameraBridge'>"
    "    <method name='SetBrightness'>"
    "      <arg type='d' name='value' direction='in'/>"
    "    </method>"
    "    <method name='GetBrightness'>"
    "      <arg type='d' name='value' direction='out'/>"
    "    </method>"
    "    <method name='GetStatus'>"
    "      <arg type='b' name='capturing' direction='out'/>"
    "    </method>"
    "    <property name='Brightness' type='d' access='readwrite'/>"
    "    <property name='Capturing' type='b' access='read'/>"
    "    <signal name='BrightnessChanged'>"
    "      <arg type='d' name='value'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static GDBusNodeInfo* dbus_node_info = nullptr;
static GDBusConnection* dbus_conn = nullptr;

static void set_brightness(double val) {
    val = std::clamp(val, 0.0, 3.0);
    g_brightness_fp.store(static_cast<int>(val * 256.0),
                          std::memory_order_relaxed);
    // Emit signal
    if (dbus_conn) {
        GError* err = nullptr;
        g_dbus_connection_emit_signal(
            dbus_conn, nullptr, DBUS_PATH,
            "com.surface.CameraBridge", "BrightnessChanged",
            g_variant_new("(d)", val), &err);
        if (err) g_error_free(err);
    }
}

static double get_brightness() {
    return g_brightness_fp.load(std::memory_order_relaxed) / 256.0;
}

static void dbus_method_call(GDBusConnection* /*conn*/,
                             const gchar* /*sender*/,
                             const gchar* /*obj_path*/,
                             const gchar* /*iface*/,
                             const gchar* method_name,
                             GVariant* parameters,
                             GDBusMethodInvocation* invocation,
                             gpointer /*user_data*/) {
    if (g_strcmp0(method_name, "SetBrightness") == 0) {
        gdouble val;
        g_variant_get(parameters, "(d)", &val);
        set_brightness(val);
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (g_strcmp0(method_name, "GetBrightness") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(d)", get_brightness()));
    } else if (g_strcmp0(method_name, "GetStatus") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(b)", g_capture_running.load()));
    }
}

static GVariant* dbus_get_property(GDBusConnection* /*conn*/,
                                   const gchar* /*sender*/,
                                   const gchar* /*obj_path*/,
                                   const gchar* /*iface*/,
                                   const gchar* property_name,
                                   GError** /*error*/,
                                   gpointer /*user_data*/) {
    if (g_strcmp0(property_name, "Brightness") == 0)
        return g_variant_new_double(get_brightness());
    if (g_strcmp0(property_name, "Capturing") == 0)
        return g_variant_new_boolean(g_capture_running.load());
    return nullptr;
}

static gboolean dbus_set_property(GDBusConnection* /*conn*/,
                                  const gchar* /*sender*/,
                                  const gchar* /*obj_path*/,
                                  const gchar* /*iface*/,
                                  const gchar* property_name,
                                  GVariant* value,
                                  GError** /*error*/,
                                  gpointer /*user_data*/) {
    if (g_strcmp0(property_name, "Brightness") == 0) {
        set_brightness(g_variant_get_double(value));
        return TRUE;
    }
    return FALSE;
}

static const GDBusInterfaceVTable dbus_vtable = {
    dbus_method_call,
    dbus_get_property,
    dbus_set_property,
    {}
};

static void on_bus_acquired(GDBusConnection* conn,
                            const gchar* /*name*/,
                            gpointer /*user_data*/) {
    dbus_conn = conn;
    GError* err = nullptr;
    g_dbus_connection_register_object(
        conn, DBUS_PATH,
        dbus_node_info->interfaces[0],
        &dbus_vtable, nullptr, nullptr, &err);
    if (err) {
        fprintf(stderr, "D-Bus register failed: %s\n", err->message);
        g_error_free(err);
    } else {
        fprintf(stderr, "D-Bus: %s registered\n", DBUS_NAME);
    }
}

static void on_name_acquired(GDBusConnection* /*conn*/,
                              const gchar* /*name*/,
                              gpointer /*user_data*/) {}

static void on_name_lost(GDBusConnection* /*conn*/,
                          const gchar* /*name*/,
                          gpointer /*user_data*/) {
    fprintf(stderr, "D-Bus: lost name %s\n", DBUS_NAME);
}

// ── Helpers: run a command and capture stdout ───────────────────────
static std::string exec_capture(const std::vector<std::string>& args,
                                int timeout_sec = 3) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return {};

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {}; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(STDERR_FILENO);
        close(pipefd[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::string out;
    char buf[4096];
    // Simple read with timeout via poll
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_sec);
    while (true) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        fd_set fds; FD_ZERO(&fds); FD_SET(pipefd[0], &fds);
        struct timeval tv;
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        int r = select(pipefd[0] + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) break;
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, n);
    }
    close(pipefd[0]);
    int st; waitpid(pid, &st, 0);
    return out;
}

static bool exec_run(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// ── Media pipeline setup (same as Python version) ───────────────────
static std::string find_device_node(const std::string& media_out,
                                    const std::string& entity_needle) {
    size_t pos = media_out.find(entity_needle);
    if (pos == std::string::npos) return {};
    pos = media_out.find("device node name", pos);
    if (pos == std::string::npos) return {};
    auto line_end = media_out.find('\n', pos);
    std::string line = media_out.substr(pos,
        (line_end == std::string::npos ? media_out.size() : line_end) - pos);
    auto sp = line.rfind(' ');
    return (sp != std::string::npos) ? line.substr(sp + 1) : std::string{};
}

static std::string find_cio2_video_node() {
    auto out = exec_capture({"media-ctl", "-d", "/dev/media0", "-p"});
    return find_device_node(out, "ipu3-cio2 1");
}

static std::string find_ov9734_entity() {
    auto out = exec_capture({"media-ctl", "-d", "/dev/media0", "-p"});
    // Parse line by line, looking for a line with both "entity" and "ov9734"
    // Format: "* entity 37: ov9734 2-0036 (1 pad, 1 link 0 routes)"
    size_t pos = 0;
    while (pos < out.size()) {
        auto line_end = out.find('\n', pos);
        if (line_end == std::string::npos) line_end = out.size();
        std::string line = out.substr(pos, line_end - pos);
        pos = line_end + 1;

        // Need both "entity" and "ov9734" (case-insensitive) on the same line
        std::string lower_line = line;
        std::transform(lower_line.begin(), lower_line.end(),
                       lower_line.begin(), ::tolower);
        if (lower_line.find("entity") == std::string::npos ||
            lower_line.find("ov9734") == std::string::npos)
            continue;

        // Extract name between ": " and " ("
        auto colon = line.find(": ");
        if (colon != std::string::npos) {
            auto paren = line.find(" (", colon);
            if (paren != std::string::npos)
                return line.substr(colon + 2, paren - colon - 2);
        }
    }
    throw std::runtime_error("ov9734 entity not found in media topology");
}

static std::string find_ov9734_subdev() {
    auto out = exec_capture({"media-ctl", "-d", "/dev/media0", "-p"});
    std::string lower = out;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    size_t pos = lower.find("ov9734");
    if (pos == std::string::npos) return {};
    return find_device_node(out, "ov9734");
}

static void setup_media_pipeline() {
    auto sensor = find_ov9734_entity();
    fprintf(stderr, "Found sensor entity: %s\n", sensor.c_str());

    std::string fmt = "fmt:SGRBG10_1X10/1296x734";
    exec_run({"media-ctl", "-d", "/dev/media0", "-l",
              "\"" + sensor + "\":0->\"ipu3-csi2 1\":0[1]"});
    exec_run({"media-ctl", "-d", "/dev/media0", "--set-v4l2",
              "\"" + sensor + "\":0[" + fmt + "]"});
    exec_run({"media-ctl", "-d", "/dev/media0", "--set-v4l2",
              "\"ipu3-csi2 1\":0[" + fmt + "]"});
    exec_run({"media-ctl", "-d", "/dev/media0", "--set-v4l2",
              "\"ipu3-csi2 1\":1[" + fmt + "]"});

    auto subdev = find_ov9734_subdev();
    if (!subdev.empty()) {
        exec_run({"v4l2-ctl", "-d", subdev,
                  "--set-ctrl=analogue_gain=80,digital_gain=10"});
    }
}

// ── V4L2 loopback setup ────────────────────────────────────────────
static int open_v4l2_loopback() {
    int fd = open(V4L2_DEV, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "v4l2loopback not available: %s\n", strerror(errno));
        return -1;
    }
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width       = OUT_W;
    fmt.fmt.pix.height      = OUT_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = OUT_W * 2;
    fmt.fmt.pix.sizeimage    = OUT_W * 2 * OUT_H;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "v4l2loopback VIDIOC_S_FMT failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(stderr, "v4l2loopback: %s ready (%dx%d YUYV)\n",
            V4L2_DEV, OUT_W, OUT_H);
    return fd;
}

// ── Frame processing (hot path — all integer, no allocations) ───────

// Unpack IPU3 RAW10 packed format into 8-bit Bayer.
// Input:  FRAME_IN bytes (STRIDE * SENSOR_H)
// Output: SENSOR_H * SENSOR_W bytes (pre-allocated)
static void unpack_raw10(const uint8_t* __restrict__ in,
                         uint8_t* __restrict__ out) {
    for (int row = 0; row < SENSOR_H; ++row) {
        const uint8_t* row_in = in + row * STRIDE;
        uint8_t* row_out = out + row * SENSOR_W;
        int col_out = 0;

        // Each group: 32 bytes → 25 pixels (10 bits each)
        for (int grp = 0; grp < 52 && col_out < SENSOR_W; ++grp) {
            const uint32_t* u32 =
                reinterpret_cast<const uint32_t*>(row_in + grp * 32);

            for (int k = 0; k < 25 && col_out < SENSOR_W; ++k, ++col_out) {
                const auto& c = g_raw10_lut[k];
                uint32_t val;
                if (!c.spans) {
                    val = (u32[c.word] >> c.shift) & 0x3FF;
                } else {
                    val = ((u32[c.word] >> c.shift) |
                           ((u32[c.word + 1] & c.hi_mask) << c.lo_bits))
                          & 0x3FF;
                }
                row_out[col_out] = static_cast<uint8_t>(val >> 2);
            }
        }
    }
}

// Debayer GRBG with 2×2 binning → half-res RGB.
// Input:  SENSOR_H × SENSOR_W bayer
// Output: OUT_H × OUT_W × 3 RGB (pre-allocated)
// White balance: R×1.6, G×1.0, B×1.3 done as integer (×13/8, ×10/8).
static void debayer_half(const uint8_t* __restrict__ bayer,
                         uint8_t* __restrict__ rgb) {
    // Read brightness once per frame (atomic, relaxed is fine)
    const int bright = g_brightness_fp.load(std::memory_order_relaxed);

    for (int y = 0; y < OUT_H; ++y) {
        const uint8_t* r0 = bayer + (y * 2)     * SENSOR_W;
        const uint8_t* r1 = bayer + (y * 2 + 1) * SENSOR_W;
        uint8_t* dst = rgb + y * OUT_W * 3;

        for (int x = 0; x < OUT_W; ++x) {
            int x2 = x * 2;
            // GRBG: row0=[G R], row1=[B G]
            int g = (static_cast<int>(r0[x2]) + r1[x2 + 1]) >> 1;
            int r = r0[x2 + 1];
            int b = r1[x2];

            // White balance as integer multiply+shift
            r = (r * 13) >> 3;  // ~1.625
            b = (b * 10) >> 3;  // ~1.25
            // g stays as-is (×1.0)

            // Apply brightness (fixed-point ×256, so >>8)
            r = (r * bright) >> 8;
            g = (g * bright) >> 8;
            b = (b * bright) >> 8;

            dst[x * 3 + 0] = static_cast<uint8_t>(std::min(r, 255));
            dst[x * 3 + 1] = static_cast<uint8_t>(std::min(g, 255));
            dst[x * 3 + 2] = static_cast<uint8_t>(std::min(b, 255));
        }
    }
}

// RGB → YUYV conversion, integer fixed-point (×256).
// Input:  OUT_H × OUT_W × 3 RGB
// Output: OUT_H × OUT_W × 2 YUYV (pre-allocated)
static void rgb_to_yuyv(const uint8_t* __restrict__ rgb,
                        uint8_t* __restrict__ yuyv) {
    for (int y = 0; y < OUT_H; ++y) {
        const uint8_t* src = rgb  + y * OUT_W * 3;
        uint8_t* dst       = yuyv + y * OUT_W * 2;

        for (int x = 0; x < OUT_W; x += 2) {
            int r0 = src[x*3+0], g0 = src[x*3+1], b0 = src[x*3+2];
            int r1 = src[x*3+3], g1 = src[x*3+4], b1 = src[x*3+5];

            int y0 = ( 77*r0 + 150*g0 +  29*b0) >> 8;
            int y1 = ( 77*r1 + 150*g1 +  29*b1) >> 8;
            int u  = (-38*r0 -  74*g0 + 112*b0 + 32768) >> 8;
            int v  = (157*r0 - 132*g0 -  26*b0 + 32768) >> 8;

            dst[x*2+0] = std::clamp(y0, 0, 255);
            dst[x*2+1] = std::clamp(u,  0, 255);
            dst[x*2+2] = std::clamp(y1, 0, 255);
            dst[x*2+3] = std::clamp(v,  0, 255);
        }
    }
}

// ── Capture loop ────────────────────────────────────────────────────
static void capture_loop(const std::string& sensor_dev) {
    // Start v4l2-ctl streaming
    int pipe_in[2], pipe_err[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_err) < 0) {
        perror("pipe"); return;
    }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return; }

    if (child == 0) {
        close(pipe_in[0]);
        close(pipe_err[0]);
        dup2(pipe_in[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        close(pipe_in[1]);
        close(pipe_err[1]);
        std::string fmt_arg = "--set-fmt-video=width=1296,height=734,pixelformat=ip3G";
        execlp("v4l2-ctl", "v4l2-ctl",
               "-d", sensor_dev.c_str(),
               fmt_arg.c_str(),
               "--stream-mmap=4", "--stream-to=-", "--stream-count=0",
               nullptr);
        _exit(127);
    }

    close(pipe_in[1]);
    close(pipe_err[1]);

    // Set non-blocking on the data pipe
    int flags = fcntl(pipe_in[0], F_GETFL);
    fcntl(pipe_in[0], F_SETFL, flags | O_NONBLOCK);

    // Pre-allocate all frame buffers (zero heap allocations in the loop)
    std::vector<uint8_t> read_buf(FRAME_IN * 2);
    std::vector<uint8_t> bayer(SENSOR_H * SENSOR_W);
    std::vector<uint8_t> rgb(OUT_W * OUT_H * 3);
    std::vector<uint8_t> yuyv(OUT_W * OUT_H * 2);

    size_t buf_fill = 0;
    int frame_count = 0;

    while (!g_capture_stop.load(std::memory_order_relaxed)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(pipe_in[0], &fds);
        struct timeval tv = {0, 500000}; // 500ms timeout
        int r = select(pipe_in[0] + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        ssize_t n = read(pipe_in[0], read_buf.data() + buf_fill,
                         read_buf.size() - buf_fill);
        if (n <= 0) break;
        buf_fill += n;

        while (buf_fill >= static_cast<size_t>(FRAME_IN)) {
            unpack_raw10(read_buf.data(), bayer.data());
            debayer_half(bayer.data(), rgb.data());
            ++frame_count;

            if (frame_count % 30 == 0) {
                fprintf(stderr, "  %d frames\n", frame_count);
            }

            // Push RGB to GStreamer appsrc
            GstBuffer* gst_buf = gst_buffer_new_allocate(
                nullptr, rgb.size(), nullptr);
            gst_buffer_fill(gst_buf, 0, rgb.data(), rgb.size());
            gst_app_src_push_buffer(GST_APP_SRC(g_appsrc), gst_buf);
            // gst_buf is now owned by appsrc

            // Write YUYV to v4l2loopback
            if (g_v4l2_fd >= 0) {
                rgb_to_yuyv(rgb.data(), yuyv.data());
                ssize_t w = write(g_v4l2_fd, yuyv.data(), yuyv.size());
                if (w < 0 && (frame_count <= 3 || frame_count % 300 == 0)) {
                    fprintf(stderr, "v4l2 write error: %s\n", strerror(errno));
                }
            }

            // Shift remaining data forward
            size_t consumed = FRAME_IN;
            buf_fill -= consumed;
            if (buf_fill > 0)
                memmove(read_buf.data(), read_buf.data() + consumed, buf_fill);
        }
    }

    close(pipe_in[0]);
    close(pipe_err[0]);
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    fprintf(stderr, "Capture loop exited (%d frames)\n", frame_count);
}

static void start_capture(const std::string& sensor_dev) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_capture_running.load()) return;
    fprintf(stderr, "Consumer connected — starting sensor capture\n");
    g_capture_stop.store(false);
    g_capture_running.store(true);
    g_capture_thread = std::thread([sensor_dev]() {
        capture_loop(sensor_dev);
        g_capture_running.store(false);
    });
}

static void stop_capture() {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_capture_running.load()) return;
    }
    g_capture_stop.store(true);
    if (g_capture_thread.joinable())
        g_capture_thread.join();
    fprintf(stderr, "No consumers — sensor off\n");
}

// ── Consumer detection ──────────────────────────────────────────────

// Count direct V4L2 readers on /dev/video20 via /proc scan
static int count_v4l2_readers(dev_t loopback_rdev) {
    int count = 0;
    pid_t my_pid = getpid();
    DIR* proc = opendir("/proc");
    if (!proc) return 0;

    struct dirent* de;
    while ((de = readdir(proc)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        pid_t pid = atoi(de->d_name);
        if (pid == my_pid) continue;

        // Skip our own process and our child processes (v4l2-ctl)
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        pid_t ppid = 0;
        FILE* sf = fopen(stat_path, "r");
        if (sf) {
            // Format: pid (comm) state ppid ...
            int dummy; char cbuf[256]; char state;
            if (fscanf(sf, "%d %255s %c %d", &dummy, cbuf, &state, &ppid) != 4)
                ppid = 0;
            fclose(sf);
        }
        if (ppid == my_pid) continue;
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        char comm[256] = {};
        FILE* cf = fopen(comm_path, "r");
        if (cf) {
            if (fgets(comm, sizeof(comm), cf)) {
                // Strip trailing newline
                char* nl = strchr(comm, '\n');
                if (nl) *nl = '\0';
            }
            fclose(cf);
        }
        // Skip PipeWire and WirePlumber — they hold the device open
        // for node management, not as actual consumers
        if (strcmp(comm, "pipewire") == 0 ||
            strcmp(comm, "wireplumber") == 0) continue;

        char fd_path[64];
        snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);
        DIR* fds = opendir(fd_path);
        if (!fds) continue;

        struct dirent* fde;
        while ((fde = readdir(fds)) != nullptr) {
            if (fde->d_name[0] == '.') continue;
            char link[320];
            snprintf(link, sizeof(link), "%s/%s", fd_path, fde->d_name);
            struct stat st;
            if (stat(link, &st) == 0 &&
                S_ISCHR(st.st_mode) && st.st_rdev == loopback_rdev) {
                ++count;
                fprintf(stderr, "v4l2 reader: pid=%d (%s)\n", pid, comm);
                break;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return count;
}

// Count PipeWire output links from our nodes using pw-link -l
// Output format:
//   83 ov9734-webcam:capture_1
//   88    |->    84 firefox-bin:input_1
// We count lines containing "|-> " that follow a line with our node name.
static int count_pw_links(const std::vector<std::string>& node_names) {
    auto out = exec_capture({"pw-link", "-l"}, 2);
    int count = 0;
    bool in_our_node = false;

    size_t pos = 0;
    while (pos < out.size()) {
        auto line_end = out.find('\n', pos);
        if (line_end == std::string::npos) line_end = out.size();
        std::string line = out.substr(pos, line_end - pos);
        pos = line_end + 1;

        if (line.find("|->") != std::string::npos) {
            // This is an output link line
            if (in_our_node) ++count;
        } else if (line.find("|<-") != std::string::npos) {
            // Input link line, skip
        } else {
            // Node header line — check if it's one of ours
            in_our_node = false;
            for (auto& name : node_names) {
                if (line.find(name) != std::string::npos) {
                    in_our_node = true;
                    break;
                }
            }
        }
    }
    return count;
}

// Wait for our PipeWire node to appear (startup only)
// Check if our PipeWire node exists using pw-cli
static bool wait_for_pw_node() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        auto out = exec_capture({"pw-cli", "ls", "Node"}, 3);
        if (out.find("ov9734-webcam") != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

static void pw_watcher(const std::string& sensor_dev) {
    if (!wait_for_pw_node()) {
        fprintf(stderr, "Warning: PW node not found — consumer detection disabled\n");
        return;
    }

    std::vector<std::string> node_names = {"ov9734-webcam"};
    // Check if v4l2loopback node exists
    {
        auto out = exec_capture({"pw-cli", "ls", "Node"}, 3);
        if (out.find("video20") != std::string::npos)
            node_names.push_back("video20");
    }
    fprintf(stderr, "Watching nodes:");
    for (auto& n : node_names) fprintf(stderr, " %s", n.c_str());
    fprintf(stderr, "\n");

    // Cache loopback device rdev
    struct stat dev_st;
    dev_t loopback_rdev = 0;
    if (stat(V4L2_DEV, &dev_st) == 0)
        loopback_rdev = dev_st.st_rdev;

    double last_had_consumers = 0;
    auto now_sec = []() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    while (!g_quit.load(std::memory_order_relaxed)) {
        bool running = g_capture_running.load();

        // Poll fast while streaming, slow while idle
        std::this_thread::sleep_for(
            std::chrono::milliseconds(running ? 500 : 2000));

        // Lightweight link count
        int pw_n = count_pw_links(node_names);

        // /proc scan only when PW shows no links
        int v4l2_n = 0;
        if (pw_n == 0 && loopback_rdev != 0) {
            v4l2_n = count_v4l2_readers(loopback_rdev);
        }

        int n = pw_n + v4l2_n;
        double now = now_sec();

        if (n > 0) {
            last_had_consumers = now;
            if (!running) start_capture(sensor_dev);
        } else {
            if (running && (now - last_had_consumers) > STOP_GRACE)
                stop_capture();
        }
    }
}

// ── Signal handling ─────────────────────────────────────────────────
static void signal_handler(int) {
    g_quit.store(true);
    if (g_main_loop) g_main_loop_quit(g_main_loop);
}

// ── Main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    build_raw10_lut();
    gst_init(&argc, &argv);

    // Discover sensor
    std::string sensor_dev;
    try {
        sensor_dev = find_cio2_video_node();
    } catch (const std::exception& e) {
        fprintf(stderr, "Device discovery failed: %s\n", e.what());
        return 1;
    }
    fprintf(stderr, "Sensor: %s\n", sensor_dev.c_str());

    try {
        setup_media_pipeline();
    } catch (const std::exception& e) {
        fprintf(stderr, "media-ctl setup failed: %s\n", e.what());
        return 1;
    }

    g_v4l2_fd = open_v4l2_loopback();

    // Build GStreamer pipeline
    char caps[256];
    snprintf(caps, sizeof(caps),
             "video/x-raw,format=RGB,width=%d,height=%d,framerate=%d/1",
             OUT_W, OUT_H, FRAMERATE);

    char launch[512];
    snprintf(launch, sizeof(launch),
             "appsrc name=src is-live=true block=false caps=\"%s\" ! "
             "videoconvert ! pipewiresink name=pw-sink sync=false",
             caps);

    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(launch, &err);
    if (err) {
        fprintf(stderr, "Pipeline error: %s\n", err->message);
        g_error_free(err);
        return 1;
    }

    g_appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement* pw_sink = gst_bin_get_by_name(GST_BIN(pipeline), "pw-sink");

    // Set PipeWire stream properties
    GstStructure* props = gst_structure_new(
        "props",
        "media.class",       G_TYPE_STRING, "Video/Source",
        "media.role",        G_TYPE_STRING, "Camera",
        "node.name",         G_TYPE_STRING, "ov9734-webcam",
        "node.description",  G_TYPE_STRING, "Surface Laptop 2 Webcam",
        nullptr);
    g_object_set(pw_sink, "stream-properties", props, nullptr);
    gst_structure_free(props);
    gst_object_unref(pw_sink);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fprintf(stderr, "Idle — PipeWire Video/Source active, sensor off\n");

    // Signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);

    // Start D-Bus service on session bus
    dbus_node_info = g_dbus_node_info_new_for_xml(dbus_introspection_xml, nullptr);
    if (dbus_node_info) {
        g_bus_own_name(G_BUS_TYPE_SESSION,
                       DBUS_NAME,
                       G_BUS_NAME_OWNER_FLAGS_REPLACE,
                       on_bus_acquired,
                       on_name_acquired,
                       on_name_lost,
                       nullptr, nullptr);
    } else {
        fprintf(stderr, "Warning: D-Bus introspection parse failed\n");
    }

    // Start consumer watcher thread
    std::thread watcher([&sensor_dev]() { pw_watcher(sensor_dev); });

    // Run GLib main loop
    g_main_loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(g_main_loop);

    // Cleanup
    g_quit.store(true);
    stop_capture();
    watcher.join();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(g_appsrc);
    gst_object_unref(pipeline);
    g_main_loop_unref(g_main_loop);

    if (g_v4l2_fd >= 0) close(g_v4l2_fd);
    if (dbus_node_info) g_dbus_node_info_unref(dbus_node_info);

    fprintf(stderr, "Clean shutdown.\n");
    return 0;
}
