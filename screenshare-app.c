/*
 * lunecast - LuneCast sender for webOS (HP TouchPad)
 *
 * Simple SDL-based app that:
 * 1. Starts the fbcapture daemon when launched
 * 2. Shows status/instructions on screen
 * 3. Stops the daemon when closed (card swipe)
 *
 * Build with PalmPDK + SDL
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>
#include <PDL.h>

#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

/* Install directory. Everything that depends on the app id derives from this,
 * so a rebrand only has to change it in one place. */
#define APP_DIR "/media/cryptofs/apps/usr/palm/applications/org.webosarchive.lunecast"

#define FBCAPTURE_PATH   APP_DIR "/fbcapture"
#define STATUS_ICON_PATH APP_DIR "/status-icon.png"
#define FBCAPTURE_OUTPUT "/media/internal/screen.jpg"

/* Port negotiation.
 *
 * stream-server.py prefers DEFAULT_STREAM_PORT but will fall back to another
 * port if that one is already taken on the host (nginx on 8080 is the case
 * that prompted this). Having bound, it writes the port it actually got to
 * this file over novacom, and we show that in the instructions instead of
 * telling the user to open a port nothing is listening on.
 *
 * /media/internal is bind-mounted read-write into the PDK jail, so this is
 * readable from here. If the file is absent - server not started yet - we
 * fall back to the default, which is what the server will try first anyway.
 */
#define STREAM_PORT_FILE "/media/internal/lunecast-port.txt"

/* Written by stream-server.py while it is driving capture itself over a
 * persistent novacom pipe (fbcapture -S). Two capture daemons running at once
 * would double the CPU cost and fight for the framebuffer, so while this file
 * exists we stop our own daemon and let the host own it. Removed when the
 * server exits, at which point we take ownership back. */
#define HOST_CAPTURE_FILE "/media/internal/lunecast-host.txt"
#define DEFAULT_STREAM_PORT 8080
#define PORT_POLL_MS 1000
/* Poll interval. The daemon skips the JPEG encode when the composited frame
 * is unchanged (~25ms/iteration idle vs ~140ms for an encode), so a tighter
 * interval costs little when the screen is static but noticeably shortens the
 * lag before a change is noticed - which is what leaves the far end stuck on
 * a stale frame at the tail of a short animation like the tap ripple. */
#define FBCAPTURE_INTERVAL "40"
#define FBCAPTURE_QUALITY "75"

/* Default font path on webOS */
#define FONT_PATH "/usr/share/fonts/PreludeCondensed-Medium.ttf"
#define FONT_SIZE_LARGE 48
#define FONT_SIZE_MEDIUM 32
#define FONT_SIZE_SMALL 24

static pid_t daemon_pid = 0;
static SDL_Surface *icon_surface = NULL;  /* status-screen app icon, may be NULL */
static int g_stream_port = DEFAULT_STREAM_PORT;
static int g_host_capturing = 0;
static int running = 1;

/* Colors */
static SDL_Color color_white = {255, 255, 255, 255};
static SDL_Color color_green = {100, 255, 100, 255};
static SDL_Color color_gray = {180, 180, 180, 255};
static SDL_Color color_darkgray = {80, 80, 80, 255};

static void start_daemon(void) {
    if (daemon_pid > 0) {
        /* Already running */
        return;
    }

    daemon_pid = fork();

    if (daemon_pid == 0) {
        /* Child process - exec fbcapture */
        execl(FBCAPTURE_PATH, "fbcapture",
              "-d",  /* Daemon mode (foreground, so we can track it) */
              "-i", FBCAPTURE_INTERVAL,
              "-q", FBCAPTURE_QUALITY,
              "-o", FBCAPTURE_OUTPUT,
              NULL);

        /* If exec fails, try alternate path */
        execl("/media/internal/fbcapture", "fbcapture",
              "-d", "-i", FBCAPTURE_INTERVAL,
              "-q", FBCAPTURE_QUALITY,
              "-o", FBCAPTURE_OUTPUT,
              NULL);

        fprintf(stderr, "Failed to exec fbcapture: %s\n", strerror(errno));
        _exit(1);
    } else if (daemon_pid < 0) {
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        daemon_pid = 0;
    } else {
        fprintf(stderr, "Started fbcapture daemon, PID: %d\n", daemon_pid);
    }
}

