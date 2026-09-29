// Long After Dark — Linux Screen Saver and Module Player.
// Connects to adhostwin.exe via Wine, reads P8 frames over stdout,
// handles input (keys, Caps Lock games, mouse) over stdin,
// and renders to an X11 window, full-screen, or embedded in xscreensaver (-window-id).
// See docs/DESIGN.md and scr/README.md.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#ifdef HAS_XSHM
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../../importer/minijson.h"

namespace fs = std::filesystem;

namespace {

const char* kVersion = "1.0.0";

struct ModuleInfo {
  std::string id;
  std::string name;
  std::string path;
  std::string package;
  std::string about;
};

struct Catalog {
  std::vector<ModuleInfo> modules;
};

// Global shutdown flag for signals
std::atomic<bool> g_shutdown{false};
pid_t g_child_pid = 0;

void sig_handler(int) {
  g_shutdown.store(true);
  if (g_child_pid > 0) {
    kill(g_child_pid, SIGTERM);
  }
}

std::string find_wine_bin() {
  const char* env_wine = getenv("AD_WINE_BIN");
  if (env_wine && *env_wine) return env_wine;
  if (system("which wine >/dev/null 2>&1") == 0) return "wine";
  if (system("which wine64 >/dev/null 2>&1") == 0) return "wine64";
  if (fs::exists("/usr/lib/wine/wine64")) return "/usr/lib/wine/wine64";
  return "wine";
}

// Convert X11 KeySym to Windows Virtual-Key code for After Dark games
int xkeysym_to_vk(KeySym sym) {
  if (sym >= XK_0 && sym <= XK_9) return 0x30 + int(sym - XK_0);
  if (sym >= XK_a && sym <= XK_z) return 0x41 + int(sym - XK_a);
  if (sym >= XK_A && sym <= XK_Z) return 0x41 + int(sym - XK_A);
  switch (sym) {
    case XK_BackSpace: return 0x08;
    case XK_Tab: return 0x09;
    case XK_Clear: return 0x0C;
    case XK_Return: return 0x0D;
    case XK_Shift_L:
    case XK_Shift_R: return 0x10;
    case XK_Control_L:
    case XK_Control_R: return 0x11;
    case XK_Alt_L:
    case XK_Alt_R: return 0x12;
    case XK_Pause: return 0x13;
    case XK_Caps_Lock: return 0x14;
    case XK_Escape: return 0x1B;
    case XK_space: return 0x20;
    case XK_Page_Up: return 0x21;
    case XK_Page_Down: return 0x22;
    case XK_End: return 0x23;
    case XK_Home: return 0x24;
    case XK_Left: return 0x25;
    case XK_Up: return 0x26;
    case XK_Right: return 0x27;
    case XK_Down: return 0x28;
    case XK_Select: return 0x29;
    case XK_Print: return 0x2A;
    case XK_Execute: return 0x2B;
    case XK_Insert: return 0x2D;
    case XK_Delete: return 0x2E;
    case XK_Help: return 0x2F;
    case XK_F1: return 0x70;
    case XK_F2: return 0x71;
    case XK_F3: return 0x72;
    case XK_F4: return 0x73;
    case XK_F5: return 0x74;
    case XK_F6: return 0x75;
    case XK_F7: return 0x76;
    case XK_F8: return 0x77;
    case XK_F9: return 0x78;
    case XK_F10: return 0x79;
    case XK_F11: return 0x7A;
    case XK_F12: return 0x7B;
    default: return 0;
  }
}

// Locate the home directory
fs::path get_home_dir() {
  const char* h = getenv("HOME");
  if (h && *h) return fs::path(h);
  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_dir) return fs::path(pw->pw_dir);
  return fs::path("/");
}

