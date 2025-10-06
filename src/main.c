#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ncurses.h>

#include "jsmn.h"

#define FREE_PTR( x ) { free(x); x=0; }

/* strdup implementation for C99 compatibility */
char* my_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

#define MAX_TOKENS 2048
#define INDENT_SIZE 4
#define MAX_SEARCH_LEN 256

typedef struct {
    jsmntok_t *tokens;
    int token_count;
    char *json_str;
    int current_line;
    int scroll_offset;
    int *visible_tokens;
    int visible_count;
    int *collapsed;
    int *depths;
    int max_y, max_x;
    char search_term[MAX_SEARCH_LEN];
    int *search_matches;
    int search_match_count;
    int current_match_idx;
    int search_mode;
} JsonViewer;

/* Case-insensitive substring search */
char* stristr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }

        if (!*n) return (char*)haystack;
    }

    return NULL;
}

/* Skip to next sibling token */
int skip_token(jsmntok_t *tokens, int token_idx, int count) {
    jsmntok_t *tok = &tokens[token_idx];
    int next = token_idx + 1;

    if (tok->type == JSMN_OBJECT || tok->type == JSMN_ARRAY) {
        int children = tok->size;
        if (tok->type == JSMN_OBJECT) {
            children *= 2; // Objects have key-value pairs
        }

        for (int i = 0; i < children && next < count; i++) {
            next = skip_token(tokens, next, count);
        }
    }

    return next;
}

/* Calculate token depths for indentation */
void calculate_depths(jsmntok_t *tokens, int count, int *depths) {
    depths[0] = 0;

    for (int i = 1; i < count; i++) {
        int depth = 0;
        int pos = tokens[i].start;

        for (int j = 0; j < i; j++) {
            if (tokens[j].start < pos && tokens[j].end > pos) {
                depth++;
            }
        }
        depths[i] = depth;
    }
}

/* Check if token is a key in an object */
int is_object_key(JsonViewer *viewer, int tok_idx) {
    if (tok_idx == 0) return 0;

    int pos = viewer->tokens[tok_idx].start;
    for (int i = tok_idx - 1; i >= 0; i--) {
        jsmntok_t *parent = &viewer->tokens[i];
        if (parent->start < pos && parent->end > pos) {
            if (parent->type == JSMN_OBJECT) {
                int child_idx = i + 1;
                int count = 0;
                while (child_idx < tok_idx) {
                    count++;
                    child_idx = skip_token(viewer->tokens, child_idx, viewer->token_count);
                }
                return (count % 2 == 0);
            }
            break;
        }
    }
    return 0;
}

/* Find the token index for a given visible line */
int get_token_for_line(JsonViewer *viewer, int line) {
    if (line < 0 || line >= viewer->visible_count) return -1;
    return viewer->visible_tokens[line];
}

/* Check if token content matches search term */
int token_matches_search(JsonViewer *viewer, int tok_idx) {
    if (!viewer->search_term[0]) return 0;

    jsmntok_t *tok = &viewer->tokens[tok_idx];

    // Don't match on object/array containers themselves
    if (tok->type == JSMN_OBJECT || tok->type == JSMN_ARRAY) {
        return 0;
    }

    int len = tok->end - tok->start;

    if (len <= 0) return 0;

    char *token_str = malloc(len + 1);
    if (!token_str) return 0;

    memcpy(token_str, viewer->json_str + tok->start, len);
    token_str[len] = '\0';

    int matches = (stristr(token_str, viewer->search_term) != NULL);

    FREE_PTR(token_str);
    return matches;
}

/* Build search matches list */
void build_search_matches(JsonViewer *viewer) {
    viewer->search_match_count = 0;

    if (!viewer->search_term[0]) return;

    for (int i = 0; i < viewer->visible_count; i++) {
        int tok_idx = viewer->visible_tokens[i];
        if (token_matches_search(viewer, tok_idx)) {
            viewer->search_matches[viewer->search_match_count++] = i;
        }
    }
}

