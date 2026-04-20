// stadia_receiver.cpp — Windows-side receiver for Stadia Network Bridge
// Receives packed ControllerState via UDP, presents a virtual Xbox 360 pad
// via ViGEmBus, sends RumbleState back, and handles macro shortcuts.
//
// Build:
//   cl /std:c++17 /EHsc /O2 /MD /I "ViGEmClient_Release\include" stadia_receiver.cpp
//      /link /LIBPATH:"ViGEmClient_Release\lib" ViGEmClient.lib ws2_32.lib setupapi.lib user32.lib
//
// Run: stadia_receiver.exe <linux_bridge_ip>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <ViGEm/Client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <ctype.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------------------
// Globals & Logging
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{true};
static std::string       g_bridge_ip;
static SOCKET            g_rumble_sock = INVALID_SOCKET;
static sockaddr_in       g_rumble_dest{};
static std::mutex        g_log_mtx;

static void log_info(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    va_list ap; va_start(ap, fmt);
    printf("[INFO]  "); vprintf(fmt, ap); printf("\n");
    fflush(stdout); va_end(ap);
}
static void log_err(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ERROR] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    fflush(stderr); va_end(ap);
}

// ---------------------------------------------------------------------------
// Macro Listener — reads stadia_buttons.ini and fires SendInput combos
//
// HOLD-TO-REPEAT: The listener tracks when a macro code was first received
// and how many times it has repeated. This mirrors the Linux-side repeat
// logic: first packet = instant fire; then Windows-side hold-to-repeat
// adds ANOTHER layer of protection so even if repeat packets arrive fast,
// we debounce them correctly.
//
// In practice the Linux bridge already handles timing and sends repeat
// packets at 100ms intervals after a 350ms initial delay. The receiver
// simply fires SendInput for every packet it gets. No extra debouncing
// needed here — just a clean fire-on-receive loop.
// ---------------------------------------------------------------------------

#define CONFIG_FILE "stadia_buttons.ini"
#define MAX_LINE    256
#define MAX_CODE_LEN 16

#define MY_MOD_ALT     1
#define MY_MOD_CONTROL 2
#define MY_MOD_SHIFT   4
#define MY_MOD_WIN     8

struct KeyMapping {
    char code[MAX_CODE_LEN];
    WORD modifiers;
    WORD mainKey;
    bool repeat;   // true = allow hold-to-repeat (volume/media); false = one-shot
};

#define MAX_MAPPINGS 128
static KeyMapping g_mappings[MAX_MAPPINGS];
static int        g_mappingCount = 0;