// Find assets/win root
fs::path find_assets_dir(const std::string& override_dir) {
  if (!override_dir.empty()) {
    fs::path p(override_dir);
    if (fs::exists(p / "catalog-win.json")) return p;
    if (fs::exists(p / "win" / "catalog-win.json")) return p / "win";
    return p;
  }

  const char* env_assets = getenv("AD_ASSETS_DIR");
  if (env_assets && *env_assets) {
    fs::path p(env_assets);
    if (fs::exists(p / "catalog-win.json")) return p;
    if (fs::exists(p / "win" / "catalog-win.json")) return p / "win";
  }

  fs::path home = get_home_dir();
  // Check wine prefix default
  fs::path wine_path = home / ".wine" / "drive_c" / "users";
  if (fs::exists(wine_path)) {
    for (const auto& entry : fs::directory_iterator(wine_path)) {
      fs::path candidate = entry.path() / "AppData" / "Local" / "LongAfterDark" / "assets" / "win";
      if (fs::exists(candidate / "catalog-win.json")) return candidate;
    }
  }

  // Check XDG data dir
  fs::path xdg_path = home / ".local" / "share" / "LongAfterDark" / "assets" / "win";
  if (fs::exists(xdg_path / "catalog-win.json")) return xdg_path;

  // Check local assets
  if (fs::exists("assets/win/catalog-win.json")) return fs::absolute("assets/win");
  if (fs::exists("../assets/win/catalog-win.json")) return fs::absolute("../assets/win");

  return wine_path; // Fallback
}

// Find adhostwin.exe binary
fs::path find_adhost_exe(const fs::path& exe_dir) {
  const char* env_host = getenv("AD_HOST_EXE");
  if (env_host && *env_host && fs::exists(env_host)) return fs::path(env_host);

  // Look in the same folder as this binary
  if (!exe_dir.empty() && fs::exists(exe_dir / "adhostwin.exe")) {
    return exe_dir / "adhostwin.exe";
  }

  // Look in build directories
  if (fs::exists("build/dist/LongAfterDark/adhostwin.exe")) return fs::absolute("build/dist/LongAfterDark/adhostwin.exe");
  if (fs::exists("build/win/host/core/adhostwin.exe")) return fs::absolute("build/win/host/core/adhostwin.exe");
  if (fs::exists("build/win-release/host/core/adhostwin.exe")) return fs::absolute("build/win-release/host/core/adhostwin.exe");

  return "adhostwin.exe";
}

// Find adimport.exe binary
fs::path find_adimport_exe(const fs::path& exe_dir) {
  if (!exe_dir.empty() && fs::exists(exe_dir / "adimport.exe")) {
    return exe_dir / "adimport.exe";
  }
  if (fs::exists("build/dist/LongAfterDark/adimport.exe")) return fs::absolute("build/dist/LongAfterDark/adimport.exe");
  if (fs::exists("build/win/importer/adimport.exe")) return fs::absolute("build/win/importer/adimport.exe");
  if (fs::exists("build/win-release/importer/adimport.exe")) return fs::absolute("build/win-release/importer/adimport.exe");
  return "adimport.exe";
}

// Parse catalog-win.json
bool load_catalog(const fs::path& catalog_path, Catalog& out) {
  if (!fs::exists(catalog_path)) return false;
  std::ifstream f(catalog_path, std::ios::binary);
  if (!f.is_open()) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();

  auto root = adw::import::parse_json(content);
  if (!root || root->kind != adw::import::JsonValue::Kind::object) return false;

  const auto* modules = root->get("modules");
  if (!modules || modules->kind != adw::import::JsonValue::Kind::array) return false;

  for (const auto& item : modules->array) {
    if (item.kind != adw::import::JsonValue::Kind::object) continue;
    ModuleInfo m;
    m.id = item.str("id");
    m.name = item.str("displayName");
    m.path = item.str("path");
    m.package = item.str("package");
    m.about = item.str("about");
    if (!m.id.empty() && !m.path.empty()) {
      out.modules.push_back(std::move(m));
    }
  }
  return !out.modules.empty();
}

}  // namespace