/* Go to next search match */
void goto_next_match(JsonViewer *viewer) {
    if (viewer->search_match_count == 0) return;

    viewer->current_match_idx = (viewer->current_match_idx + 1) % viewer->search_match_count;
    viewer->current_line = viewer->search_matches[viewer->current_match_idx];
}

/* Go to previous search match */
void goto_prev_match(JsonViewer *viewer) {
    if (viewer->search_match_count == 0) return;

    viewer->current_match_idx--;
    if (viewer->current_match_idx < 0) {
        viewer->current_match_idx = viewer->search_match_count - 1;
    }
    viewer->current_line = viewer->search_matches[viewer->current_match_idx];
}

/* Build list of visible tokens (expanded tree view) */
void build_visible_tokens(JsonViewer *viewer, int token_idx, int depth) {
    if (token_idx >= viewer->token_count || viewer->visible_count >= MAX_TOKENS) {
        return;
    }

    jsmntok_t *tok = &viewer->tokens[token_idx];

    // Add current token to visible list
    viewer->visible_tokens[viewer->visible_count++] = token_idx;

    // Don't expand if collapsed
    if (viewer->collapsed[token_idx]) {
        return;
    }

    // Process children
    if (tok->type == JSMN_OBJECT) {
        int child_idx = token_idx + 1;
        for (int i = 0; i < tok->size && child_idx < viewer->token_count; i++) {
            // Key
            int key_idx = child_idx; (void)key_idx;
            build_visible_tokens(viewer, child_idx, depth + 1);
            child_idx = skip_token(viewer->tokens, child_idx, viewer->token_count);

            // Value - check if it's a primitive/string that will be shown inline
            jsmntok_t *value_tok = &viewer->tokens[child_idx];
            if (value_tok->type == JSMN_STRING || value_tok->type == JSMN_PRIMITIVE) {
                // Skip adding the value token since it will be displayed inline with key
                // But we still need to mark it as processed
            } else {
                // Object or array - add it separately
                build_visible_tokens(viewer, child_idx, depth + 1);
            }
            child_idx = skip_token(viewer->tokens, child_idx, viewer->token_count);
        }
    } else if (tok->type == JSMN_ARRAY) {
        int child_idx = token_idx + 1;
        for (int i = 0; i < tok->size && child_idx < viewer->token_count; i++) {
            build_visible_tokens(viewer, child_idx, depth + 1);
            child_idx = skip_token(viewer->tokens, child_idx, viewer->token_count);
        }
    }
}

/* Print token value to a string buffer */
void format_token_value(const char *json, jsmntok_t *tok, char *buf, int bufsize) {
    int len = tok->end - tok->start;
    if (len >= bufsize) len = bufsize - 1;

    if (tok->type == JSMN_STRING) {
        snprintf(buf, bufsize, "\"%.*s\"", len, json + tok->start);
    } else {
        snprintf(buf, bufsize, "%.*s", len, json + tok->start);
    }
}

