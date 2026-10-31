#include "io.h"
#include "power.h"
#include <stdint.h>

#define BOARD_WIDTH 40
#define BOARD_HEIGHT 18
#define MAX_SNAKE_LENGTH (BOARD_WIDTH * BOARD_HEIGHT)

#define KEY_ESCAPE 0x1b
#define KEY_UP 'w'
#define KEY_DOWN 's'
#define KEY_LEFT 'a'
#define KEY_RIGHT 'd'

typedef struct {
    int8_t x;
    int8_t y;
} Point;

typedef enum {
    DIRECTION_UP,
    DIRECTION_DOWN,
    DIRECTION_LEFT,
    DIRECTION_RIGHT,
} Direction;

typedef enum {
    GAME_READY,
    GAME_PLAYING,
    GAME_PAUSED,
    GAME_OVER,
} GameState;

typedef struct {
    Point body[MAX_SNAKE_LENGTH];
    int length;
    Direction direction;
    Direction pending_direction;
    Point food;
    uint32_t score;
    uint32_t high_score;
    uint32_t random_state;
    GameState state;
} Game;

static Game game;
static uint64_t last_tick;
static int escape_state;
static int quit_requested;

static uint64_t read_time(void) {
    uint64_t value;
    asm volatile("rdtime %0" : "=r"(value));
    return value;
}

static int uart_can_read(void) {
    return (*(volatile uint8_t*)(UART_ADDR + 5) & 1) != 0;
}

static uint8_t uart_read(void) {
    return *(volatile uint8_t*)UART_ADDR;
}

static void write_string(const char* text) {
    while (*text)
        uart_putc(*text++);
}

static void write_unsigned(uint32_t value) {
    char digits[10];
    int length = 0;

    do {
        digits[length++] = '0' + value % 10;
        value /= 10;
    } while (value);

    while (length)
        uart_putc(digits[--length]);
}

static uint32_t next_random(void) {
    uint32_t value = game.random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game.random_state = value;
    return value;
}

