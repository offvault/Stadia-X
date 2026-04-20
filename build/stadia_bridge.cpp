#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <linux/input.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#pragma pack(push, 1)
struct ControllerState {
    uint16_t buttons;
    uint8_t  trigger_left;
    uint8_t  trigger_right;
    int16_t  stick_lx;
    int16_t  stick_ly;
    int16_t  stick_rx;
    int16_t  stick_ry;
};
struct RumbleState {
    uint8_t motor_left;
    uint8_t motor_right;
};
#pragma pack(pop)

enum ButtonBit : uint16_t {
    BTN_BIT_A           = 1 << 0,
    BTN_BIT_B           = 1 << 1,
    BTN_BIT_X           = 1 << 2,
    BTN_BIT_Y           = 1 << 3,
    BTN_BIT_LB          = 1 << 4,
    BTN_BIT_RB          = 1 << 5,
    BTN_BIT_SELECT      = 1 << 6,
    BTN_BIT_START       = 1 << 7,
    BTN_BIT_STADIA      = 1 << 8,
    BTN_BIT_L3          = 1 << 9,
    BTN_BIT_R3          = 1 << 10,
    BTN_BIT_ASSISTANT   = 1 << 11,
    BTN_BIT_DPAD_UP     = 1 << 12,
    BTN_BIT_DPAD_DOWN   = 1 << 13,
    BTN_BIT_DPAD_LEFT   = 1 << 14,
    BTN_BIT_DPAD_RIGHT  = 1 << 15,
};

// Mask of all digital buttons that should be SUPPRESSED from gamepad output when a modifier is held
static constexpr uint16_t CHORD_SUPPRESS_MASK =
    BTN_BIT_A | BTN_BIT_B | BTN_BIT_X | BTN_BIT_Y |
    BTN_BIT_DPAD_UP | BTN_BIT_DPAD_DOWN | BTN_BIT_DPAD_LEFT | BTN_BIT_DPAD_RIGHT |
    BTN_BIT_LB | BTN_BIT_RB | BTN_BIT_L3 | BTN_BIT_R3 |
    BTN_BIT_SELECT | BTN_BIT_START | BTN_BIT_STADIA;

static constexpr uint16_t PORT_INPUT  = 45493;
static constexpr uint16_t PORT_RUMBLE = 45494;
static constexpr uint16_t PORT_C2     = 45495;
static constexpr uint16_t PORT_MACRO  = 45499;

static std::atomic<bool> g_running{true};
static std::atomic<int>  g_evdev_fd{-1};
static std::mutex        g_state_mtx;
static ControllerState   g_state{};
static std::mutex        g_raw_state_mtx;
static ControllerState   g_raw_state{};

static std::atomic<bool> g_assistant_held{false};
static std::atomic<bool> g_capture_held{false};

static std::string       g_target_ip;
static std::mutex        g_controller_mtx;
static std::string       g_controller_path;
static std::string       g_controller_name;
static std::atomic<bool> g_controller_connected{false};
static std::mutex              g_wake_mtx;
static std::condition_variable g_wake_cv;

static std::string shell_exec(const std::string& cmd) {
    std::string result; FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result; char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static std::mutex g_log_mtx;
static void log_info(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mtx); va_list ap; va_start(ap, fmt);
    fprintf(stdout, "[INFO]  "); vfprintf(stdout, fmt, ap); fprintf(stdout, "\n"); fflush(stdout); va_end(ap);
}
static void log_err(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mtx); va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ERROR] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); fflush(stderr); va_end(ap);
}
static void signal_handler(int) { g_running = false; g_wake_cv.notify_all(); }

static int16_t scale_stick(int raw) {
    int centered = raw - 128;
    int scaled = (centered * 32767) / 127;
    return static_cast<int16_t>(std::clamp(scaled, -32767, 32767));
}

static uint16_t keycode_to_bit(uint16_t code) {
    switch (code) {
        case BTN_SOUTH: return BTN_BIT_A; case BTN_EAST: return BTN_BIT_B;
        case BTN_NORTH: return BTN_BIT_X; case BTN_WEST: return BTN_BIT_Y;
        case BTN_TL: return BTN_BIT_LB; case BTN_TR: return BTN_BIT_RB;
        case BTN_SELECT: return BTN_BIT_SELECT; case BTN_START: return BTN_BIT_START;
        case BTN_MODE: return BTN_BIT_STADIA; case BTN_THUMBL: return BTN_BIT_L3;
        case BTN_THUMBR: return BTN_BIT_R3; case BTN_TRIGGER_HAPPY1: return BTN_BIT_ASSISTANT;
        default: return 0;
    }
}