/* Display the JSON tree using ncurses */
void display_json(JsonViewer *viewer) {
    getmaxyx(stdscr, viewer->max_y, viewer->max_x);

    // Clear and redraw
    erase();

    // Header
    attron(A_REVERSE);
    mvprintw(0, 0, " JSON Viewer - by Cristian Mancus ");
    for (int i = 35; i < viewer->max_x; i++) {
        addch(' ');
    }
    attroff(A_REVERSE);

    mvprintw(1, 0, " j/k: down/up | h/l: collapse/expand | /: search | n/N: next/prev | q: quit");

    // Content area starts at line 3
    int content_start = 3;
    int max_lines = viewer->max_y - content_start - 2; // Leave room for status line

    // Adjust scroll to keep current line visible
    if (viewer->current_line < viewer->scroll_offset) {
        viewer->scroll_offset = viewer->current_line;
    }
    if (viewer->current_line >= viewer->scroll_offset + max_lines) {
        viewer->scroll_offset = viewer->current_line - max_lines + 1;
    }

    // Display visible lines
    for (int i = 0; i < max_lines && (viewer->scroll_offset + i) < viewer->visible_count; i++) {
        int line_idx = viewer->scroll_offset + i;
        int tok_idx = viewer->visible_tokens[line_idx];
        jsmntok_t *tok = &viewer->tokens[tok_idx];
        int depth = viewer->depths[tok_idx];

        int y_pos = content_start + i;

        // Check if this line is a search match
        int is_search_match = 0;
        if (viewer->search_term[0]) {
            for (int j = 0; j < viewer->search_match_count; j++) {
                if (viewer->search_matches[j] == line_idx) {
                    is_search_match = 1;
                    break;
                }
            }
        }

        // Highlight current line or search match
        if (line_idx == viewer->current_line) {
            attron(A_REVERSE);
        } else if (is_search_match) {
            attron(A_BOLD);
        }

        // Move to position and clear line
        move(y_pos, 0);
        clrtoeol();

        // Indentation
        int x_pos = depth * INDENT_SIZE;
        move(y_pos, x_pos);

        // Check if this is a key and next token is its value
        int is_key = is_object_key(viewer, tok_idx);
        int next_line_idx = line_idx + 1; (void)next_line_idx;

        char value_buf[256];

        if (is_key) {
            // Display key
            format_token_value(viewer->json_str, tok, value_buf, sizeof(value_buf));
            printw("%s : ", value_buf);

            // Find the actual value token (not from visible list, but from token array)
            int value_tok_idx = skip_token(viewer->tokens, tok_idx, viewer->token_count);
            if (value_tok_idx < viewer->token_count) {
                jsmntok_t *value_tok = &viewer->tokens[value_tok_idx];

                if (value_tok->type == JSMN_STRING || value_tok->type == JSMN_PRIMITIVE) {
                    // Show value inline
                    format_token_value(viewer->json_str, value_tok, value_buf, sizeof(value_buf));
                    printw("%s", value_buf);
                } else if (value_tok->type == JSMN_OBJECT) {
                    // printw("%s{ } (%d items)%s",
                    printw("%s{%d items}%s",
                           viewer->collapsed[value_tok_idx] ? "[+] " : "[-] ",
                           value_tok->size,
                           viewer->collapsed[value_tok_idx] ? " ..." : "");
                } else if (value_tok->type == JSMN_ARRAY) {
                    // printw("%s[ ] (%d items)%s",
                    printw("%s[%d items]%s",
                           viewer->collapsed[value_tok_idx] ? "[+] " : "[-] ",
                           value_tok->size,
                           viewer->collapsed[value_tok_idx] ? " ..." : "");
                }
            }
        } else if (tok->type == JSMN_OBJECT) {
            // printw("%s{ } (%d items)%s",
            printw("%s{%d items}%s",
                   viewer->collapsed[tok_idx] ? "[+] " : "[-] ",
                   tok->size,
                   viewer->collapsed[tok_idx] ? " ..." : "");
        } else if (tok->type == JSMN_ARRAY) {
            // printw("%s[ ] (%d items)%s",
            printw("%s[%d items]%s",
                   viewer->collapsed[tok_idx] ? "[+] " : "[-] ",
                   tok->size,
                   viewer->collapsed[tok_idx] ? " ..." : "");
        } else {
            // Standalone primitive/string (array element)
            format_token_value(viewer->json_str, tok, value_buf, sizeof(value_buf));
            printw("%s", value_buf);
        }

        if (line_idx == viewer->current_line) {
            attroff(A_REVERSE);
        } else if (is_search_match) {
            attroff(A_BOLD);
        }
    }

    // Status line
    attron(COLOR_PAIR(1));
    if (viewer->search_term[0]) {
        mvprintw(viewer->max_y - 1, 0, " Line %d/%d | Search: \"%s\" (%d matches) | Match %d/%d ",
                 viewer->current_line + 1, viewer->visible_count,
                 viewer->search_term,
                 viewer->search_match_count,
                 viewer->search_match_count > 0 ? viewer->current_match_idx + 1 : 0,
                 viewer->search_match_count);
    } else {
        mvprintw(viewer->max_y - 1, 0, " Line %d/%d | Tokens: %d | Size: %dx%d ",
                 viewer->current_line + 1, viewer->visible_count,
                 viewer->token_count,
                 viewer->max_y, viewer->max_x);
    }
    clrtoeol();
    attroff(COLOR_PAIR(1));

    refresh();
}

