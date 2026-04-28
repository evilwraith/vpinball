//
// compile gcc sdl3-joymap-vpx.c -o sdl3-joymap-vpx `pkg-config --cflags --libs sdl3 ncurses`
//
#include <SDL3/SDL.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>

#define AXIS_DEADZONE 0.05f

/* VPX virtual button IDs for HAT directions */
#define VPX_DPAD_LEFT   256
#define VPX_DPAD_RIGHT  257
#define VPX_DPAD_UP     258
#define VPX_DPAD_DOWN   259

static void guid_to_string(SDL_GUID guid, char *out, size_t len)
{
    SDL_GUIDToString(guid, out, len);
}

int main(void)
{
    /* ---------- SDL INIT ---------- */
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int num_joys = 0;
    SDL_JoystickID *joy_ids = SDL_GetJoysticks(&num_joys);
    if (!joy_ids || num_joys == 0) {
        fprintf(stderr, "No joysticks detected\n");
        SDL_Quit();
        return 0;
    }

    SDL_Joystick *joy = SDL_OpenJoystick(joy_ids[0]);
    if (!joy) {
        fprintf(stderr, "Failed to open joystick: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int num_axes    = SDL_GetNumJoystickAxes(joy);
    int num_buttons = SDL_GetNumJoystickButtons(joy);
    int num_hats    = SDL_GetNumJoystickHats(joy);

    char guid_str[64];
    guid_to_string(SDL_GetJoystickGUID(joy), guid_str, sizeof(guid_str));

    /* ---------- NCURSES INIT ---------- */
    initscr();
    noecho();
    cbreak();
    curs_set(0);
    timeout(0);

    start_color();
    use_default_colors();

    init_pair(1, COLOR_WHITE,  -1); /* normal */
    init_pair(2, COLOR_GREEN,  -1); /* active button */
    init_pair(3, COLOR_YELLOW, -1); /* active dpad */
    init_pair(4, COLOR_CYAN,   -1); /* active axis */
    init_pair(5, COLOR_RED,    -1); /* header */

    bool running = true;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                running = false;
        }

        SDL_UpdateJoysticks();

        erase();
        int row = 0;

        attron(COLOR_PAIR(5) | A_BOLD);
        mvprintw(row++, 0, "SDL3 -> VPX Logical Input Monitor");
        attroff(COLOR_PAIR(5) | A_BOLD);

        mvprintw(row++, 0, "==============================================");
        mvprintw(row++, 0, "Name : %s", SDL_GetJoystickName(joy));
        mvprintw(row++, 0, "GUID : %s", guid_str);
        row++;

        /* ---------- AXES ---------- */
        mvprintw(row++, 0, "Axes (%d):", num_axes);
        for (int i = 0; i < num_axes; i++) {
            Sint16 raw = SDL_GetJoystickAxis(joy, i);
            float norm = raw / 32767.0f;

            /* Xbox triggers are single-ended */
            bool is_trigger = (i == 2 || i == 5);
            bool active = is_trigger ? (norm > AXIS_DEADZONE)
                                     : (fabsf(norm) > AXIS_DEADZONE);

            attron(COLOR_PAIR(active ? 4 : 1));
            mvprintw(row++, 2,
                     "Axis %2d | VPX %3d | raw %6d | norm %+1.3f",
                     i, 512 + i, raw, norm);
            attroff(COLOR_PAIR(active ? 4 : 1));
        }

        row++;

        /* ---------- BUTTONS ---------- */
        mvprintw(row++, 0, "Buttons (%d):", num_buttons);
        for (int i = 0; i < num_buttons; i++) {
            bool pressed = SDL_GetJoystickButton(joy, i);

            attron(COLOR_PAIR(pressed ? 2 : 1));
            mvprintw(row++, 2,
                     "Button %2d | VPX %3d | %s",
                     i, i, pressed ? "PRESSED" : "released");
            attroff(COLOR_PAIR(pressed ? 2 : 1));
        }

        row++;

        /* ---------- HATS / DPAD (VPX LOGICAL) ---------- */
        mvprintw(row++, 0, "D-Pad / Hats (%d) [VPX logical buttons]:", num_hats);

        for (int h = 0; h < num_hats; h++) {
            Uint8 hat = SDL_GetJoystickHat(joy, h);

            bool up    = hat & SDL_HAT_UP;
            bool down  = hat & SDL_HAT_DOWN;
            bool left  = hat & SDL_HAT_LEFT;
            bool right = hat & SDL_HAT_RIGHT;

            attron(COLOR_PAIR(up ? 3 : 1));
            mvprintw(row++, 2,
                     "Hat %d | VPX %3d | DPad UP",
                     h, VPX_DPAD_UP);
            attroff(COLOR_PAIR(up ? 3 : 1));

            attron(COLOR_PAIR(down ? 3 : 1));
            mvprintw(row++, 2,
                     "Hat %d | VPX %3d | DPad DOWN",
                     h, VPX_DPAD_DOWN);
            attroff(COLOR_PAIR(down ? 3 : 1));

            attron(COLOR_PAIR(left ? 3 : 1));
            mvprintw(row++, 2,
                     "Hat %d | VPX %3d | DPad LEFT",
                     h, VPX_DPAD_LEFT);
            attroff(COLOR_PAIR(left ? 3 : 1));

            attron(COLOR_PAIR(right ? 3 : 1));
            mvprintw(row++, 2,
                     "Hat %d | VPX %3d | DPad RIGHT",
                     h, VPX_DPAD_RIGHT);
            attroff(COLOR_PAIR(right ? 3 : 1));
        }

        /* ---------- FOOTER ---------- */
        int maxy, maxx;
        getmaxyx(stdscr, maxy, maxx);
        attron(A_BOLD);
        mvprintw(maxy - 1, 0, "CTRL-C to quit");
        attroff(A_BOLD);

        refresh();
        SDL_Delay(50);
    }

    /* ---------- CLEANUP ---------- */
    endwin();
    SDL_CloseJoystick(joy);
    SDL_free(joy_ids);
    SDL_Quit();
    return 0;
}