static void apply_event(const struct input_event& ev, ControllerState& st) {
    if (ev.type == EV_KEY) {
        uint16_t bit = keycode_to_bit(ev.code);
        if (bit) { if (ev.value) st.buttons |= bit; else st.buttons &= ~bit; }
    } else if (ev.type == EV_ABS) {
        switch (ev.code) {
            case ABS_X: st.stick_lx = scale_stick(ev.value); break;
            case ABS_Y: st.stick_ly = scale_stick(ev.value); break;
            case ABS_Z: st.stick_rx = scale_stick(ev.value); break;
            case ABS_RZ: st.stick_ry = scale_stick(ev.value); break;
            case ABS_BRAKE: st.trigger_left = static_cast<uint8_t>(std::clamp(ev.value, 0, 255)); break;
            case ABS_GAS: st.trigger_right = static_cast<uint8_t>(std::clamp(ev.value, 0, 255)); break;
            case ABS_HAT0X:
                st.buttons &= ~(BTN_BIT_DPAD_LEFT | BTN_BIT_DPAD_RIGHT);
                if (ev.value < 0) st.buttons |= BTN_BIT_DPAD_LEFT;
                else if (ev.value > 0) st.buttons |= BTN_BIT_DPAD_RIGHT; break;
            case ABS_HAT0Y:
                st.buttons &= ~(BTN_BIT_DPAD_UP | BTN_BIT_DPAD_DOWN);
                if (ev.value < 0) st.buttons |= BTN_BIT_DPAD_UP;
                else if (ev.value > 0) st.buttons |= BTN_BIT_DPAD_DOWN; break;
        }
    }
}

static constexpr uint16_t STADIA_VID = 0x18d1;
static constexpr uint16_t STADIA_PID = 0x9400;

static std::string find_stadia_evdev() {
    namespace fs = std::filesystem;
    if (!fs::exists("/dev/input/")) return {};
    for (auto& entry : fs::directory_iterator("/dev/input/")) {
        std::string path = entry.path().string();
        if (path.find("event") == std::string::npos) continue;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct input_id id{};
        if (ioctl(fd, EVIOCGID, &id) == 0) {
            if (id.vendor == STADIA_VID && id.product == STADIA_PID) {
                char name[256] = {}; ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                close(fd);
                log_info("Found Stadia controller: %s [%s]", name, path.c_str());
                { std::lock_guard<std::mutex> lk(g_controller_mtx); g_controller_name = name; }
                return path;
            }
        }
        close(fd);
    }
    return {};
}

static int ff_effect_id = -1;
static int rumble_hid_fd = -1;
static void ensure_rumble_hid_fd() {
    if (rumble_hid_fd >= 0) return;
    for (int i = 0; i < 15; ++i) {
        char path[256]; snprintf(path, sizeof(path), "/dev/hidraw%d", i);
        int test_fd = open(path, O_RDWR | O_NONBLOCK);
        if (test_fd >= 0) {
            struct hidraw_devinfo info;
            if (ioctl(test_fd, HIDIOCGRAWINFO, &info) == 0) {
                if ((uint16_t)info.vendor == 0x18d1 && (uint16_t)info.product == 0x9400) { rumble_hid_fd = test_fd; break; }
            }
            close(test_fd);
        }
    }
}
static void send_rumble(uint8_t strong, uint8_t weak) {
    ensure_rumble_hid_fd();
    if (rumble_hid_fd >= 0) {
        uint8_t report[5] = { 0x05, strong, strong, weak, weak };
        if (write(rumble_hid_fd, report, 5) < 0) { close(rumble_hid_fd); rumble_hid_fd = -1; }
    }
}
static void ff_upload_and_play(int, uint8_t strong, uint8_t weak) { send_rumble(strong, weak); }
static void ff_stop(int fd) {
    if (ff_effect_id < 0) return;
    struct input_event stop{}; stop.type = EV_FF; stop.code = static_cast<uint16_t>(ff_effect_id); stop.value = 0;
    write(fd, &stop, sizeof(stop));
}

static int create_udp_socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) log_err("socket(UDP) failed: %s", strerror(errno));
    return fd;
}
static int create_udp_recv_socket(uint16_t port) {
    int fd = create_udp_socket(); if (fd < 0) return -1;
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_err("bind(UDP %u) failed: %s", port, strerror(errno)); close(fd); return -1;
    }
    return fd;
}
static void send_udp_code(const char* win_ip, const char* msg) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0); if (sock < 0) return;
    struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(PORT_MACRO);
    inet_pton(AF_INET, win_ip, &addr.sin_addr);
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
}