static void stop_daemon(void) {
    if (daemon_pid > 0) {
        fprintf(stderr, "Stopping fbcapture daemon, PID: %d\n", daemon_pid);
        kill(daemon_pid, SIGTERM);

        /* Wait briefly for clean exit */
        int status;
        int wait_result = waitpid(daemon_pid, &status, WNOHANG);
        if (wait_result == 0) {
            /* Still running, give it a moment */
            usleep(100000);
            wait_result = waitpid(daemon_pid, &status, WNOHANG);
            if (wait_result == 0) {
                /* Force kill */
                kill(daemon_pid, SIGKILL);
                waitpid(daemon_pid, &status, 0);
            }
        }
        daemon_pid = 0;
    }
}

static int check_daemon_running(void) {
    if (daemon_pid <= 0) return 0;

    int status;
    int result = waitpid(daemon_pid, &status, WNOHANG);

    if (result == 0) {
        /* Still running */
        return 1;
    } else {
        /* Exited */
        daemon_pid = 0;
        return 0;
    }
}

/* Re-read the port the host server published. Cheap, but pointless to do on
 * every frame, so at most once a second. */
static void refresh_stream_port(void) {
    static Uint32 last_check = 0;
    Uint32 now = SDL_GetTicks();

    if (last_check != 0 && (now - last_check) < PORT_POLL_MS) {
        return;
    }
    last_check = now;

    /* Is the host driving capture over its own pipe? */
    FILE *hf = fopen(HOST_CAPTURE_FILE, "r");
    g_host_capturing = (hf != NULL);
    if (hf) fclose(hf);

    int port = DEFAULT_STREAM_PORT;
    FILE *f = fopen(STREAM_PORT_FILE, "r");
    if (f) {
        int parsed = 0;
        if (fscanf(f, "%d", &parsed) == 1 && parsed >= 1024 && parsed <= 65535) {
            port = parsed;
        }
        fclose(f);
    }
    g_stream_port = port;
}

static void render_text_centered(SDL_Surface *screen, TTF_Font *font,
                                  const char *text, int y, SDL_Color color) {
    SDL_Surface *text_surface = TTF_RenderText_Blended(font, text, color);
    if (text_surface) {
        SDL_Rect dest = {(SCREEN_WIDTH - text_surface->w) / 2, y, 0, 0};
        SDL_BlitSurface(text_surface, NULL, screen, &dest);
        SDL_FreeSurface(text_surface);
    }
}