int main(int argc, char** argv) {
  struct sigaction sa{};
  sa.sa_handler = sig_handler;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  fs::path exe_dir;
  {
    char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      exe_dir = fs::path(buf).parent_path();
    }
  }

  // Options
  bool mode_window = false;
  bool mode_fullscreen = false;
  bool mode_random = false;
  bool mode_list = false;
  bool mode_sound = true;
  bool mode_root = false;
  Window window_id = 0;
  int scale = 0;
  int fps = 60;
  int cycle_seconds = 300;
  std::string override_assets_dir;
  std::string target_module;
  std::vector<std::string> import_args;
  bool run_import = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Long After Dark (Linux) v" << kVersion << "\n"
                << "Runs original After Dark screen savers on Linux.\n\n"
                << "Usage:\n"
                << "  longafterdark [options] [module-name-or-path]\n\n"
                << "Options:\n"
                << "  -w, --window            Run in a window (default size 640x480)\n"
                << "  -f, --fullscreen        Run full-screen\n"
                << "  -r, --random            Rotate through random modules\n"
                << "      --cycle <sec>       Seconds per module in random mode (default: 300)\n"
                << "  -l, --list              List all imported modules\n"
                << "  -s, --scale <factor>    Integer scale factor (1, 2, 3, etc.)\n"
                << "      --fps <rate>        Target frame rate (default: 60)\n"
                << "      --sound             Enable sound (default)\n"
                << "      --no-sound          Mute sound\n"
                << "  -window-id <id>         Draw in an existing X11 window (for XScreenSaver)\n"
                << "  -root                   Draw on the root window\n"
                << "      --assets-dir <dir>  Path to assets folder containing catalog-win.json\n"
                << "      --import [opts]     Run adimport.exe under Wine to import releases\n"
                << "  -v, --version           Print version and exit\n\n"
                << "Controls:\n"
                << "  Esc / 'q' / Space       Quit (screensaver mode)\n"
                << "  Caps Lock               Start / Stop game mode (Rodger Dodger, etc.)\n"
                << "  Mouse move              Wake / Exit (screensaver mode)\n"
                << "  Alt                     Exit even while playing a game\n";
      return 0;
    } else if (arg == "-v" || arg == "--version") {
      std::cout << "Long After Dark " << kVersion << "\n";
      return 0;
    } else if (arg == "-w" || arg == "--window") {
      mode_window = true;
    } else if (arg == "-f" || arg == "--fullscreen") {
      mode_fullscreen = true;
    } else if (arg == "-r" || arg == "--random") {
      mode_random = true;
    } else if (arg == "-l" || arg == "--list") {
      mode_list = true;
    } else if (arg == "-root") {
      mode_root = true;
    } else if (arg == "--sound") {
      mode_sound = true;
    } else if (arg == "--no-sound") {
      mode_sound = false;
    } else if (arg == "-window-id" && i + 1 < argc) {
      window_id = (Window)std::stoul(argv[++i], nullptr, 0);
    } else if ((arg == "-s" || arg == "--scale") && i + 1 < argc) {
      scale = std::stoi(argv[++i]);
    } else if (arg == "--fps" && i + 1 < argc) {
      fps = std::stoi(argv[++i]);
    } else if (arg == "--cycle" && i + 1 < argc) {
      cycle_seconds = std::stoi(argv[++i]);
    } else if (arg == "--assets-dir" && i + 1 < argc) {
      override_assets_dir = argv[++i];
    } else if (arg == "--import") {
      run_import = true;
      for (int j = i + 1; j < argc; ++j) {
        import_args.push_back(argv[j]);
      }
      break;
    } else if (!arg.empty() && arg[0] != '-') {
      target_module = arg;
    }
  }

  std::string wine_bin = find_wine_bin();

  // Handle --import forwarding
  if (run_import) {
    fs::path adimport = find_adimport_exe(exe_dir);
    std::string adimport_str = adimport.string();
    std::vector<const char*> cmd = {wine_bin.c_str(), adimport_str.c_str()};
    for (const auto& a : import_args) cmd.push_back(a.c_str());
    cmd.push_back(nullptr);
    execvp(wine_bin.c_str(), const_cast<char* const*>(cmd.data()));
    std::cerr << "longafterdark: failed to execute " << wine_bin << " " << adimport << "\n";
    return 1;
  }

  // Find assets and catalog
  fs::path assets_dir = find_assets_dir(override_assets_dir);
  fs::path catalog_path = assets_dir / "catalog-win.json";
  Catalog catalog;
  bool has_catalog = load_catalog(catalog_path, catalog);

  if (mode_list) {
    if (!has_catalog) {
      std::cerr << "No After Dark modules imported yet.\n"
                << "Run 'longafterdark --import --download [deluxe|simpsons|ad32|tt|ad10]' to download and import.\n";
      return 1;
    }
    std::cout << "Imported After Dark modules (" << catalog.modules.size() << " total):\n\n";
    std::string last_pkg;
    for (const auto& m : catalog.modules) {
      if (m.package != last_pkg) {
        std::cout << "\n[" << m.package << "]\n";
        last_pkg = m.package;
      }
      std::cout << "  " << m.id << "\t" << m.name << " (" << m.path << ")\n";
    }
    return 0;
  }

  // Check if Wine is installed
  if (system((wine_bin + " --version >/dev/null 2>&1").c_str()) != 0) {
    std::cerr << "Error: 'wine' is required to run After Dark modules on Linux.\n"
              << "Please install Wine (e.g. 'sudo apt install wine wine64' or 'sudo pacman -S wine').\n";
    return 1;
  }

  fs::path adhost = find_adhost_exe(exe_dir);
  if (!fs::exists(adhost)) {
    std::cerr << "Error: cannot find adhostwin.exe at " << adhost << "\n";
    return 1;
  }

  // Determine which module to run
  if (target_module.empty()) {
    if (!has_catalog || catalog.modules.empty()) {
      std::cerr << "No module specified and no imported modules found in " << assets_dir << ".\n"
                << "Running built-in test pattern. (To import modules: 'longafterdark --import --download simpsons')\n";
      target_module = "--test-pattern";
    } else {
      mode_random = true;
    }
  }
  if (!target_module.empty() && target_module != "--test-pattern") {
    bool found = false;
    for (const auto& m : catalog.modules) {
      if (m.id == target_module || m.path == target_module) {
        target_module = m.path;
        found = true;
        break;
      }
    }
    if (!found) {
      for (const auto& m : catalog.modules) {
        if (m.id.find(target_module) != std::string::npos ||
            m.name.find(target_module) != std::string::npos ||
            m.path.find(target_module) != std::string::npos) {
          target_module = m.path;
          found = true;
          break;
        }
      }
    }
  }

  // Connect to X11 Display
  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    std::cerr << "Error: cannot open X11 display (is DISPLAY set?)\n";
    return 1;
  }

  int screen_num = DefaultScreen(display);
  Visual* visual = DefaultVisual(display, screen_num);
  int depth = DefaultDepth(display, screen_num);

  // Setup window
  Window win = 0;
  int win_w = 640;
  int win_h = 480;

  if (window_id != 0) {
    win = window_id;
    XWindowAttributes wa;
    XGetWindowAttributes(display, win, &wa);
    win_w = wa.width;
    win_h = wa.height;
  } else if (mode_root) {
    win = RootWindow(display, screen_num);
    win_w = DisplayWidth(display, screen_num);
    win_h = DisplayHeight(display, screen_num);
  } else {
    if (mode_fullscreen || (!mode_window && window_id == 0)) {
      win_w = DisplayWidth(display, screen_num);
      win_h = DisplayHeight(display, screen_num);
      mode_fullscreen = true;
    } else if (scale > 0) {
      win_w = 640 * scale;
      win_h = 480 * scale;
    }

    XSetWindowAttributes swa;
    swa.background_pixel = BlackPixel(display, screen_num);
    swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask;

    win = XCreateWindow(display, RootWindow(display, screen_num), 0, 0, win_w, win_h, 0,
                        depth, InputOutput, visual, CWBackPixel | CWEventMask, &swa);

    XStoreName(display, win, "Long After Dark");

    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, win, &wm_delete, 1);

    if (mode_fullscreen) {
      Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
      Atom wm_fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
      XChangeProperty(display, win, wm_state, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char*>(&wm_fullscreen), 1);
    }

    XMapRaised(display, win);
    XFlush(display);
  }

  GC gc = XCreateGC(display, win, 0, nullptr);