static void input_sender_thread() {
    int udp_fd = create_udp_socket(); if (udp_fd < 0) return;
    struct sockaddr_in dest{}; dest.sin_family = AF_INET; dest.sin_port = htons(PORT_INPUT);
    inet_pton(AF_INET, g_target_ip.c_str(), &dest.sin_addr);

    while (g_running) {
        std::string evpath;
        {
            std::unique_lock<std::mutex> lk(g_wake_mtx);
            while (g_running) {
                evpath = find_stadia_evdev(); if (!evpath.empty()) break;
                g_wake_cv.wait_for(lk, std::chrono::seconds(3));
            }
        }
        if (!g_running) break;

        int ev_fd = open(evpath.c_str(), O_RDWR | O_NONBLOCK);
        if (ev_fd < 0) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }

        ioctl(ev_fd, EVIOCGRAB, 1);
        { std::lock_guard<std::mutex> lk(g_controller_mtx); g_controller_path = evpath; }
        g_evdev_fd.store(ev_fd); g_controller_connected.store(true); ff_effect_id = -1;

        { std::lock_guard<std::mutex> lk(g_state_mtx); std::memset(&g_state, 0, sizeof(g_state)); }

        struct pollfd pfd{}; pfd.fd = ev_fd; pfd.events = POLLIN;
        bool connected = true;

        while (g_running && connected) {
            int ret = poll(&pfd, 1, 500);
            if (ret < 0) { if (errno == EINTR) continue; break; }
            if (ret == 0) continue;

            struct input_event events[64];
            ssize_t rd = read(ev_fd, events, sizeof(events));
            if (rd <= 0) { connected = false; break; }

            size_t count = static_cast<size_t>(rd) / sizeof(struct input_event);
            bool changed = false;

            {
                std::lock_guard<std::mutex> lk(g_state_mtx);
                for (size_t i = 0; i < count; ++i) {
                    if (events[i].type == EV_SYN) {
                        if (changed) {
                            ControllerState send_state = g_state;
                            if (g_assistant_held.load() || g_capture_held.load()) {
                                send_state.buttons &= ~CHORD_SUPPRESS_MASK;
                                send_state.trigger_left = 0;
                                send_state.trigger_right = 0;
                            }
                            { std::lock_guard<std::mutex> rlk(g_raw_state_mtx); g_raw_state = g_state; }
                            sendto(udp_fd, &send_state, sizeof(send_state), 0, (struct sockaddr*)&dest, sizeof(dest));
                            changed = false;
                        }
                    } else {
                        apply_event(events[i], g_state); changed = true;
                    }
                }
                if (changed) {
                    ControllerState send_state = g_state;
                    if (g_assistant_held.load() || g_capture_held.load()) {
                        send_state.buttons &= ~CHORD_SUPPRESS_MASK;
                        send_state.trigger_left = 0;
                        send_state.trigger_right = 0;
                    }
                    { std::lock_guard<std::mutex> rlk(g_raw_state_mtx); g_raw_state = g_state; }
                    sendto(udp_fd, &send_state, sizeof(send_state), 0, (struct sockaddr*)&dest, sizeof(dest));
                }
            }
        }
        g_controller_connected.store(false); g_evdev_fd.store(-1);
        { std::lock_guard<std::mutex> lk(g_controller_mtx); g_controller_path.clear(); }
        ff_stop(ev_fd); ioctl(ev_fd, EVIOCGRAB, 0); close(ev_fd);
    }
    close(udp_fd);
}

static void rumble_receiver_thread() {
    int udp_fd = create_udp_recv_socket(PORT_RUMBLE); if (udp_fd < 0) return;
    struct timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0; setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (g_running) {
        RumbleState rs{}; ssize_t n = recv(udp_fd, &rs, sizeof(rs), 0);
        if (n == sizeof(rs)) { int fd = g_evdev_fd.load(); if (fd >= 0) ff_upload_and_play(fd, rs.motor_left, rs.motor_right); }
    }
    close(udp_fd);
}