/* Search input mode */
void search_input(JsonViewer *viewer) {
    int ch;
    int cursor_pos = strlen(viewer->search_term);

    // Enable echo and cursor for input
    echo();
    curs_set(1);

    while (1) {
        // Display search prompt
        attron(COLOR_PAIR(1));
        mvprintw(viewer->max_y - 1, 0, " Search: %s", viewer->search_term);
        clrtoeol();
        attroff(COLOR_PAIR(1));
        move(viewer->max_y - 1, 9 + cursor_pos);
        refresh();

        ch = getch();

        if (ch == '\n' || ch == KEY_ENTER) {
            // Execute search
            break;
        } else if (ch == 27) { // ESC
            // Cancel search
            viewer->search_term[0] = '\0';
            viewer->search_match_count = 0;
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            // Backspace
            if (cursor_pos > 0) {
                cursor_pos--;
                viewer->search_term[cursor_pos] = '\0';
            }
        } else if (ch >= 32 && ch < 127 && cursor_pos < MAX_SEARCH_LEN - 1) {
            // Regular character
            viewer->search_term[cursor_pos++] = ch;
            viewer->search_term[cursor_pos] = '\0';
        }
    }

    // Disable echo and cursor
    noecho();
    curs_set(0);

    // Build search matches
    if (viewer->search_term[0]) {
        build_search_matches(viewer);
        if (viewer->search_match_count > 0) {
            viewer->current_match_idx = 0;
            viewer->current_line = viewer->search_matches[0];
        }
    }
}

/* Initialize viewer */
int viewer_init(JsonViewer *viewer, const char *json_str) {
    viewer->json_str = my_strdup(json_str);
    viewer->current_line = 0;
    viewer->scroll_offset = 0;
    viewer->search_term[0] = '\0';
    viewer->search_match_count = 0;
    viewer->current_match_idx = 0;
    viewer->search_mode = 0;

    // Parse JSON
    jsmn_parser parser;
    jsmn_init(&parser);

    viewer->tokens = malloc(sizeof(jsmntok_t) * MAX_TOKENS);
    viewer->token_count = jsmn_parse(&parser, json_str, strlen(json_str),
                                     viewer->tokens, MAX_TOKENS);

    if (viewer->token_count < 0) {
        fprintf(stderr, "Failed to parse JSON: %d\n", viewer->token_count);
        return -1;
    }

    viewer->visible_tokens = calloc(MAX_TOKENS, sizeof(int));
    viewer->collapsed      = calloc(viewer->token_count, sizeof(int));
    viewer->depths         = calloc(viewer->token_count, sizeof(int));
    viewer->search_matches = calloc(MAX_TOKENS, sizeof(int));

    calculate_depths(viewer->tokens, viewer->token_count, viewer->depths);

    return 0;
}

void viewer_cleanup(JsonViewer *viewer) {
    FREE_PTR(viewer->json_str);
    FREE_PTR(viewer->tokens);
    FREE_PTR(viewer->visible_tokens);
    FREE_PTR(viewer->collapsed);
    FREE_PTR(viewer->depths);
    FREE_PTR(viewer->search_matches);
}