static void render_screen(SDL_Surface *screen, TTF_Font *font_large,
                          TTF_Font *font_medium, TTF_Font *font_small) {
    refresh_stream_port();

    /* Clear screen with dark background */
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 30, 30, 35));

    /* Layout is built by accumulating y rather than using absolute positions,
     * so it stays correct if the icon fails to load. The start offset centres
     * the block vertically now that the Clear Buffer button is gone. */
    int y = 80;

    /* App icon */
    if (icon_surface) {
        SDL_Rect icon_dest = {(SCREEN_WIDTH - icon_surface->w) / 2, y, 0, 0};
        SDL_BlitSurface(icon_surface, NULL, screen, &icon_dest);
        y += icon_surface->h + 12;
    } else {
        y += 40;
    }

    /* Title */
    render_text_centered(screen, font_large, "LuneCast", y, color_white);
    y += 64;

    /* Status */
    if (g_host_capturing) {
        /* Host owns capture - make sure we are not also running one. */
        if (daemon_pid > 0) {
            stop_daemon();
        }
        render_text_centered(screen, font_medium, "Status: STREAMING", y, color_green);
    } else if (check_daemon_running()) {
        render_text_centered(screen, font_medium, "Status: STREAMING", y, color_green);
    } else {
        render_text_centered(screen, font_medium, "Status: Starting...", y, color_gray);
        /* Try to restart */
        start_daemon();
    }
    y += 52;

    /* Divider */
    SDL_Rect divider = {100, y, SCREEN_WIDTH - 200, 2};
    SDL_FillRect(screen, &divider, SDL_MapRGB(screen->format, 60, 60, 65));
    y += 24;

    /* Instructions */
    render_text_centered(screen, font_small, "On your computer, download:", y, color_gray);
    y += 34;

    render_text_centered(screen, font_medium, "https://github.com/webOSArchive/LuneCast", y, color_white);
    y += 46;

    render_text_centered(screen, font_small, "then run:", y, color_gray);
    y += 34;

    render_text_centered(screen, font_medium, "./stream-server.py", y, color_white);
    y += 46;

    render_text_centered(screen, font_small, "Connect via USB, then open the stream at:", y, color_gray);
    y += 34;

    char stream_url[64];
    /* Point at the viewer PAGE, not /stream. The page is an ordinary HTML
     * document embedding <img src="/stream">, so only the img needs
     * multipart/x-mixed-replace support - the most widely compatible way to
     * consume it across browsers. */
    snprintf(stream_url, sizeof(stream_url), "http://localhost:%d/", g_stream_port);
    render_text_centered(screen, font_medium, stream_url, y, color_white);
    y += 46;

    /* Footer */
    y += 26;
    render_text_centered(screen, font_small, "Swipe away this app to stop streaming", y, color_darkgray);

    SDL_Flip(screen);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Initialize PDL */
    PDL_Init(0);

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Initialize SDL_ttf */
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    /* Create window */
    SDL_Surface *screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32,
                                            SDL_SWSURFACE);
    if (!screen) {
        fprintf(stderr, "SDL_SetVideoMode failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    /* Load fonts */
    TTF_Font *font_large = TTF_OpenFont(FONT_PATH, FONT_SIZE_LARGE);
    TTF_Font *font_medium = TTF_OpenFont(FONT_PATH, FONT_SIZE_MEDIUM);
    TTF_Font *font_small = TTF_OpenFont(FONT_PATH, FONT_SIZE_SMALL);

    if (!font_large || !font_medium || !font_small) {
        fprintf(stderr, "Failed to load font: %s\n", TTF_GetError());
        /* Try fallback */
        font_large = TTF_OpenFont("/usr/share/fonts/Prelude-Medium.ttf", FONT_SIZE_LARGE);
        font_medium = TTF_OpenFont("/usr/share/fonts/Prelude-Medium.ttf", FONT_SIZE_MEDIUM);
        font_small = TTF_OpenFont("/usr/share/fonts/Prelude-Medium.ttf", FONT_SIZE_SMALL);
    }

    /* Load the status-screen icon. Optional: if it fails to load the layout
     * simply omits it rather than failing to start. Converted to the display
     * format up front so the per-frame blit does not re-convert, and so the
     * PNG's alpha composites against the dark background. */
    SDL_Surface *icon_raw = IMG_Load(STATUS_ICON_PATH);
    if (icon_raw) {
        icon_surface = SDL_DisplayFormatAlpha(icon_raw);
        SDL_FreeSurface(icon_raw);
    } else {
        fprintf(stderr, "Icon load failed (%s): %s\n", STATUS_ICON_PATH, IMG_GetError());
    }

    /* Set custom pause handling - we want to keep running when minimized */
    PDL_CustomPauseUiEnable(PDL_TRUE);

    /* Start the capture daemon */
    start_daemon();

    /* Main loop */
    SDL_Event event;
    Uint32 last_render = 0;

    while (running) {
        /* Handle events */
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;

                case SDL_ACTIVEEVENT:
                    if (event.active.state & SDL_APPACTIVE) {
                        if (event.active.gain == 0) {
                            /* App is being closed/minimized */
                            /* On webOS, SDL_QUIT is sent when carded */
                        }
                    }
                    break;

            }
        }

        /* Render at ~30 FPS to save battery */
        Uint32 now = SDL_GetTicks();
        if (now - last_render >= 33) {
            render_screen(screen, font_large, font_medium, font_small);
            last_render = now;
        }

        /* Small delay to reduce CPU usage */
        SDL_Delay(16);
    }

    /* Cleanup */
    stop_daemon();

    if (icon_surface) SDL_FreeSurface(icon_surface);
    if (font_large) TTF_CloseFont(font_large);
    if (font_medium) TTF_CloseFont(font_medium);
    if (font_small) TTF_CloseFont(font_small);

    TTF_Quit();
    SDL_Quit();
    PDL_Quit();

    return 0;
}