void start_extra_buttons_thread(const char* win_ip) {
    std::thread([win_ip] {
        int hid_fd = -1;
        while (hid_fd < 0) {
            for (int i = 0; i < 15; ++i) {
                char path[256]; snprintf(path, sizeof(path), "/dev/hidraw%d", i);
                int test_fd = open(path, O_RDONLY);
                if (test_fd >= 0) {
                    struct hidraw_devinfo info;
                    if (ioctl(test_fd, HIDIOCGRAWINFO, &info) == 0 && (uint16_t)info.vendor == 0x18d1 && (uint16_t)info.product == 0x9400) {
                        hid_fd = test_fd; break;
                    }
                    close(test_fd);
                }
            }
            if (hid_fd < 0) sleep(1);
        }

        send_rumble(128, 128); usleep(500000); send_rumble(0, 0);

        struct ChordSlot {
            const char* code; bool held; bool sent_once;
            std::chrono::steady_clock::time_point first_fire;
            std::chrono::steady_clock::time_point last_fire;
        };

        #define NUM_CHORDS 17
        ChordSlot ast_slots[NUM_CHORDS] = {
            {"A_A",0,0,{},{}}, {"A_B",0,0,{},{}}, {"A_X",0,0,{},{}}, {"A_Y",0,0,{},{}},
            {"A_UP",0,0,{},{}}, {"A_DOWN",0,0,{},{}}, {"A_LEFT",0,0,{},{}}, {"A_RIGHT",0,0,{},{}},
            {"A_LB",0,0,{},{}}, {"A_RB",0,0,{},{}}, {"A_L2",0,0,{},{}}, {"A_R2",0,0,{},{}},
            {"A_L3",0,0,{},{}}, {"A_R3",0,0,{},{}},
            {"A_SELECT",0,0,{},{}}, {"A_START",0,0,{},{}}, {"A_STADIA",0,0,{},{}}
        };
        ChordSlot cap_slots[NUM_CHORDS] = {
            {"C_A",0,0,{},{}}, {"C_B",0,0,{},{}}, {"C_X",0,0,{},{}}, {"C_Y",0,0,{},{}},
            {"C_UP",0,0,{},{}}, {"C_DOWN",0,0,{},{}}, {"C_LEFT",0,0,{},{}}, {"C_RIGHT",0,0,{},{}},
            {"C_LB",0,0,{},{}}, {"C_RB",0,0,{},{}}, {"C_L2",0,0,{},{}}, {"C_R2",0,0,{},{}},
            {"C_L3",0,0,{},{}}, {"C_R3",0,0,{},{}},
            {"C_SELECT",0,0,{},{}}, {"C_START",0,0,{},{}}, {"C_STADIA",0,0,{},{}}
        };

        constexpr auto REPEAT_DELAY = std::chrono::milliseconds(350);
        constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(100);

        auto fire_slot = [&](ChordSlot& slot) {
            auto now = std::chrono::steady_clock::now();
            if (!slot.sent_once) {
                send_udp_code(win_ip, slot.code);
                slot.first_fire = now; slot.last_fire = now; slot.sent_once = true;
            } else if (now - slot.first_fire >= REPEAT_DELAY && now - slot.last_fire >= REPEAT_INTERVAL) {
                send_udp_code(win_ip, slot.code); slot.last_fire = now;
            }
        };

        uint8_t buf[32];
        while (true) {
            struct pollfd pfd; pfd.fd = hid_fd; pfd.events = POLLIN;
            int ret = poll(&pfd, 1, 50);
            if (ret < 0) { sleep(1); continue; }
            if (ret > 0) { int n = read(hid_fd, buf, sizeof(buf)); if (n < 6) continue; }

            bool ast = (buf[2] & 0x02) != 0;
            bool cap = (buf[2] & 0x01) != 0;
            g_assistant_held.store(ast); g_capture_held.store(cap);

            uint16_t b = 0; uint8_t l2_val = 0; uint8_t r2_val = 0;
            {
                std::lock_guard<std::mutex> rlk(g_raw_state_mtx);
                b = g_raw_state.buttons;
                l2_val = g_raw_state.trigger_left;
                r2_val = g_raw_state.trigger_right;
            }

            bool st_a = (b & BTN_BIT_A) != 0; bool st_b = (b & BTN_BIT_B) != 0;
            bool st_x = (b & BTN_BIT_X) != 0; bool st_y = (b & BTN_BIT_Y) != 0;
            bool d_up = (b & BTN_BIT_DPAD_UP) != 0; bool d_dn = (b & BTN_BIT_DPAD_DOWN) != 0;
            bool d_lf = (b & BTN_BIT_DPAD_LEFT) != 0; bool d_rt = (b & BTN_BIT_DPAD_RIGHT) != 0;
            bool st_lb = (b & BTN_BIT_LB) != 0; bool st_rb = (b & BTN_BIT_RB) != 0;
            bool st_l2 = l2_val > 128; bool st_r2 = r2_val > 128;
            bool st_l3 = (b & BTN_BIT_L3) != 0; bool st_r3 = (b & BTN_BIT_R3) != 0;
            bool st_sel = (b & BTN_BIT_SELECT) != 0; bool st_sta = (b & BTN_BIT_START) != 0;
            bool st_home = (b & BTN_BIT_STADIA) != 0;

            bool active_arr[NUM_CHORDS] = { st_a, st_b, st_x, st_y, d_up, d_dn, d_lf, d_rt, st_lb, st_rb, st_l2, st_r2, st_l3, st_r3, st_sel, st_sta, st_home };

            bool any_ast = false; bool any_cap = false;

            for (int i = 0; i < NUM_CHORDS; ++i) {
                if (ast && active_arr[i]) { ast_slots[i].held = true; fire_slot(ast_slots[i]); any_ast = true; } 
                else { ast_slots[i].held = false; ast_slots[i].sent_once = false; }

                if (cap && active_arr[i]) { cap_slots[i].held = true; fire_slot(cap_slots[i]); any_cap = true; } 
                else { cap_slots[i].held = false; cap_slots[i].sent_once = false; }
            }

            static bool ast_solo_sent = false; static bool cap_solo_sent = false;
            if (ast && !any_ast) { if (!ast_solo_sent) { send_udp_code(win_ip, "A"); ast_solo_sent = true; } } else ast_solo_sent = false;
            if (cap && !any_cap) { if (!cap_solo_sent) { send_udp_code(win_ip, "C"); cap_solo_sent = true; } } else cap_solo_sent = false;
        }
    }).detach();
}