#ifdef HAS_XSHM
  bool use_xshm = XShmQueryExtension(display);
  XShmSegmentInfo shminfo{};
  static bool s_xshm_error = false;
#else
  bool use_xshm = false;
#endif

  XImage* ximage = nullptr;
  int img_w = win_w;
  int img_h = win_h;

  auto allocate_image = [&](int w, int h) {
    if (ximage) {
#ifdef HAS_XSHM
      if (use_xshm) {
        XShmDetach(display, &shminfo);
        XDestroyImage(ximage);
        shmdt(shminfo.shmaddr);
      } else
#endif
      {
        XDestroyImage(ximage);
      }
      ximage = nullptr;
    }

    img_w = w;
    img_h = h;

#ifdef HAS_XSHM
    if (use_xshm) {
      s_xshm_error = false;
      auto old_handler = XSetErrorHandler([](Display*, XErrorEvent*) -> int {
        s_xshm_error = true;
        return 0;
      });
      ximage = XShmCreateImage(display, visual, depth, ZPixmap, nullptr, &shminfo, img_w, img_h);
      if (ximage) {
        shminfo.shmid = shmget(IPC_PRIVATE, ximage->bytes_per_line * ximage->height, IPC_CREAT | 0777);
        if (shminfo.shmid != -1) {
          shminfo.shmaddr = ximage->data = (char*)shmat(shminfo.shmid, 0, 0);
          if (shminfo.shmaddr != (char*)-1) {
            shminfo.readOnly = False;
            XShmAttach(display, &shminfo);
            XSync(display, False);
            shmctl(shminfo.shmid, IPC_RMID, nullptr);
          } else {
            s_xshm_error = true;
          }
        } else {
          s_xshm_error = true;
        }
      } else {
        s_xshm_error = true;
      }
      XSetErrorHandler(old_handler);
      if (s_xshm_error) {
        if (ximage) {
          XDestroyImage(ximage);
          ximage = nullptr;
        }
        use_xshm = false;
      }
    }
    if (!use_xshm)
#endif
    {
      char* data = (char*)malloc(img_w * img_h * 4);
      ximage = XCreateImage(display, visual, depth, ZPixmap, 0, data, img_w, img_h, 32, 0);
    }
  };

  allocate_image(win_w, win_h);

  std::mt19937 rng(std::random_device{}());

  auto pick_module = [&]() -> std::string {
    if (!mode_random || catalog.modules.empty()) return target_module;
    std::uniform_int_distribution<size_t> dist(0, catalog.modules.size() - 1);
    return catalog.modules[dist(rng)].path;
  };

  bool in_game = false;
  int last_mouse_x = -1;
  int last_mouse_y = -1;
  bool first_motion = true;

  // Frame processing loop with module switching support
  while (!g_shutdown.load()) {
    std::string current_module = pick_module();
    if (current_module.empty()) current_module = "--test-pattern";

    // Setup child pipes
    int pipe_in[2];   // Parent -> Child stdin
    int pipe_out[2];  // Child stdout -> Parent
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
      std::cerr << "Failed to create pipes\n";
      break;
    }

    pid_t pid = fork();
    if (pid == 0) {
      // Child process
      close(pipe_in[1]);
      close(pipe_out[0]);
      dup2(pipe_in[0], STDIN_FILENO);
      dup2(pipe_out[1], STDOUT_FILENO);
      close(pipe_in[0]);
      close(pipe_out[1]);

      // Set environment
      setenv("ADSTREAM", "1", 1);
      setenv("ADSCREENW", "640", 1);
      setenv("ADSCREENH", "480", 1);
      setenv("WINEDEBUG", "-all", 1);
      if (mode_sound) setenv("ADSOUND", "1", 1);
      else setenv("ADSOUND", "0", 1);

      if (!assets_dir.empty()) {
        setenv("AD_ASSETS_DIR", assets_dir.c_str(), 1);
      }

      std::string host_str = adhost.string();
      std::vector<const char*> c_args = {wine_bin.c_str(), host_str.c_str()};
      if (current_module != "--test-pattern") {
        c_args.push_back(current_module.c_str());
      } else {
        c_args.push_back("--test-pattern");
      }
      c_args.push_back(nullptr);

      execvp(wine_bin.c_str(), const_cast<char* const*>(c_args.data()));
      _exit(127);
    }

    g_child_pid = pid;
    close(pipe_in[0]);
    close(pipe_out[1]);

    int host_stdin_fd = pipe_in[1];
    int host_stdout_fd = pipe_out[0];

    auto send_command = [&](const std::string& cmd) {
      if (host_stdin_fd >= 0) {
        std::string line = cmd + "\n";
        ssize_t written = write(host_stdin_fd, line.data(), line.size());
        (void)written;
      }
    };

    // Send first GO to initiate lockstep streaming
    send_command("GO");

    // Reading P8 stream
    auto read_exact = [&](void* buf, size_t n) -> bool {
      uint8_t* p = static_cast<uint8_t*>(buf);
      size_t left = n;
      while (left > 0 && !g_shutdown.load()) {
        struct pollfd pfd{};
        pfd.fd = host_stdout_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
          ssize_t got = read(host_stdout_fd, p, left);
          if (got <= 0) return false;
          p += got;
          left -= got;
        } else if (ret < 0 && errno != EINTR) {
          return false;
        }
      }
      return left == 0;
    };

    auto read_line = [&]() -> std::string {
      std::string s;
      char ch = 0;
      while (!g_shutdown.load() && read_exact(&ch, 1)) {
        if (ch == '\n') break;
        s.push_back(ch);
      }
      return s;
    };

    auto start_time = std::chrono::steady_clock::now();
    uint32_t argb_pal[256];
    std::vector<uint8_t> raw_pixels;

    bool module_running = true;

    while (module_running && !g_shutdown.load()) {
      // Check cycle timeout in random mode
      if (mode_random && cycle_seconds > 0) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= cycle_seconds) {
          break; // Switch module
        }
      }

      // Check X11 events
      while (XPending(display) > 0) {
        XEvent ev;
        XNextEvent(display, &ev);

        if (ev.type == ConfigureNotify) {
          if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
            win_w = ev.xconfigure.width;
            win_h = ev.xconfigure.height;
            allocate_image(win_w, win_h);
          }
        } else if (ev.type == ClientMessage) {
          g_shutdown.store(true);
          break;
        } else if (ev.type == KeyPress) {
          KeySym sym = XLookupKeysym(&ev.xkey, 0);
          if (sym == XK_Caps_Lock) {
            in_game = !in_game;
            send_command(std::string("CAPS ") + (in_game ? "1" : "0"));
          } else if (sym == XK_Escape || sym == XK_q || sym == XK_Q) {
            if (!in_game) {
              g_shutdown.store(true);
              break;
            } else {
              // In game mode: Esc quits
              g_shutdown.store(true);
              break;
            }
          } else if (sym == XK_Alt_L || sym == XK_Alt_R) {
            g_shutdown.store(true);
            break;
          } else {
            if (!in_game && !mode_window && window_id == 0) {
              // Screensaver mode: key press exits
              g_shutdown.store(true);
              break;
            }
            int vk = xkeysym_to_vk(sym);
            if (vk > 0) {
              send_command("KEY " + std::to_string(vk) + " 1");
            }
          }
        } else if (ev.type == KeyRelease) {
          KeySym sym = XLookupKeysym(&ev.xkey, 0);
          int vk = xkeysym_to_vk(sym);
          if (vk > 0) {
            send_command("KEY " + std::to_string(vk) + " 0");
          }
        } else if (ev.type == MotionNotify) {
          if (first_motion) {
            last_mouse_x = ev.xmotion.x;
            last_mouse_y = ev.xmotion.y;
            first_motion = false;
          } else {
            int dx = std::abs(ev.xmotion.x - last_mouse_x);
            int dy = std::abs(ev.xmotion.y - last_mouse_y);
            if (!in_game && !mode_window && window_id == 0 && (dx > 10 || dy > 10)) {
              g_shutdown.store(true);
              break;
            }
            last_mouse_x = ev.xmotion.x;
            last_mouse_y = ev.xmotion.y;
          }

          if (in_game) {
            // Scale mouse to 640x480
            int mx = std::clamp(ev.xmotion.x * 640 / win_w, 0, 639);
            int my = std::clamp(ev.xmotion.y * 480 / win_h, 0, 479);
            send_command("MOUSE " + std::to_string(mx) + " " + std::to_string(my) + " 0");
          }
        } else if (ev.type == ButtonPress) {
          if (!in_game && !mode_window && window_id == 0) {
            g_shutdown.store(true);
            break;
          }
          if (in_game) {
            int mx = std::clamp(ev.xbutton.x * 640 / win_w, 0, 639);
            int my = std::clamp(ev.xbutton.y * 480 / win_h, 0, 479);
            send_command("MOUSE " + std::to_string(mx) + " " + std::to_string(my) + " 1");
          }
        } else if (ev.type == ButtonRelease) {
          if (in_game) {
            int mx = std::clamp(ev.xbutton.x * 640 / win_w, 0, 639);
            int my = std::clamp(ev.xbutton.y * 480 / win_h, 0, 479);
            send_command("MOUSE " + std::to_string(mx) + " " + std::to_string(my) + " 0");
          }
        }
      }

      if (g_shutdown.load()) break;

      // Read next frame from adhostwin
      std::string magic = read_line();
      if (magic.empty()) {
        module_running = false;
        break;
      }

      if (magic != "P8") {
        // Skip log or non-frame output
        continue;
      }

      std::string dim_line = read_line();
      int fw = 640, fh = 480;
      if (sscanf(dim_line.c_str(), "%d %d", &fw, &fh) != 2) {
        module_running = false;
        break;
      }

      // Read 768 palette bytes
      uint8_t pal_raw[768];
      if (!read_exact(pal_raw, sizeof(pal_raw))) {
        module_running = false;
        break;
      }

      for (int i = 0; i < 256; ++i) {
        uint8_t r = pal_raw[i * 3 + 0];
        uint8_t g = pal_raw[i * 3 + 1];
        uint8_t b = pal_raw[i * 3 + 2];
        argb_pal[i] = (0xFFu << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
      }

      // Read pixel data
      size_t num_pixels = size_t(fw) * size_t(fh);
      if (raw_pixels.size() < num_pixels) raw_pixels.resize(num_pixels);
      if (!read_exact(raw_pixels.data(), num_pixels)) {
        module_running = false;
        break;
      }

      // Blit and scale to XImage
      uint32_t* dst_row = reinterpret_cast<uint32_t*>(ximage->data);
      int dst_stride = ximage->bytes_per_line / 4;

      // Nearest-neighbor scaling
      for (int y = 0; y < win_h; ++y) {
        int src_y = y * fh / win_h;
        const uint8_t* src_row = raw_pixels.data() + src_y * fw;
        uint32_t* dst = dst_row + y * dst_stride;
        for (int x = 0; x < win_w; ++x) {
          int src_x = x * fw / win_w;
          dst[x] = argb_pal[src_row[src_x]];
        }
      }

      // Put to screen
#ifdef HAS_XSHM
      if (use_xshm) {
        XShmPutImage(display, win, gc, ximage, 0, 0, 0, 0, win_w, win_h, False);
      } else
#endif
      {
        XPutImage(display, win, gc, ximage, 0, 0, 0, 0, win_w, win_h);
      }
      XFlush(display);

      // Signal host for next frame
      send_command("GO");
    }

    // Clean up child process
    send_command("QUIT");
    close(host_stdin_fd);
    close(host_stdout_fd);

    int status = 0;
    bool reaped = false;
    for (int w = 0; w < 20; ++w) {
      if (waitpid(pid, &status, WNOHANG) == pid) {
        reaped = true;
        break;
      }
      usleep(25000); // 25ms
    }
    if (!reaped) {
      kill(pid, SIGTERM);
      usleep(50000);
      if (waitpid(pid, &status, WNOHANG) != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
    }
    g_child_pid = 0;
  }

  // Cleanup X11
  if (ximage) {
#ifdef HAS_XSHM
    if (use_xshm) {
      XShmDetach(display, &shminfo);
      XDestroyImage(ximage);
      shmdt(shminfo.shmaddr);
      shmctl(shminfo.shmid, IPC_RMID, nullptr);
    } else
#endif
    {
      XDestroyImage(ximage);
    }
  }

  XFreeGC(display, gc);
  if (window_id == 0 && !mode_root) {
    XDestroyWindow(display, win);
  }
  XCloseDisplay(display);

  return 0;
}
