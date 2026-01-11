/*
 * screenshare-app - webOS Screen Share Application
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
#include <PDL.h>

#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

#define FBCAPTURE_PATH "/media/cryptofs/apps/usr/palm/applications/org.webosarchive.screenshare/fbcapture"
#define FBCAPTURE_OUTPUT "/media/internal/screen.jpg"
#define FBCAPTURE_INTERVAL "100"
#define FBCAPTURE_QUALITY "75"

/* Default font path on webOS */
#define FONT_PATH "/usr/share/fonts/PreludeCondensed-Medium.ttf"
#define FONT_SIZE_LARGE 48
#define FONT_SIZE_MEDIUM 32
#define FONT_SIZE_SMALL 24

static pid_t daemon_pid = 0;
static int running = 1;
static Uint32 clear_buffer_until = 0;  /* Timestamp when to stop showing black */

/* Button position for "Clear Buffer" */
#define BUTTON_X 312
#define BUTTON_Y 580
#define BUTTON_W 400
#define BUTTON_H 60

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
    Uint32 now = SDL_GetTicks();

    /* If we're in clear buffer mode, just show black */
    if (clear_buffer_until > 0 && now < clear_buffer_until) {
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
        render_text_centered(screen, font_medium, "Clearing buffer...", SCREEN_HEIGHT / 2, color_gray);
        SDL_Flip(screen);
        return;
    }
    clear_buffer_until = 0;  /* Reset after timeout */

    /* Clear screen with dark background */
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 30, 30, 35));

    int y = 80;

    /* Title */
    render_text_centered(screen, font_large, "Screen Share", y, color_white);
    y += 80;

    /* Status */
    if (check_daemon_running()) {
        render_text_centered(screen, font_medium, "Status: STREAMING", y, color_green);
    } else {
        render_text_centered(screen, font_medium, "Status: Starting...", y, color_gray);
        /* Try to restart */
        start_daemon();
    }
    y += 60;

    /* Divider */
    SDL_Rect divider = {100, y, SCREEN_WIDTH - 200, 2};
    SDL_FillRect(screen, &divider, SDL_MapRGB(screen->format, 60, 60, 65));
    y += 40;

    /* Instructions */
    render_text_centered(screen, font_small, "On your computer, run:", y, color_gray);
    y += 50;

    render_text_centered(screen, font_medium, "./stream-server.py", y, color_white);
    y += 50;

    render_text_centered(screen, font_small, "Then open in VLC:", y, color_gray);
    y += 50;

    render_text_centered(screen, font_medium, "http://localhost:8080/stream", y, color_white);
    y += 60;

    /* Clear Buffer button */
    SDL_Rect button = {BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H};
    SDL_FillRect(screen, &button, SDL_MapRGB(screen->format, 60, 60, 70));
    /* Button border */
    SDL_Rect border_top = {BUTTON_X, BUTTON_Y, BUTTON_W, 2};
    SDL_Rect border_bot = {BUTTON_X, BUTTON_Y + BUTTON_H - 2, BUTTON_W, 2};
    SDL_Rect border_left = {BUTTON_X, BUTTON_Y, 2, BUTTON_H};
    SDL_Rect border_right = {BUTTON_X + BUTTON_W - 2, BUTTON_Y, 2, BUTTON_H};
    Uint32 border_color = SDL_MapRGB(screen->format, 100, 100, 110);
    SDL_FillRect(screen, &border_top, border_color);
    SDL_FillRect(screen, &border_bot, border_color);
    SDL_FillRect(screen, &border_left, border_color);
    SDL_FillRect(screen, &border_right, border_color);
    render_text_centered(screen, font_medium, "Clear Buffer", BUTTON_Y + 12, color_white);

    /* Footer */
    render_text_centered(screen, font_small, "Swipe up to close and stop streaming", BUTTON_Y + 100, color_darkgray);

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

                case SDL_MOUSEBUTTONDOWN:
                    /* Check if touch is on the Clear Buffer button */
                    if (event.button.x >= BUTTON_X &&
                        event.button.x <= BUTTON_X + BUTTON_W &&
                        event.button.y >= BUTTON_Y &&
                        event.button.y <= BUTTON_Y + BUTTON_H) {
                        /* Trigger clear buffer mode for 2 seconds */
                        clear_buffer_until = SDL_GetTicks() + 2000;
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

    if (font_large) TTF_CloseFont(font_large);
    if (font_medium) TTF_CloseFont(font_medium);
    if (font_small) TTF_CloseFont(font_small);

    TTF_Quit();
    SDL_Quit();
    PDL_Quit();

    return 0;
}