/* Main viewer loop */
void viewer_run(JsonViewer *viewer) {
    int ch;
    int running = 1;

    while (running) {
        // Rebuild visible token list
        viewer->visible_count = 0;
        build_visible_tokens(viewer, 0, 0);

        // Rebuild search matches if search is active
        if (viewer->search_term[0]) {
            build_search_matches(viewer);
        }

        // Ensure current line is in bounds
        if (viewer->current_line >= viewer->visible_count) {
            viewer->current_line = viewer->visible_count - 1;
        }
        if (viewer->current_line < 0) {
            viewer->current_line = 0;
        }

        display_json(viewer);

        ch = getch();

        int tok_idx = get_token_for_line(viewer, viewer->current_line);
        if (tok_idx < 0 && ch != 'q' && ch != 'Q' && ch != '/') continue;

        jsmntok_t *tok = tok_idx >= 0 ? &viewer->tokens[tok_idx] : NULL;
        int max_lines = viewer->max_y - 5;

        switch(ch) {
            case 'q':
            case 'Q':
                running = 0;
                break;

            case '/':
                // Enter search mode
                viewer->search_term[0] = '\0';
                viewer->search_match_count = 0;
                search_input(viewer);
                break;

            case 'n':
                // Next search match
                goto_next_match(viewer);
                break;

            case 'N':
                // Previous search match
                goto_prev_match(viewer);
                break;

            case 27: // ESC - clear search
                viewer->search_term[0] = '\0';
                viewer->search_match_count = 0;
                viewer->current_match_idx = 0;
                break;

            case 'j': // Down
            case KEY_DOWN:
                if (viewer->current_line < viewer->visible_count - 1) {
                    viewer->current_line++;
                }
                break;

            case 'k': // Up
            case KEY_UP:
                if (viewer->current_line > 0) {
                    viewer->current_line--;
                }
                break;

            case 'h': // Collapse
            case KEY_LEFT:
                if (tok && (tok->type == JSMN_OBJECT || tok->type == JSMN_ARRAY)) {
                    viewer->collapsed[tok_idx] = 1;
                }
                break;

            case 'l': // Expand
            case KEY_RIGHT:
                if (tok && (tok->type == JSMN_OBJECT || tok->type == JSMN_ARRAY)) {
                    viewer->collapsed[tok_idx] = 0;
                }
                break;

            case 4: // Ctrl-D
                {
                    int half_page = max_lines / 2;
                    viewer->current_line += half_page;
                    if (viewer->current_line >= viewer->visible_count) {
                        viewer->current_line = viewer->visible_count - 1;
                    }
                }
                break;

            case 21: // Ctrl-U
                {
                    int half_page = max_lines / 2;
                    viewer->current_line -= half_page;
                    if (viewer->current_line < 0) {
                        viewer->current_line = 0;
                    }
                }
                break;

            case ' ': // Space to toggle expand/collapse
                if (tok && (tok->type == JSMN_OBJECT || tok->type == JSMN_ARRAY)) {
                    viewer->collapsed[tok_idx] = !viewer->collapsed[tok_idx];
                }
                break;

            case 'g': // Go to top
                viewer->current_line = 0;
                break;

            case 'G': // Go to bottom
                viewer->current_line = viewer->visible_count - 1;
                break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <json_file>\n", argv[0]);
        return 1;
    }

    char *json_str = 0;
    FILE *f = NULL;

    // Try to load from file if it exists
    f = fopen(argv[1], "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        json_str = malloc(size + 1);
        if (!json_str) {
            fprintf(stderr, "Memory allocation failed\n");
            fclose(f);
            return 1;
        }
        fread(json_str, 1, size, f);
        json_str[size] = '\0';
        fclose(f);
    }

    JsonViewer viewer;
    if (viewer_init(&viewer, json_str) < 0) {
        if (f) FREE_PTR(json_str);
        return 1;
    }

    // Initialize ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    // Initialize colors if available
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_BLACK, COLOR_CYAN);
    }

    viewer_run(&viewer);

    // Cleanup ncurses
    endwin();

    viewer_cleanup(&viewer);

    if (f) FREE_PTR(json_str);

    return 0;
}