static std::string bt_scan(int seconds = 8) { return shell_exec("timeout " + std::to_string(seconds + 2) + " bluetoothctl --timeout " + std::to_string(seconds) + " scan on 2>&1; bluetoothctl devices 2>&1"); }
static std::string bt_pair(const std::string& mac) { std::ostringstream oss; oss << "trust: " << shell_exec("bluetoothctl trust " + mac + " 2>&1") << "\npair: " << shell_exec("timeout 15 bluetoothctl pair " + mac + " 2>&1") << "\nconnect: " << shell_exec("bluetoothctl connect " + mac + " 2>&1") << "\n"; return oss.str(); }
static std::string bt_connect(const std::string& mac) { return shell_exec("bluetoothctl connect " + mac + " 2>&1"); }
static std::string bt_disconnect(const std::string& mac) { return shell_exec("bluetoothctl disconnect " + mac + " 2>&1"); }
static std::string bt_remove(const std::string& mac) { return shell_exec("bluetoothctl remove " + mac + " 2>&1"); }
static std::string bt_info(const std::string& mac) { return shell_exec("bluetoothctl info " + mac + " 2>&1"); }
static std::string bt_devices() { return shell_exec("bluetoothctl devices 2>&1"); }

static void handle_c2_client(int client_fd) {
    auto send_reply = [&](const std::string& msg) { std::string out = msg + "\n"; send(client_fd, out.c_str(), out.size(), MSG_NOSIGNAL); };
    send_reply("STADIA_BRIDGE READY");
    char buf[4096]; std::string leftover;
    while (g_running) {
        struct pollfd pfd{}; pfd.fd = client_fd; pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 1000); if (ret <= 0) { if (ret == 0 || errno == EINTR) continue; break; }
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0); if (n <= 0) break;
        buf[n] = '\0'; leftover += buf;
        size_t pos;
        while ((pos = leftover.find('\n')) != std::string::npos) {
            std::string line = leftover.substr(0, pos); leftover.erase(0, pos + 1);
            if (line == "SHUTDOWN") { send_reply("BYE"); g_running = false; g_wake_cv.notify_all(); break; }
            send_reply("ACK");
        }
    }
    close(client_fd);
}

static void c2_server_thread() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); if (server_fd < 0) return;
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(PORT_C2);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server_fd, 4) < 0) { close(server_fd); return; }
    while (g_running) {
        struct pollfd pfd{}; pfd.fd = server_fd; pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 1000); if (ret <= 0) continue;
        struct sockaddr_in client_addr{}; socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) std::thread(handle_c2_client, client_fd).detach();
    }
    close(server_fd);
}

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;
    g_target_ip = argv[1];
    start_extra_buttons_thread(argv[1]);
    std::thread t_input(input_sender_thread);
    std::thread t_rumble(rumble_receiver_thread);
    std::thread t_c2(c2_server_thread);
    t_input.join(); t_rumble.join(); t_c2.join();
    return 0;
}