struct KeyNameMap { const char* name; WORD vk; };
static KeyNameMap keyNames[] = {
    {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},
    {"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},
    {"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
    {"0",'0'},{"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},
    {"5",'5'},{"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},
    {"A",'A'},{"B",'B'},{"C",'C'},{"D",'D'},{"E",'E'},
    {"F",'F'},{"G",'G'},{"H",'H'},{"I",'I'},{"J",'J'},
    {"K",'K'},{"L",'L'},{"M",'M'},{"N",'N'},{"O",'O'},
    {"P",'P'},{"Q",'Q'},{"R",'R'},{"S",'S'},{"T",'T'},
    {"U",'U'},{"V",'V'},{"W",'W'},{"X",'X'},{"Y",'Y'},{"Z",'Z'},
    {"TAB",VK_TAB},{"ESC",VK_ESCAPE},{"ESCAPE",VK_ESCAPE},
    {"SPACE",VK_SPACE},{"ENTER",VK_RETURN},{"RETURN",VK_RETURN},
    {"BACKSPACE",VK_BACK},{"DELETE",VK_DELETE},{"INSERT",VK_INSERT},
    {"HOME",VK_HOME},{"END",VK_END},{"PAGEUP",VK_PRIOR},{"PAGEDOWN",VK_NEXT},
    {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
    {"PRINTSCREEN",VK_SNAPSHOT},{"SCROLLLOCK",VK_SCROLL},{"PAUSE",VK_PAUSE},
    {"CAPSLOCK",VK_CAPITAL},{"NUMLOCK",VK_NUMLOCK},{"APPS",VK_APPS},
    {"VOLUME_UP",VK_VOLUME_UP},{"VOLUME_DOWN",VK_VOLUME_DOWN},{"VOLUME_MUTE",VK_VOLUME_MUTE},
    {"MEDIA_NEXT",VK_MEDIA_NEXT_TRACK},{"MEDIA_PREV",VK_MEDIA_PREV_TRACK},
    {"MEDIA_PLAY_PAUSE",VK_MEDIA_PLAY_PAUSE},{"MEDIA_STOP",VK_MEDIA_STOP},
    {"NEXT_TRACK",VK_MEDIA_NEXT_TRACK},{"PREV_TRACK",VK_MEDIA_PREV_TRACK},
    {"LWIN",VK_LWIN},{"RWIN",VK_RWIN},
    {"LCONTROL",VK_LCONTROL},{"RCONTROL",VK_RCONTROL},
    {"LMENU",VK_LMENU},{"RMENU",VK_RMENU},
    {"SHIFT",VK_SHIFT},{"CONTROL",VK_CONTROL},{"ALT",VK_MENU},
    {nullptr,0}
};

// Keys that are media/volume — these benefit from hold-to-repeat
static bool is_repeatable_vk(WORD vk) {
    return vk == VK_VOLUME_UP   || vk == VK_VOLUME_DOWN ||
           vk == VK_VOLUME_MUTE || vk == VK_MEDIA_NEXT_TRACK ||
           vk == VK_MEDIA_PREV_TRACK || vk == VK_MEDIA_PLAY_PAUSE ||
           vk == VK_MEDIA_STOP  || vk == VK_UP  || vk == VK_DOWN ||
           vk == VK_LEFT || vk == VK_RIGHT;
}

static WORD nameToVk(const char* name) {
    if (!name) return 0;
    for (int i = 0; keyNames[i].name; i++)
        if (_stricmp(name, keyNames[i].name) == 0) return keyNames[i].vk;
    return 0;
}

static void parseShortcut(const char* shortcut, WORD* modifiers, WORD* mainKey) {
    *modifiers = 0; *mainKey = 0;
    if (!shortcut || !*shortcut) return;
    char* copy  = _strdup(shortcut);
    char* token = strtok(copy, "+");
    while (token) {
        while (*token == ' ') token++;
        char* end = token + strlen(token) - 1;
        while (end >= token && *end == ' ') end--;
        *(end+1) = '\0';
        if      (_stricmp(token,"CTRL")==0||_stricmp(token,"CONTROL")==0) *modifiers |= MY_MOD_CONTROL;
        else if (_stricmp(token,"ALT")==0)                                *modifiers |= MY_MOD_ALT;
        else if (_stricmp(token,"SHIFT")==0)                              *modifiers |= MY_MOD_SHIFT;
        else if (_stricmp(token,"LWIN")==0||_stricmp(token,"RWIN")==0||_stricmp(token,"WIN")==0) *modifiers |= MY_MOD_WIN;
        else { WORD vk = nameToVk(token); if (vk) *mainKey = vk; }
        token = strtok(nullptr, "+");
    }
    free(copy);
}

static void loadConfig() {
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) { log_err("Config %s not found, shortcuts disabled.", CONFIG_FILE); return; }
    char line[MAX_LINE];
    int inSection = 0;
    while (fgets(line, sizeof(line), f)) {
        char* comment = strpbrk(line, ";#");
        if (comment) *comment = '\0';
        char* start = line;
        while (*start==' '||*start=='\t') start++;
        if (!*start||*start=='\r'||*start=='\n') continue;
        if (*start == '[') {
            char* end = strchr(start, ']');
            if (end) { *end='\0'; inSection=(_stricmp(start+1,"Buttons")==0); }
            continue;
        }
        if (!inSection) continue;
        char* eq = strchr(start, '=');
        if (!eq) continue;
        *eq = '\0';
        char* code = start, *shortcut = eq+1;
        // Trim code
        while (*code==' '||*code=='\t') code++;
        char* ce = code+strlen(code)-1;
        while (ce>=code&&(*ce==' '||*ce=='\t'||*ce=='\r'||*ce=='\n')) { *ce='\0'; ce--; }
        // Trim shortcut
        while (*shortcut==' '||*shortcut=='\t') shortcut++;
        char* se = shortcut+strlen(shortcut)-1;
        while (se>=shortcut&&(*se==' '||*se=='\t'||*se=='\r'||*se=='\n')) { *se='\0'; se--; }
        if (strlen(code)>=MAX_CODE_LEN) continue;
        if (g_mappingCount >= MAX_MAPPINGS) break;
        KeyMapping* map = &g_mappings[g_mappingCount++];
        strcpy(map->code, code);
        parseShortcut(shortcut, &map->modifiers, &map->mainKey);
        map->repeat = is_repeatable_vk(map->mainKey);
    }
    fclose(f);
    log_info("Loaded %d shortcut mappings from %s", g_mappingCount, CONFIG_FILE);
    for (int i = 0; i < g_mappingCount; i++)
        log_info("  [%s] -> vk=0x%04X mods=0x%02X repeat=%s",
                 g_mappings[i].code, g_mappings[i].mainKey,
                 g_mappings[i].modifiers, g_mappings[i].repeat ? "yes" : "no");
}

static void press_combo(const KeyMapping* map) {
    if (map->modifiers == 0 && map->mainKey == 0) return;
    INPUT inputs[8] = {};
    int count = 0;
    if (map->modifiers & MY_MOD_ALT)     { inputs[count].type=INPUT_KEYBOARD; inputs[count++].ki.wVk=VK_MENU; }
    if (map->modifiers & MY_MOD_CONTROL) { inputs[count].type=INPUT_KEYBOARD; inputs[count++].ki.wVk=VK_CONTROL; }
    if (map->modifiers & MY_MOD_SHIFT)   { inputs[count].type=INPUT_KEYBOARD; inputs[count++].ki.wVk=VK_SHIFT; }
    if (map->modifiers & MY_MOD_WIN)     { inputs[count].type=INPUT_KEYBOARD; inputs[count++].ki.wVk=VK_LWIN; }
    if (map->mainKey)                    { inputs[count].type=INPUT_KEYBOARD; inputs[count++].ki.wVk=map->mainKey; }
    SendInput(count, inputs, sizeof(INPUT));
    Sleep(20);
    for (int i=0;i<count;i++) inputs[i].ki.dwFlags=KEYEVENTF_KEYUP;
    SendInput(count, inputs, sizeof(INPUT));
}

static void macro_listener_thread() {
    loadConfig();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(45499); addr.sin_addr.s_addr=INADDR_ANY;
    bind(s, (sockaddr*)&addr, sizeof(addr));

    // Each macro slot tracks last-received time to avoid double-firing
    // if the OS somehow delivers the same UDP packet twice.
    struct SlotState {
        std::chrono::steady_clock::time_point last_received;
        bool ever_received = false;
    };
    SlotState slot_state[MAX_MAPPINGS] = {};

    char buf[32];
    while (g_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        timeval tv = {1, 0};
        if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
            int len = recvfrom(s, buf, sizeof(buf)-1, 0, nullptr, nullptr);
            if (len > 0) {
                buf[len] = '\0';
                auto now = std::chrono::steady_clock::now();
                for (int i = 0; i < g_mappingCount; i++) {
                    if (strcmp(buf, g_mappings[i].code) == 0) {
                        SlotState& ss = slot_state[i];

                        if (g_mappings[i].repeat) {
                            // For repeatable keys: fire every packet unconditionally.
                            // The Linux bridge already handles timing/gating.
                            press_combo(&g_mappings[i]);
                        } else {
                            // For one-shot keys: only fire if >200ms since last
                            // to prevent accidental double-trigger from network jitter.
                            auto elapsed = ss.ever_received
                                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - ss.last_received).count()
                                : 9999;
                            if (elapsed > 200) {
                                press_combo(&g_mappings[i]);
                            }
                        }

                        ss.last_received  = now;
                        ss.ever_received  = true;
                        break;
                    }
                }
            }
        }
    }
    closesocket(s);
}

// ---------------------------------------------------------------------------
// Original Receiver Code (ViGEm Xbox 360 emulation)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct ControllerState {
    uint16_t buttons; uint8_t trigger_left; uint8_t trigger_right;
    int16_t stick_lx; int16_t stick_ly; int16_t stick_rx; int16_t stick_ry;
};
struct RumbleState { uint8_t motor_left; uint8_t motor_right; };
#pragma pack(pop)

enum ButtonBit : uint16_t {
    BTN_BIT_A=1<<0,BTN_BIT_B=1<<1,BTN_BIT_X=1<<2,BTN_BIT_Y=1<<3,
    BTN_BIT_LB=1<<4,BTN_BIT_RB=1<<5,BTN_BIT_SELECT=1<<6,BTN_BIT_START=1<<7,
    BTN_BIT_STADIA=1<<8,BTN_BIT_L3=1<<9,BTN_BIT_R3=1<<10,BTN_BIT_ASSISTANT=1<<11,
    BTN_BIT_DPAD_UP=1<<12,BTN_BIT_DPAD_DOWN=1<<13,BTN_BIT_DPAD_LEFT=1<<14,BTN_BIT_DPAD_RIGHT=1<<15,
};

static constexpr uint16_t PORT_INPUT  = 45493;
static constexpr uint16_t PORT_RUMBLE = 45494;

static XUSB_REPORT map_to_xusb(const ControllerState& cs) {
    XUSB_REPORT report; XUSB_REPORT_INIT(&report);
    USHORT xb = 0;
    if (cs.buttons&BTN_BIT_A)          xb|=XUSB_GAMEPAD_A;
    if (cs.buttons&BTN_BIT_B)          xb|=XUSB_GAMEPAD_B;
    if (cs.buttons&BTN_BIT_X)          xb|=XUSB_GAMEPAD_X;
    if (cs.buttons&BTN_BIT_Y)          xb|=XUSB_GAMEPAD_Y;
    if (cs.buttons&BTN_BIT_LB)         xb|=XUSB_GAMEPAD_LEFT_SHOULDER;
    if (cs.buttons&BTN_BIT_RB)         xb|=XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (cs.buttons&BTN_BIT_SELECT)     xb|=XUSB_GAMEPAD_BACK;
    if (cs.buttons&BTN_BIT_START)      xb|=XUSB_GAMEPAD_START;
    if (cs.buttons&BTN_BIT_STADIA)     xb|=XUSB_GAMEPAD_GUIDE;
    if (cs.buttons&BTN_BIT_L3)         xb|=XUSB_GAMEPAD_LEFT_THUMB;
    if (cs.buttons&BTN_BIT_R3)         xb|=XUSB_GAMEPAD_RIGHT_THUMB;
    if (cs.buttons&BTN_BIT_DPAD_UP)    xb|=XUSB_GAMEPAD_DPAD_UP;
    if (cs.buttons&BTN_BIT_DPAD_DOWN)  xb|=XUSB_GAMEPAD_DPAD_DOWN;
    if (cs.buttons&BTN_BIT_DPAD_LEFT)  xb|=XUSB_GAMEPAD_DPAD_LEFT;
    if (cs.buttons&BTN_BIT_DPAD_RIGHT) xb|=XUSB_GAMEPAD_DPAD_RIGHT;
    report.wButtons = xb;
    report.bLeftTrigger  = cs.trigger_left;
    report.bRightTrigger = cs.trigger_right;
    report.sThumbLX = cs.stick_lx;
    report.sThumbLY = (cs.stick_ly==-32767) ? 32767 : (SHORT)(-cs.stick_ly);
    report.sThumbRX = cs.stick_rx;
    report.sThumbRY = (cs.stick_ry==-32767) ? 32767 : (SHORT)(-cs.stick_ry);
    return report;
}

static VOID CALLBACK vigem_rumble_callback(PVIGEM_CLIENT,PVIGEM_TARGET,UCHAR l,UCHAR s,UCHAR,LPVOID) {
    RumbleState rs; rs.motor_left=l; rs.motor_right=s;
    if (g_rumble_sock!=INVALID_SOCKET)
        sendto(g_rumble_sock,(const char*)&rs,sizeof(rs),0,(const sockaddr*)&g_rumble_dest,sizeof(g_rumble_dest));
}

static void input_receiver_thread(PVIGEM_CLIENT client, PVIGEM_TARGET pad) {
    SOCKET sock = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    DWORD to=1000; setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char*)&to,sizeof(to));
    sockaddr_in bind_addr{}; bind_addr.sin_family=AF_INET; bind_addr.sin_addr.s_addr=INADDR_ANY; bind_addr.sin_port=htons(PORT_INPUT);
    bind(sock,(sockaddr*)&bind_addr,sizeof(bind_addr));

    while (g_running) {
        ControllerState cs{};
        int n = recv(sock,(char*)&cs,sizeof(cs),0);
        if (n==sizeof(cs)) vigem_target_x360_update(client,pad,map_to_xusb(cs));
    }
    closesocket(sock);
}