static int points_equal(Point lhs, Point rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

static int body_contains(Point point) {
    for (int i = 0; i < game.length; ++i) {
        if (points_equal(game.body[i], point))
            return 1;
    }
    return 0;
}

static void place_food(void) {
    int free_cells = BOARD_WIDTH * BOARD_HEIGHT - game.length;
    int target = (int)(next_random() % (uint32_t)free_cells);

    for (int y = 0; y < BOARD_HEIGHT; ++y) {
        for (int x = 0; x < BOARD_WIDTH; ++x) {
            Point candidate = { (int8_t)x, (int8_t)y };
            if (body_contains(candidate))
                continue;
            if (target-- == 0) {
                game.food = candidate;
                return;
            }
        }
    }
}

static int direction_is_opposite(Direction lhs, Direction rhs) {
    return (lhs == DIRECTION_UP && rhs == DIRECTION_DOWN) ||
           (lhs == DIRECTION_DOWN && rhs == DIRECTION_UP) ||
           (lhs == DIRECTION_LEFT && rhs == DIRECTION_RIGHT) ||
           (lhs == DIRECTION_RIGHT && rhs == DIRECTION_LEFT);
}

static void reset_game(void) {
    game.length = 4;
    game.direction = DIRECTION_RIGHT;
    game.pending_direction = DIRECTION_RIGHT;
    game.score = 0;
    game.state = GAME_READY;
    game.random_state ^= (uint32_t)read_time();
    if (game.random_state == 0)
        game.random_state = 0x51a7e5u;

    for (int i = 0; i < game.length; ++i) {
        game.body[i].x = (int8_t)(8 - i);
        game.body[i].y = BOARD_HEIGHT / 2;
    }
    place_food();
}

static void render(void) {
    char cells[BOARD_HEIGHT][BOARD_WIDTH];

    for (int y = 0; y < BOARD_HEIGHT; ++y) {
        for (int x = 0; x < BOARD_WIDTH; ++x)
            cells[y][x] = ' ';
    }
    cells[game.food.y][game.food.x] = '*';
    for (int i = game.length - 1; i >= 0; --i) {
        cells[game.body[i].y][game.body[i].x] = i == 0 ? '@' : 'o';
    }

    write_string("\x1b[H");
    write_string("\x1b[1;36m  HERE SNAKE\x1b[0m  score ");
    write_string("\x1b[1;33m");
    write_unsigned(game.score);
    write_string("\x1b[0m  high ");
    write_string("\x1b[1;35m");
    write_unsigned(game.high_score);
    write_string("\x1b[0m      \n");

    write_string("  +");
    for (int x = 0; x < BOARD_WIDTH; ++x)
        uart_putc('-');
    write_string("+\n");

    for (int y = 0; y < BOARD_HEIGHT; ++y) {
        write_string("  |");
        for (int x = 0; x < BOARD_WIDTH; ++x) {
            char cell = cells[y][x];
            if (cell == '@')
                write_string("\x1b[1;32m@\x1b[0m");
            else if (cell == 'o')
                write_string("\x1b[32mo\x1b[0m");
            else if (cell == '*')
                write_string("\x1b[1;31m*\x1b[0m");
            else
                uart_putc(' ');
        }
        write_string("|\n");
    }

    write_string("  +");
    for (int x = 0; x < BOARD_WIDTH; ++x)
        uart_putc('-');
    write_string("+\x1b[K\n");

    switch (game.state) {
    case GAME_READY:
        write_string("  \x1b[1;33mWASD / ARROWS\x1b[0m TO START   P PAUSE   Q QUIT\x1b[K\n");
        break;
    case GAME_PLAYING:
        write_string("  \x1b[32mRUNNING\x1b[0m   WASD / ARROWS   P PAUSE   Q QUIT\x1b[K\n");
        break;
    case GAME_PAUSED:
        write_string("  \x1b[1;33mPAUSED\x1b[0m   P RESUME   R RESTART   Q QUIT\x1b[K\n");
        break;
    case GAME_OVER:
        write_string("  \x1b[1;31mGAME OVER\x1b[0m   R RESTART   Q QUIT\x1b[K\n");
        break;
    }
    write_string("\x1b[J");
}

static void finish_game(void) {
    if (game.score > game.high_score)
        game.high_score = game.score;
    game.state = GAME_OVER;
}

static void step_game(void) {
    if (!direction_is_opposite(game.direction, game.pending_direction))
        game.direction = game.pending_direction;

    Point head = game.body[0];
    switch (game.direction) {
    case DIRECTION_UP:
        head.y--;
        break;
    case DIRECTION_DOWN:
        head.y++;
        break;
    case DIRECTION_LEFT:
        head.x--;
        break;
    case DIRECTION_RIGHT:
        head.x++;
        break;
    }

    if (head.x < 0 || head.x >= BOARD_WIDTH || head.y < 0 || head.y >= BOARD_HEIGHT) {
        finish_game();
        return;
    }

    int ate_food = points_equal(head, game.food);
    int tail_index = game.length - 1;
    for (int i = 0; i < game.length; ++i) {
        if (i != tail_index || ate_food) {
            if (points_equal(head, game.body[i])) {
                finish_game();
                return;
            }
        }
    }

    int next_length = game.length + (ate_food ? 1 : 0);
    for (int i = next_length - 1; i > 0; --i)
        game.body[i] = game.body[i - 1];
    game.body[0] = head;
    game.length = next_length;

    if (ate_food) {
        game.score += 10;
        if (game.score > game.high_score)
            game.high_score = game.score;
        if (game.length == MAX_SNAKE_LENGTH) {
            finish_game();
            return;
        }
        place_food();
    }
}

static void set_direction(Direction direction) {
    if (game.state == GAME_READY) {
        game.direction = direction;
        game.pending_direction = direction;
        game.state = GAME_PLAYING;
        last_tick = read_time();
    } else if (game.state == GAME_PLAYING) {
        game.pending_direction = direction;
    }
}

static int handle_key(uint8_t key) {
    if (escape_state == 1) {
        if (key == '[') {
            escape_state = 2;
            return 0;
        }
        escape_state = 0;
        return 0;
    }
    if (escape_state == 2) {
        escape_state = 0;
        if (key == 'A')
            set_direction(DIRECTION_UP);
        else if (key == 'B')
            set_direction(DIRECTION_DOWN);
        else if (key == 'C')
            set_direction(DIRECTION_RIGHT);
        else if (key == 'D')
            set_direction(DIRECTION_LEFT);
        return 0;
    }

    if (key == KEY_ESCAPE) {
        escape_state = 1;
        return 0;
    }

    if (key >= 'A' && key <= 'Z')
        key = (uint8_t)(key - 'A' + 'a');

    switch (key) {
    case KEY_UP:
        set_direction(DIRECTION_UP);
        return 0;
    case KEY_DOWN:
        set_direction(DIRECTION_DOWN);
        return 0;
    case KEY_LEFT:
        set_direction(DIRECTION_LEFT);
        return 0;
    case KEY_RIGHT:
        set_direction(DIRECTION_RIGHT);
        return 0;
    case 'p':
        if (game.state == GAME_PLAYING)
            game.state = GAME_PAUSED;
        else if (game.state == GAME_PAUSED) {
            game.state = GAME_PLAYING;
            last_tick = read_time();
        }
        return 0;
    case 'r': {
        uint32_t high_score = game.high_score;
        reset_game();
        game.high_score = high_score;
        return 0;
    }
    case 'q':
        quit_requested = 1;
        return 1;
    default:
        return 0;
    }
}

static void wait_for_next_tick(uint64_t tick_interval) {
    while (read_time() - last_tick < tick_interval) {
        while (uart_can_read()) {
            if (handle_key(uart_read()))
                return;
        }
    }
}

int main(void) {
    game.high_score = 0;
    game.random_state = 0x6d2b79f5u;
    reset_game();

    write_string("\x1b[?1049h\x1b[?25l\x1b[2J");
    render();

    while (1) {
        if (game.state == GAME_PLAYING) {
            wait_for_next_tick(1600000);
            if (quit_requested) {
                write_string("\x1b[?25h\x1b[?1049l");
                PowerOff();
            }
            if (game.state == GAME_PLAYING) {
                step_game();
                last_tick = read_time();
            }
            render();
        } else {
            if (uart_can_read()) {
                if (handle_key(uart_read())) {
                    write_string("\x1b[?25h\x1b[?1049l");
                    PowerOff();
                }
                render();
            }
        }
    }
}