static BOOL WINAPI console_handler(DWORD signal) {
    if (signal==CTRL_C_EVENT||signal==CTRL_CLOSE_EVENT) { g_running=false; return TRUE; }
    return FALSE;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <linux_bridge_ip>\n", argv[0]);
        return 1;
    }
    g_bridge_ip = argv[1];
    WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa);
    SetConsoleCtrlHandler(console_handler,TRUE);

    log_info("Stadia Receiver starting — bridge at %s", g_bridge_ip.c_str());

    PVIGEM_CLIENT client = vigem_alloc();
    if (!client || !VIGEM_SUCCESS(vigem_connect(client))) {
        log_err("ViGEmBus init failed — is the driver installed?");
        return 1;
    }
    PVIGEM_TARGET pad = vigem_target_x360_alloc();
    vigem_target_add(client, pad);
    vigem_target_x360_register_notification(client, pad, vigem_rumble_callback, nullptr);

    g_rumble_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    g_rumble_dest.sin_family=AF_INET; g_rumble_dest.sin_port=htons(PORT_RUMBLE);
    inet_pton(AF_INET, g_bridge_ip.c_str(), &g_rumble_dest.sin_addr);

    std::thread t_input(input_receiver_thread, client, pad);
    std::thread t_macro(macro_listener_thread);

    log_info("Receiver running. Press Ctrl+C to exit.");
    if (t_input.joinable()) t_input.join();
    if (t_macro.joinable()) t_macro.join();

    vigem_target_x360_unregister_notification(pad);
    vigem_target_remove(client,pad); vigem_target_free(pad);
    vigem_disconnect(client); vigem_free(client);
    closesocket(g_rumble_sock); WSACleanup();
    return 0;
}