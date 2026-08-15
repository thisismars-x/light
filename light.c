//   
// Light is a light weight terminal code editor for light weight text editing tasks(similar to Nano)
// It is different to Nano, in regards to supporting simple plugins, and shortcuts(similar to Vim)
// 

#include<stdio.h>
#include<unistd.h>
#include<fcntl.h>
#include<signal.h>
#include<string.h>
#include<stdlib.h>
#include<errno.h>
#include<time.h>

#include<malloc.h>
#include<memory.h>

#include<sys/uio.h>
#include<sys/ioctl.h>
#include<sys/types.h>
#include<sys/stat.h>

#include<termios.h>
#include<pthread.h>

/*
------------------------------------

- MAX_NUMBER_OF_ROWS is the maximum screen 
size which is allowed

- MAX_NUMBER_OF_COLS defines the maximum number
of ASCII characters you can use per line


- DISPLAY_BUFFER_LEN defines the allocated
space for the display buffer

- SCRATCH_FILE is the name of the file where
your files are stored in case of an error
before crashing out

- TABSPACE how many spaces a TAB expands to

------------------------------------
*/
#define MAX_NUMBER_OF_ROWS    0xFFFF 
#define MAX_NUMBER_OF_COLS    0x0400 
#define DISPLAY_BUFFER_LEN    MAX_NUMBER_OF_ROWS * MAX_NUMBER_OF_COLS 
#define SCRATCH_FILE          ".light-scratch-XXXXXX"
#define PATHMAX               0x1000
#define TABSPACE              4
#define LINE_GUTTER           7

/*
------------------------------------

- NOFILE is the error which happens when
last line has '=filename' but filename
is empty

- CALLED_THROUGH_SHORTCUT is set when 
shortcut_save_file is called

------------------------------------
*/
#define bool                     u_int8_t 
#define true                       0x0001
#define false                 !      true
#define U_NOFILE                      -10
#define CALLED_THROUGH_SHORTCUT      true

/*
------------------------------------

- NUMBER_OF_ROWS counts the current total 
number of rows and is increased only
when you press enter

- CURRENT_ROW is the row we are at

- CURRENT_COL records which column we are
at CURRENT_ROW line

- EXIT_FLAG is set when we encounter '=quit' in
last line or a signal like SIGINT

- DISPLAY_BUFFER is the buffer that is written to 
by the user, and stored to be used with display_buffer
thread, to display

- handler_SIGINT sets EXIT_FLAG and exits

- SAVE_FILE is used if the last line in
the buffer has '=<filename>' format

- IGN_FILE is used if the buffer is to be
scratched after writing to it

- INIT_FILE is set if DISPLAY_BUFFER is 
constructed using a file instead of from
scratch

- INIT_ARG_FNAME is set to argv[1] if initialized 
with a file, like, so: light <fname>

- check_EXIT exits, by checking EXIT_FLAG, and
is aware if it was called_through_shortcut

------------------------------------
*/
u_int16_t NUMBER_OF_ROWS = 0;
u_int16_t CURRENT_ROW    = 0;
u_int16_t CURRENT_COL    = 0;
u_int16_t TERM_ROW       = 0;
u_int16_t TERM_COL       = 0;
volatile sig_atomic_t EXIT_FLAG = false;
bool      SAVE_FILE      = false;
bool      IGN_FILE       = true;
bool      INIT_FILE      = false;
bool      BUFFER_ENDS_NEWLINE = false;
bool      BUFFER_DIRTY   = false;
char      INIT_ARG_FNAME[PATHMAX];
char      DISPLAY_BUFFER[MAX_NUMBER_OF_ROWS][MAX_NUMBER_OF_COLS];
char      LINE_CLIPBOARD[MAX_NUMBER_OF_COLS];
u_int16_t VIEW_START_ROW = 0;
u_int16_t CURRENT_VIEW_COL = 0;
u_int16_t SELECT_START_ROW = 0;
u_int16_t SELECT_START_COL = 0;
u_int16_t SELECT_END_ROW = 0;
u_int16_t SELECT_END_COL = 0;
bool      SELECT_VISIBLE = false;
bool      SELECT_ACTIVE = false;
bool      CONFIRM_EXIT = false;
char*     TEXT_CLIPBOARD = NULL;
size_t    TEXT_CLIPBOARD_LEN = 0;

enum Language {
    LANGUAGE_TEXT,
    LANGUAGE_C,
    LANGUAGE_PYTHON
} FILE_LANGUAGE = LANGUAGE_TEXT;

struct UndoState {
    char* buffer;
    u_int16_t rows;
    u_int16_t row;
    u_int16_t col;
    bool ends_newline;
    bool valid;
} UNDO_STATE = {0};
void      save_buffer_to_file(const char*, bool);
void      set_terminal_raw_mode(bool);

void detect_language(const char* filename) {
    const char* extension = strrchr(filename, '.');
    FILE_LANGUAGE = LANGUAGE_TEXT;
    if(!extension) return;
    if(strcmp(extension, ".c") == 0 || strcmp(extension, ".cpp") == 0 ||
       strcmp(extension, ".cu") == 0) FILE_LANGUAGE = LANGUAGE_C;
    else if(strcmp(extension, ".py") == 0) FILE_LANGUAGE = LANGUAGE_PYTHON;
}
void      check_EXIT(char* filename, bool called_through_shortcut) {
  (void)filename;

  // if called_through_shortcut, just update that file, and continue  
  if(called_through_shortcut == CALLED_THROUGH_SHORTCUT) { return; }

  if(EXIT_FLAG) {
    printf("\033[H\033[2J");
    fflush(stdout);
    if(BUFFER_DIRTY) fprintf(stdout, "Exited without saving changes.\n");

    set_terminal_raw_mode(false);

    _exit(0);
  }

  return;
}
void      handler_SIGINT(int signal_number) {
  (void)signal_number;
  EXIT_FLAG = true;
}

// Disable buffer and echo in terminal, to capture sequences
// like up, down, left, right, enter as characters
void set_terminal_raw_mode(bool yes) {
  static struct termios old_t, new_t; 

  if(yes) {
    tcgetattr(STDIN_FILENO, &old_t);
    new_t = old_t;
    new_t.c_lflag &= ~( ICANON | ECHO | ISIG | IEXTEN );
    new_t.c_iflag &= ~( IXON | ICRNL );
    new_t.c_cc[VSUSP] = _POSIX_VDISABLE; // Ctrl+Z belongs to light, not the shell
    tcsetattr(STDIN_FILENO, TCSANOW, &new_t);
  } else {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
  }

  // SGR mouse mode is click-only; selection belongs to the keyboard.
  printf(yes? "\033[?7l\033[?25l\033[?1000h\033[?1006h":
              "\033[?1006l\033[?1000l\033[?25h\033[?7h");
  fflush(stdout);     

  return;
}

// Get number of rows and columns in my terminal window
void get_terminal_size() {
    struct winsize ws;

    // Get terminal size
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        fprintf(stderr, "grave error, can not recover(IOCTL), bye\n");
        EXIT_FLAG = true;
        check_EXIT("", !CALLED_THROUGH_SHORTCUT);
    }

    TERM_ROW = ws.ws_row;
    TERM_COL = ws.ws_col;
}

// What type of key are you pressing?
enum KeyType {
    KEY_UNKNOWN,
    KEY_CHAR,        
    KEY_CTRL,       
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_ARROW_LEFT,
    KEY_ARROW_RIGHT,
    KEY_WORD_LEFT,
    KEY_WORD_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_BACKSPACE,
    KEY_ENTER,
    KEY_TAB,
    KEY_ESC,
    KEY_MOUSE
};

// Which key are you exactly pressing?
// 'ch' is valid for KEY_CHAR and KEY_CTRL
struct Key {
    enum KeyType type;
    char ch;
    int mouse_button;
    int mouse_x;
    int mouse_y;
    bool mouse_pressed;
};

/*
------------------------------------

- current_char_lock is to avoid deadlock
conditions 

- current_char_cond wakes up display_buffer
thread whenever get_input receives some ip

- get_input is the thread that collects and 
interprets the input at terminal

- display_buffer is the thread that is 
responsible for displaying buffer after any
event that triggers 'DISPLAY' flag is 
performed
  
------------------------------------
*/
struct Key             current_char;
#define KEY_QUEUE_LEN 256
struct Key             key_queue[KEY_QUEUE_LEN];
size_t                 key_queue_read = 0;
size_t                 key_queue_write = 0;
pthread_mutex_t        current_char_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t         current_char_cond = PTHREAD_COND_INITIALIZER;
pthread_t              get_input, display_buffer; 

// Read input continously from terminal and 
// interpret it as any valid struct Key
void* input(void* unused) {
  (void)unused;
    
  while(true) { 
    int c = getchar();

    if(c == EOF) {
        EXIT_FLAG = true;
        pthread_mutex_lock(&current_char_lock);
        pthread_cond_signal(&current_char_cond);
        pthread_mutex_unlock(&current_char_lock);
        break;
    }

    struct Key key = { .type = KEY_UNKNOWN, .ch = 0 };

    if (c == 27) {                        // ESC or arrow current_chars
        int c2 = getchar();
        if (c2 == '[') {
            int sequence = getchar();
            if(sequence == '<') {
                char mouse[64];
                size_t used = 0;
                int end;
                while((end = getchar()) != EOF && end != 'M' && end != 'm' && used < sizeof(mouse) - 1) {
                    mouse[used++] = end;
                }
                mouse[used] = '\0';
                if(sscanf(mouse, "%d;%d;%d", &key.mouse_button, &key.mouse_x, &key.mouse_y) == 3) {
                    key.type = KEY_MOUSE;
                    key.mouse_pressed = end == 'M';
                }
            } else if(sequence >= '0' && sequence <= '9') {
                char control[32];
                size_t used = 0;
                control[used++] = sequence;
                int end;
                while((end = getchar()) != EOF && used < sizeof(control) - 1) {
                    control[used++] = end;
                    if((end >= 'A' && end <= 'Z') || end == '~') break;
                }
                control[used] = '\0';
                if(strcmp(control, "1;5D") == 0) key.type = KEY_WORD_LEFT;
                else if(strcmp(control, "1;5C") == 0) key.type = KEY_WORD_RIGHT;
                else if(strcmp(control, "1;5A") == 0 || strcmp(control, "5~") == 0) key.type = KEY_PAGE_UP;
                else if(strcmp(control, "1;5B") == 0 || strcmp(control, "6~") == 0) key.type = KEY_PAGE_DOWN;
                else if(strcmp(control, "1~") == 0 || strcmp(control, "7~") == 0) key.type = KEY_HOME;
                else if(strcmp(control, "4~") == 0 || strcmp(control, "8~") == 0) key.type = KEY_END;
            } else switch (sequence) {
                case 'A': key.type = KEY_ARROW_UP; break;
                case 'B': key.type = KEY_ARROW_DOWN; break;
                case 'C': key.type = KEY_ARROW_RIGHT; break;
                case 'D': key.type = KEY_ARROW_LEFT; break;
                case 'H': key.type = KEY_HOME; break;
                case 'F': key.type = KEY_END; break;
                default: break;
            }
        } else {
            key.type = KEY_ESC;
      }
    } else if (c == 0) {                  // Ctrl + Space
        key.type = KEY_CTRL;
        key.ch = ' ';
    } else if (c == 127 || c == 8) {      // Backspace (127 on Linux, 8 in some cases)
        key.type = KEY_BACKSPACE;
    }
    else if (c == 10 || c == 13) {        // Enter (LF=10, CR=13)
        key.type = KEY_ENTER;
    }
    else if (c == 9) {                    // Tab
        key.type = KEY_CHAR;
        key.ch = '\t';
    }
    else if (c >= 1 && c <= 26) {         // Ctrl+A (1) to Ctrl+Z (26)
        key.type = KEY_CTRL;
        key.ch = 'A' + c - 1;
    }
    else if (c >= 32 && c <= 255) {       // Printable text, including pasted UTF-8 bytes
        key.type = KEY_CHAR;
        key.ch = c;
    }

    pthread_mutex_lock(&current_char_lock);
    size_t next = (key_queue_write + 1) % KEY_QUEUE_LEN;
    while(next == key_queue_read) {
        pthread_cond_wait(&current_char_cond, &current_char_lock);
        next = (key_queue_write + 1) % KEY_QUEUE_LEN;
    }
    key_queue[key_queue_write] = key;
    key_queue_write = next;
    pthread_cond_signal(&current_char_cond);
    pthread_mutex_unlock(&current_char_lock);
  }
  
  return NULL;
}


// You can add plugins, by working with char* result
// in join_display_buffer function. 
// Make sure, to only concatenate to it, and not 
// overwrite it completely, because it stores past
// lines, too.
void plugin_show_line_colored(char* result, int line_no) {
   char c_line_no[64];
   snprintf(c_line_no, sizeof(c_line_no), "\033[2K\033[38;5;240m%5d;\033[0m ", line_no);
   strcat(result, c_line_no);
   return;
}

// Highlight current row, and current column
// Some style refactoring
// Instead of highlighting entire row, and seperately
// highlighting current col, only highlight curr col
// in curr row, looks much better
bool plugin_is_keyword(const char* word, size_t len) {
    static const char* c_keywords[] = {
        "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "if", "int",
        "long", "return", "short", "signed", "sizeof", "static", "struct",
        "switch", "typedef", "union", "unsigned", "void", "volatile", "while"
    };
    static const char* python_keywords[] = {
        "and", "as", "assert", "async", "await", "break", "class", "continue",
        "def", "del", "elif", "else", "except", "False", "finally", "for",
        "from", "global", "if", "import", "in", "is", "lambda", "None",
        "nonlocal", "not", "or", "pass", "raise", "return", "True", "try",
        "while", "with", "yield"
    };
    const char** keywords = FILE_LANGUAGE == LANGUAGE_PYTHON? python_keywords: c_keywords;
    size_t count = FILE_LANGUAGE == LANGUAGE_PYTHON?
        sizeof(python_keywords) / sizeof(python_keywords[0]):
        sizeof(c_keywords) / sizeof(c_keywords[0]);
    if(FILE_LANGUAGE == LANGUAGE_TEXT) return false;
    for(size_t i = 0; i < count; i++) {
        if(strlen(keywords[i]) == len && strncmp(word, keywords[i], len) == 0) return true;
    }
    return false;
}

const char* plugin_color_at(char* row, size_t at) {
    char quote = 0;
    bool escaped = false;

    for(size_t i = 0; i <= at; i++) {
        if(!quote && FILE_LANGUAGE == LANGUAGE_C && row[i] == '/' && row[i + 1] == '/')
            return "\033[38;5;244m";
        if(!quote && FILE_LANGUAGE == LANGUAGE_PYTHON && row[i] == '#')
            return "\033[38;5;244m";
        if(!quote && (row[i] == '"' || row[i] == '\'')) quote = row[i];
        else if(quote && row[i] == quote && !escaped && i > 0) quote = 0;
        escaped = row[i] == '\\' && !escaped;
        if(row[i] != '\\') escaped = false;
    }
    if(quote || row[at] == '"' || row[at] == '\'') return "\033[38;5;114m";
    const char* first = row + strspn(row, " \t");
    if(FILE_LANGUAGE == LANGUAGE_C && first[0] == '#') return "\033[38;5;176m";
    if(FILE_LANGUAGE == LANGUAGE_PYTHON && first[0] == '@') return "\033[38;5;176m";
    if(FILE_LANGUAGE == LANGUAGE_TEXT) return "";
    if(row[at] >= '0' && row[at] <= '9') return "\033[38;5;215m";

    size_t begin = at;
    size_t end = at;
    while(begin > 0 && ((row[begin - 1] >= 'A' && row[begin - 1] <= 'Z') ||
          (row[begin - 1] >= 'a' && row[begin - 1] <= 'z') || row[begin - 1] == '_')) begin--;
    while((row[end] >= 'A' && row[end] <= 'Z') ||
          (row[end] >= 'a' && row[end] <= 'z') || row[end] == '_') end++;
    if(plugin_is_keyword(row + begin, end - begin)) return "\033[38;5;81m";
    return "";
}

int selection_compare(u_int16_t row_a, u_int16_t col_a, u_int16_t row_b, u_int16_t col_b) {
    if(row_a != row_b) return row_a < row_b? -1: 1;
    if(col_a != col_b) return col_a < col_b? -1: 1;
    return 0;
}

bool plugin_is_selected(u_int16_t row, u_int16_t col) {
    if(!SELECT_VISIBLE) return false;
    u_int16_t first_row = SELECT_START_ROW, first_col = SELECT_START_COL;
    u_int16_t last_row = SELECT_END_ROW, last_col = SELECT_END_COL;
    if(selection_compare(first_row, first_col, last_row, last_col) > 0) {
        first_row = SELECT_END_ROW; first_col = SELECT_END_COL;
        last_row = SELECT_START_ROW; last_col = SELECT_START_COL;
    }
    return selection_compare(row, col, first_row, first_col) >= 0 &&
           selection_compare(row, col, last_row, last_col) < 0;
}

// Syntax color is deliberately only a display plugin: file content stays clean.
void plugin_highlight(char* result, char* row, int line_no) {
    char temp_line[MAX_NUMBER_OF_COLS << 8];
    char* ptr = temp_line;
    ptr[0] = '\0';

    size_t len = strlen(row);
    size_t width = TERM_COL > LINE_GUTTER? TERM_COL - LINE_GUTTER: 1;
    size_t start = 0;
    if(line_no == CURRENT_ROW && CURRENT_COL >= width) start = CURRENT_COL - width + 1;
    size_t finish = start + width;
    size_t displayed_len = len + (line_no == CURRENT_ROW);
    if(finish > displayed_len) finish = displayed_len;
    if(line_no == CURRENT_ROW) CURRENT_VIEW_COL = start;

    for (size_t i = start; i < finish; i++) {
        char ch = i < len? row[i]: ' ';
        const char* color = i < len? plugin_color_at(row, i): "";
        if(plugin_is_selected(line_no, i)) {
            ptr += sprintf(ptr, "%s\033[48;5;24m%c\033[0m", color, ch);
        } else if(line_no == CURRENT_ROW && i == CURRENT_COL) {
            ptr += sprintf(ptr, "%s\033[7m%c\033[0m", color, ch);
        } else {
            ptr += sprintf(ptr, "%s%c\033[0m", color, ch);
        }
    }
    ptr += sprintf(ptr, "\n");

    strcat(result, temp_line);
}

// Keep the important state visible without taking space from the buffer.
void plugin_status_bar() {
    char status[PATHMAX + 128];
    const char* filename = INIT_FILE? INIT_ARG_FNAME: "[scratch]";
    const char* language = FILE_LANGUAGE == LANGUAGE_C? "C":
                           FILE_LANGUAGE == LANGUAGE_PYTHON? "PY": "TEXT";
    if(CONFIRM_EXIT && INIT_FILE) {
        snprintf(status, sizeof(status), " Save changes before exit?  y save | n discard | c cancel ");
    } else if(CONFIRM_EXIT) {
        snprintf(status, sizeof(status), " No filename: c cancel, then use =filename and Ctrl+N | n discard ");
    } else {
        snprintf(status, sizeof(status), " light | %s | %s | %s%s | %d:%d | ^Space select  Enter copy  d delete  ^V paste ",
                 SELECT_ACTIVE? "SELECT": "EDIT", language, filename, BUFFER_DIRTY? " [+]": "",
                 CURRENT_ROW + 1, CURRENT_COL + 1);
    }
    printf("\033[%d;1H\033[7m%-*.*s\033[0m", TERM_ROW, TERM_COL, TERM_COL, status);
}


// plugins and shortcuts go hand in hand, this is an example
// where a plugin might call a shortcut 
void shortcut_delete_curr_line(char);
void normalize_COL();

// Concatenate strings in DISPLAY_BUFFER with newline character
// char* result iterates over all ROWS and COLUMNS, this is a 
// nice place to use your plugins
char* join_display_buffer() {
    get_terminal_size();
    int viewport_rows = TERM_ROW > 1? TERM_ROW - 1: 1;
    int start_line = VIEW_START_ROW;
    int latest_start = NUMBER_OF_ROWS - viewport_rows + 1;
    if(CURRENT_ROW < start_line) start_line = CURRENT_ROW;
    if(CURRENT_ROW >= start_line + viewport_rows) start_line = CURRENT_ROW - viewport_rows + 1;
    if(start_line < 0) start_line = 0;
    if(latest_start < 0) latest_start = 0;
    if(start_line > latest_start) start_line = latest_start;
    int end_line = start_line + viewport_rows - 1;
    if(end_line > NUMBER_OF_ROWS) end_line = NUMBER_OF_ROWS;
    VIEW_START_ROW = start_line;

    size_t total_len = 0;
    for (int i = start_line; i <= end_line; i++) {
        total_len += strlen(DISPLAY_BUFFER[i]) + 1;   
    }

    // line number + highlight escape codes + the cursor's extra blank
    size_t visible_rows = end_line - start_line + 1;
    size_t decoration_len = total_len * 24 + visible_rows * 48 + 1;
    char *result = malloc(total_len + decoration_len);
    if (!result) return NULL;

    result[0] = '\0'; 
    for (int i = start_line; i <= end_line; i++) {
            
        // Add your plugins here
        plugin_show_line_colored(result, i);
        plugin_highlight(result, DISPLAY_BUFFER[i], i);
    }

    return result;
}

// normal IO compared to before scatter gather io
bool write_all(int fd, const char* buffer, size_t len) {
    while(len > 0) {
        ssize_t written = write(fd, buffer, len);
        if(written < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        buffer += written;
        len -= written;
    }
    return true;
}

void save_buffer_to_file(const char* filename, bool called_through_shortcut) {
    if(filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "Can not save: filename is empty\n");
        return;
    }

    char temporary[PATHMAX + 16];
    if(snprintf(temporary, sizeof(temporary), "%s.light-XXXXXX", filename) >= (int)sizeof(temporary)) {
        fprintf(stderr, "Can not save: file path is too long\n");
        return;
    }

    struct stat old_file;
    mode_t mode = stat(filename, &old_file) == 0? old_file.st_mode & 0777: 0644;
    int fd = mkstemp(temporary);
    if (fd == -1) {
        perror("save");
        return;
    }
    fchmod(fd, mode);

    for (int i = 0; i <= NUMBER_OF_ROWS; i++) {
        size_t len = strlen(DISPLAY_BUFFER[i]);

        if(!write_all(fd, DISPLAY_BUFFER[i], len)) {
            perror("write");
            close(fd);
            unlink(temporary);
            return;
        }

        if(i < NUMBER_OF_ROWS || BUFFER_ENDS_NEWLINE) {
          if(!write_all(fd, "\n", 1)) {
            perror("write");
            close(fd);
            unlink(temporary);
            return;
          }
        }
    }

    if(fsync(fd) == -1 || close(fd) == -1) {
        perror("save");
        unlink(temporary);
        return;
    }
    if(rename(temporary, filename) == -1) {
        perror("rename");
        unlink(temporary);
        return;
    }

    BUFFER_DIRTY = false;

    if (called_through_shortcut) {
        IGN_FILE = SAVE_FILE = EXIT_FLAG = false;
    } else {
        IGN_FILE = false;
        SAVE_FILE = true;
        EXIT_FLAG = true;
    }

    check_EXIT((char*)filename, called_through_shortcut); // type checker : discard const qualifier
}

// A line containing =<filename> chooses a filename. Ctrl+N remains
// the only shortcut which writes the buffer.
bool checkpoint() {
    char* command = DISPLAY_BUFFER[NUMBER_OF_ROWS];
    if(command[0] != '=') return false;

    char filename[PATHMAX];
    size_t len = strcspn(command + 1, " \t");
    if(len == 0 || len >= sizeof(filename)) {
        fprintf(stderr, "Can not save: invalid filename\n");
        return false;
    }

    memcpy(filename, command + 1, len);
    filename[len] = '\0';

    memset(DISPLAY_BUFFER[NUMBER_OF_ROWS], 0, MAX_NUMBER_OF_COLS);
    if(NUMBER_OF_ROWS > 0) {
        NUMBER_OF_ROWS--;
        BUFFER_ENDS_NEWLINE = true;
    } else {
        BUFFER_ENDS_NEWLINE = false;
    }
    strcpy(INIT_ARG_FNAME, filename);
    detect_language(INIT_ARG_FNAME);
    INIT_FILE = true;
    BUFFER_DIRTY = true;
    CURRENT_ROW = NUMBER_OF_ROWS;
    CURRENT_COL = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
    return true;
}

// :<row-number> is a transient command: Enter removes it, then jumps.
bool shortcut_goto_typed_line() {
    char* command = DISPLAY_BUFFER[CURRENT_ROW];
    if(command[0] != ':' || command[1] == '\0') return false;

    for(size_t i = 1; command[i] != '\0'; i++) {
        if(command[i] < '0' || command[i] > '9') return false;
    }

    unsigned long target = strtoul(command + 1, NULL, 10);
    if(target > NUMBER_OF_ROWS) return false;

    shortcut_delete_curr_line('D');
    CURRENT_ROW = target > NUMBER_OF_ROWS? NUMBER_OF_ROWS: target;
    normalize_COL();
    BUFFER_DIRTY = true;
    return true;
}

// Check for overflow and underflow in CURRENT_ROW 
void normalize_ROW() {
    if(CURRENT_ROW > NUMBER_OF_ROWS) {
        CURRENT_ROW = NUMBER_OF_ROWS;
    }

    return;
}

// Check for overflow and underflow in CURRENT_COL
void normalize_COL() {

    unsigned int this_row_number_of_cols = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
    if(CURRENT_COL > this_row_number_of_cols) {
        CURRENT_COL = this_row_number_of_cols;
    } else if(this_row_number_of_cols > MAX_NUMBER_OF_COLS) {
        fprintf(stderr, "you are at %d\n", this_row_number_of_cols);
        fprintf(stderr, "you can not extend beyond MAX_NUMBER_OF_COLS(unrecoverable error)\n");
        fprintf(stderr, "you can try changing MAX_NUMBER_OF_COLS\n");
        IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
        save_buffer_to_file(SCRATCH_FILE, !CALLED_THROUGH_SHORTCUT);  
    }

    return;
}

void copy_selection_to_terminal() {
    if(!SELECT_VISIBLE || selection_compare(SELECT_START_ROW, SELECT_START_COL,
       SELECT_END_ROW, SELECT_END_COL) == 0) return;

    u_int16_t first_row = SELECT_START_ROW, first_col = SELECT_START_COL;
    u_int16_t last_row = SELECT_END_ROW, last_col = SELECT_END_COL;
    if(selection_compare(first_row, first_col, last_row, last_col) > 0) {
        first_row = SELECT_END_ROW; first_col = SELECT_END_COL;
        last_row = SELECT_START_ROW; last_col = SELECT_START_COL;
    }

    size_t capacity = 1;
    for(u_int16_t row = first_row; row <= last_row; row++) {
        capacity += strlen(DISPLAY_BUFFER[row]) + 1;
        if(row == last_row) break;
    }
    if(capacity > 1024 * 1024) return; // terminal clipboards dislike enormous OSC messages

    char* plain = malloc(capacity);
    if(!plain) return;
    size_t used = 0;
    for(u_int16_t row = first_row; row <= last_row; row++) {
        size_t begin = row == first_row? first_col: 0;
        size_t end = row == last_row? last_col: strlen(DISPLAY_BUFFER[row]);
        size_t row_len = strlen(DISPLAY_BUFFER[row]);
        if(begin > row_len) begin = row_len;
        if(end > row_len) end = row_len;
        if(end > begin) {
            memcpy(plain + used, DISPLAY_BUFFER[row] + begin, end - begin);
            used += end - begin;
        }
        if(row != last_row) plain[used++] = '\n';
        if(row == last_row) break;
    }

    char* clipboard = malloc(used + 1);
    if(!clipboard) { free(plain); return; }
    memcpy(clipboard, plain, used);
    clipboard[used] = '\0';
    free(TEXT_CLIPBOARD);
    TEXT_CLIPBOARD = clipboard;
    TEXT_CLIPBOARD_LEN = used;

    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_len = ((used + 2) / 3) * 4;
    char* encoded = malloc(encoded_len + 1);
    if(!encoded) { free(plain); return; }
    size_t out = 0;
    for(size_t i = 0; i < used; i += 3) {
        unsigned int value = (unsigned char)plain[i] << 16;
        if(i + 1 < used) value |= (unsigned char)plain[i + 1] << 8;
        if(i + 2 < used) value |= (unsigned char)plain[i + 2];
        encoded[out++] = alphabet[(value >> 18) & 63];
        encoded[out++] = alphabet[(value >> 12) & 63];
        encoded[out++] = i + 1 < used? alphabet[(value >> 6) & 63]: '=';
        encoded[out++] = i + 2 < used? alphabet[value & 63]: '=';
    }
    encoded[out] = '\0';
    printf("\033]52;c;%s\a", encoded);
    free(encoded);
    free(plain);
}

void shortcut_mouse(struct Key key) {
    if(key.mouse_y < 1 || key.mouse_y >= TERM_ROW) return;
    unsigned int row = VIEW_START_ROW + key.mouse_y - 1;
    if(row > NUMBER_OF_ROWS) row = NUMBER_OF_ROWS;
    unsigned int col = key.mouse_x > LINE_GUTTER? key.mouse_x - LINE_GUTTER - 1: 0;
    if(row == CURRENT_ROW) col += CURRENT_VIEW_COL;
    size_t row_len = strlen(DISPLAY_BUFFER[row]);
    if(col > row_len) col = row_len;

    if(key.mouse_pressed && !(key.mouse_button & 32) && (key.mouse_button & 3) == 0) {
        CURRENT_ROW = row;
        CURRENT_COL = col;
        SELECT_VISIBLE = SELECT_ACTIVE = false;
    }
}

void shortcut_select(char ch) {
    if(ch != ' ') return;
    if(!SELECT_ACTIVE) {
        SELECT_START_ROW = SELECT_END_ROW = CURRENT_ROW;
        SELECT_START_COL = SELECT_END_COL = CURRENT_COL;
        SELECT_VISIBLE = SELECT_ACTIVE = true;
    } else {
        SELECT_ACTIVE = SELECT_VISIBLE = false;
    }
}

void selection_follows_cursor() {
    if(!SELECT_ACTIVE) {
        SELECT_VISIBLE = false;
        return;
    }
    SELECT_END_ROW = CURRENT_ROW;
    SELECT_END_COL = CURRENT_COL;
    SELECT_VISIBLE = true;
}

void shortcut_copy(char ch) {
    if(ch == 'C' && SELECT_VISIBLE) copy_selection_to_terminal();
}

void delete_selected_text() {
    if(!SELECT_VISIBLE) return;
    u_int16_t first_row = SELECT_START_ROW, first_col = SELECT_START_COL;
    u_int16_t last_row = SELECT_END_ROW, last_col = SELECT_END_COL;
    if(selection_compare(first_row, first_col, last_row, last_col) > 0) {
        first_row = SELECT_END_ROW; first_col = SELECT_END_COL;
        last_row = SELECT_START_ROW; last_col = SELECT_START_COL;
    }
    if(selection_compare(first_row, first_col, last_row, last_col) == 0) return;

    if(first_row == last_row) {
        memmove(DISPLAY_BUFFER[first_row] + first_col,
                DISPLAY_BUFFER[first_row] + last_col,
                strlen(DISPLAY_BUFFER[first_row] + last_col) + 1);
    } else {
        size_t prefix = first_col;
        size_t suffix = strlen(DISPLAY_BUFFER[last_row] + last_col);
        if(prefix + suffix < MAX_NUMBER_OF_COLS) {
            memcpy(DISPLAY_BUFFER[first_row] + prefix,
                   DISPLAY_BUFFER[last_row] + last_col, suffix + 1);
        } else DISPLAY_BUFFER[first_row][prefix] = '\0';

        u_int16_t removed_rows = last_row - first_row;
        for(u_int16_t row = first_row + 1; row + removed_rows <= NUMBER_OF_ROWS; row++) {
            memcpy(DISPLAY_BUFFER[row], DISPLAY_BUFFER[row + removed_rows], MAX_NUMBER_OF_COLS);
        }
        for(u_int16_t row = NUMBER_OF_ROWS - removed_rows + 1; row <= NUMBER_OF_ROWS; row++) {
            memset(DISPLAY_BUFFER[row], 0, MAX_NUMBER_OF_COLS);
            if(row == NUMBER_OF_ROWS) break;
        }
        NUMBER_OF_ROWS -= removed_rows;
    }
    CURRENT_ROW = first_row;
    CURRENT_COL = first_col;
    SELECT_VISIBLE = SELECT_ACTIVE = false;
}

void shortcut_paste_text(char ch) {
    if(ch != 'V' || !TEXT_CLIPBOARD || TEXT_CLIPBOARD_LEN == 0) return;
    delete_selected_text();

    for(size_t at = 0; at < TEXT_CLIPBOARD_LEN; at++) {
        if(TEXT_CLIPBOARD[at] == '\n') {
            if(NUMBER_OF_ROWS >= MAX_NUMBER_OF_ROWS - 1) break;
            for(int row = NUMBER_OF_ROWS; row > CURRENT_ROW; row--) {
                memcpy(DISPLAY_BUFFER[row + 1], DISPLAY_BUFFER[row], MAX_NUMBER_OF_COLS);
            }
            char tail[MAX_NUMBER_OF_COLS];
            strcpy(tail, DISPLAY_BUFFER[CURRENT_ROW] + CURRENT_COL);
            DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] = '\0';
            strcpy(DISPLAY_BUFFER[CURRENT_ROW + 1], tail);
            NUMBER_OF_ROWS++;
            CURRENT_ROW++;
            CURRENT_COL = 0;
        } else {
            size_t len = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
            if(len + 1 >= MAX_NUMBER_OF_COLS) continue;
            memmove(DISPLAY_BUFFER[CURRENT_ROW] + CURRENT_COL + 1,
                    DISPLAY_BUFFER[CURRENT_ROW] + CURRENT_COL,
                    len - CURRENT_COL + 1);
            DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL++] = TEXT_CLIPBOARD[at];
        }
    }
    BUFFER_DIRTY = true;
}

// One honest undo is more useful than a complicated history that lies.
// The snapshot only contains rows currently in use.
void remember_for_undo() {
    size_t bytes = ((size_t)NUMBER_OF_ROWS + 1) * MAX_NUMBER_OF_COLS;
    char* snapshot = malloc(bytes);
    if(!snapshot) return;
    memcpy(snapshot, DISPLAY_BUFFER, bytes);
    free(UNDO_STATE.buffer);
    UNDO_STATE.buffer = snapshot;
    UNDO_STATE.rows = NUMBER_OF_ROWS;
    UNDO_STATE.row = CURRENT_ROW;
    UNDO_STATE.col = CURRENT_COL;
    UNDO_STATE.ends_newline = BUFFER_ENDS_NEWLINE;
    UNDO_STATE.valid = true;
}

void shortcut_undo(char ch) {
    if(ch != 'Z' || !UNDO_STATE.valid) return;

    size_t current_bytes = ((size_t)NUMBER_OF_ROWS + 1) * MAX_NUMBER_OF_COLS;
    size_t undo_bytes = ((size_t)UNDO_STATE.rows + 1) * MAX_NUMBER_OF_COLS;
    char* redo = malloc(current_bytes);
    if(!redo) return;
    memcpy(redo, DISPLAY_BUFFER, current_bytes);
    memset(DISPLAY_BUFFER, 0, current_bytes > undo_bytes? current_bytes: undo_bytes);
    memcpy(DISPLAY_BUFFER, UNDO_STATE.buffer, undo_bytes);

    free(UNDO_STATE.buffer);
    UNDO_STATE.buffer = redo;
    u_int16_t old_rows = NUMBER_OF_ROWS;
    NUMBER_OF_ROWS = UNDO_STATE.rows;
    UNDO_STATE.rows = old_rows;
    u_int16_t old_row = CURRENT_ROW;
    CURRENT_ROW = UNDO_STATE.row;
    UNDO_STATE.row = old_row;
    u_int16_t old_col = CURRENT_COL;
    CURRENT_COL = UNDO_STATE.col;
    UNDO_STATE.col = old_col;
    bool old_ending = BUFFER_ENDS_NEWLINE;
    BUFFER_ENDS_NEWLINE = UNDO_STATE.ends_newline;
    UNDO_STATE.ends_newline = old_ending;
    BUFFER_DIRTY = true;
}

// This is a shortcut to get a newline above your current line
// with Ctrl + O
void shortcut_newline_above(char ch) {
    if (ch == 'O') {
        if (NUMBER_OF_ROWS < MAX_NUMBER_OF_ROWS - 1) {
            for (int i = NUMBER_OF_ROWS; i >= CURRENT_ROW; i--) {
                strncpy(DISPLAY_BUFFER[i + 1], DISPLAY_BUFFER[i], MAX_NUMBER_OF_COLS);
            }

            memset(DISPLAY_BUFFER[CURRENT_ROW], 0, MAX_NUMBER_OF_COLS);

            NUMBER_OF_ROWS++; 
            CURRENT_COL = 0;
        }
    }

    return;
}

// This shortcut adds newline below current line, whereas Enter would
// split this line if it was in a middle of the row
// use Ctrl + L
void shortcut_newline_below(char ch) {
    // Go one row below, then add one line above 
    if(ch == 'L') {
        CURRENT_ROW += 1;
        shortcut_newline_above('O');
    }

    return;
}

// This is a shortcut to clear the current line
// with Ctrl + X
void shortcut_clear_curr_line(char ch) {
    if(ch == 'X') {
        memset(DISPLAY_BUFFER[CURRENT_ROW], 0, MAX_NUMBER_OF_COLS);
        CURRENT_COL = 0;
    }

    return;
}

// This is a shortcut to delete the whole line, 
// instead of just clearing it. To use it,
// use Ctrl + D
void shortcut_delete_curr_line(char ch) {
    if(ch == 'D') {
        if (NUMBER_OF_ROWS == 0) {
            memset(DISPLAY_BUFFER[0], 0, MAX_NUMBER_OF_COLS);
            CURRENT_COL = 0;
            return;
        }

        for (int i = CURRENT_ROW; i < NUMBER_OF_ROWS; i++) {
            memmove(DISPLAY_BUFFER[i], DISPLAY_BUFFER[i+1], MAX_NUMBER_OF_COLS);
        }

        memset(DISPLAY_BUFFER[NUMBER_OF_ROWS], 0, MAX_NUMBER_OF_COLS);

        NUMBER_OF_ROWS--;
        if(CURRENT_ROW > NUMBER_OF_ROWS) CURRENT_ROW = NUMBER_OF_ROWS;
        normalize_COL();
    }

    return;
}

// Set cursor to point to end of line
// use Ctrl + B
void shortcut_beginning_of_line(char ch) {
    if(ch == 'B') {
        CURRENT_COL = 0;
    }

    return;
}

// Set cursor to point to end of line
// use Ctrl + E
void shortcut_end_of_line(char ch) {
    if(ch == 'E') {
        CURRENT_COL = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
    }

    return;
}

// Add tab(space) to beginning of line
// use Ctrl + T
void shortcut_add_tab(char ch) {
    if(ch == 'T') {
        size_t len = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
        if(len + TABSPACE >= MAX_NUMBER_OF_COLS) return;
        memmove(DISPLAY_BUFFER[CURRENT_ROW] + TABSPACE,
                DISPLAY_BUFFER[CURRENT_ROW], len + 1);
        memset(DISPLAY_BUFFER[CURRENT_ROW], ' ', TABSPACE);
        CURRENT_COL += TABSPACE;
    }

    return;
}

// Remove up to one TABSPACE from the beginning, using Ctrl + U.
void shortcut_remove_tab(char ch) {
    if(ch != 'U') return;
    size_t remove = 0;
    while(remove < TABSPACE && DISPLAY_BUFFER[CURRENT_ROW][remove] == ' ') remove++;
    if(remove == 0) return;
    memmove(DISPLAY_BUFFER[CURRENT_ROW], DISPLAY_BUFFER[CURRENT_ROW] + remove,
            strlen(DISPLAY_BUFFER[CURRENT_ROW] + remove) + 1);
    CURRENT_COL = CURRENT_COL > remove? CURRENT_COL - remove: 0;
}

// Ctrl + G duplicates the current line below it.
void shortcut_duplicate_line(char ch) {
    if(ch != 'G' || NUMBER_OF_ROWS >= MAX_NUMBER_OF_ROWS - 1) return;
    for(int i = NUMBER_OF_ROWS; i > CURRENT_ROW; i--) {
        memcpy(DISPLAY_BUFFER[i + 1], DISPLAY_BUFFER[i], MAX_NUMBER_OF_COLS);
    }
    memcpy(DISPLAY_BUFFER[CURRENT_ROW + 1], DISPLAY_BUFFER[CURRENT_ROW], MAX_NUMBER_OF_COLS);
    NUMBER_OF_ROWS++;
    CURRENT_ROW++;
    normalize_COL();
}

// Ctrl + K cuts a line, Ctrl + Y pastes it below the cursor.
void shortcut_cut_line(char ch) {
    if(ch != 'K') return;
    memcpy(LINE_CLIPBOARD, DISPLAY_BUFFER[CURRENT_ROW], MAX_NUMBER_OF_COLS);
    shortcut_delete_curr_line('D');
}

void shortcut_paste_line(char ch) {
    if(ch != 'Y' || LINE_CLIPBOARD[0] == '\0' || NUMBER_OF_ROWS >= MAX_NUMBER_OF_ROWS - 1) return;
    if(NUMBER_OF_ROWS == 0 && DISPLAY_BUFFER[0][0] == '\0') {
        memcpy(DISPLAY_BUFFER[0], LINE_CLIPBOARD, MAX_NUMBER_OF_COLS);
        normalize_COL();
        return;
    }
    for(int i = NUMBER_OF_ROWS; i > CURRENT_ROW; i--) {
        memcpy(DISPLAY_BUFFER[i + 1], DISPLAY_BUFFER[i], MAX_NUMBER_OF_COLS);
    }
    NUMBER_OF_ROWS++;
    CURRENT_ROW++;
    memcpy(DISPLAY_BUFFER[CURRENT_ROW], LINE_CLIPBOARD, MAX_NUMBER_OF_COLS);
    normalize_COL();
}

// Go to first line, using Ctrl + W
void shortcut_goto_first_line(char ch) {
    if(ch == 'W') {
        CURRENT_ROW = 0;
    }

    return;
}

// Go to last line, using Ctrl + A
void shortcut_goto_last_line(char ch) {
    if(ch == 'A') {
        CURRENT_ROW = NUMBER_OF_ROWS;
    }

    return;
}

// This shortcut updates any file opened with 
// light <filename>
// Use with Ctrl + N
// Be aware though, it might take a bit longer
// than expected
void shortcut_save_file(char ch) {
    if(INIT_FILE == true) {
        if(ch == 'N') {
            save_buffer_to_file(INIT_ARG_FNAME, CALLED_THROUGH_SHORTCUT);
        }
    }
}

void shortcut_quit(char ch) {
    if(ch == 'Q') {
        if(BUFFER_DIRTY) CONFIRM_EXIT = true;
        else EXIT_FLAG = true;
    }
}


// This function, pads the string output vertically, 
// because RAW_MODE disables scrolling
char* resize_string(const char* result) {
    int viewport_rows = TERM_ROW > 1? TERM_ROW - 1: 1;
    int used_rows = 0;
    for(size_t i = 0; result[i] != '\0'; i++) if(result[i] == '\n') used_rows++;
    size_t result_len = strlen(result);
    char* res = malloc(result_len + (viewport_rows - used_rows) * 5 + 1);
    if(!res) return NULL;
    memcpy(res, result, result_len + 1);
    for(int row = used_rows; row < viewport_rows; row++) strcat(res, "\033[2K\n");
    return res;
}

// Most edits touch one row. Repainting only that row avoids flashing the
// entire viewport for every character typed.
void render_buffer_row(u_int16_t buffer_row) {
    if(buffer_row < VIEW_START_ROW || buffer_row >= VIEW_START_ROW + TERM_ROW - 1) return;
    size_t len = strlen(DISPLAY_BUFFER[buffer_row]);
    char* row = malloc((len + 1) * 24 + 128);
    if(!row) return;
    row[0] = '\0';
    plugin_show_line_colored(row, buffer_row);
    plugin_highlight(row, DISPLAY_BUFFER[buffer_row], buffer_row);
    int screen_row = buffer_row - VIEW_START_ROW + 1;
    printf("\033[%d;1H%s", screen_row, row);
    free(row);
}

void render_current_row() {
    printf("\033[?2026h");
    render_buffer_row(CURRENT_ROW);
    plugin_status_bar();
    printf("\033[?2026l");
    fflush(stdout);
}

void render_vertical_move(u_int16_t old_row, u_int16_t old_view_start) {
    int viewport_rows = TERM_ROW > 1? TERM_ROW - 1: 1;
    printf("\033[?2026h");

    if(CURRENT_ROW < old_view_start) {
        VIEW_START_ROW = CURRENT_ROW;
        printf("\033[1;%dr\033[1;1H\033M\033[r", viewport_rows);
    } else if(CURRENT_ROW >= old_view_start + viewport_rows) {
        VIEW_START_ROW = CURRENT_ROW - viewport_rows + 1;
        printf("\033[1;%dr\033[%d;1H\033D\033[r", viewport_rows, viewport_rows);
    }

    render_buffer_row(old_row);
    if(old_row != CURRENT_ROW) render_buffer_row(CURRENT_ROW);
    plugin_status_bar();
    printf("\033[?2026l");
    fflush(stdout);
}

// EHHHH code duplication is not always so avoidable is it
// Use Ctrl + P to delete a char opposite to backspace
void shortcut_delete_backwards(char ch) {
    if(ch == 'P') {
        if(CURRENT_COL < strlen(DISPLAY_BUFFER[CURRENT_ROW])) {
        CURRENT_COL++;
        if(CURRENT_COL > 0) {
            CURRENT_COL -= 1;

            memmove(&DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL],
                    &DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL + 1],
                    MAX_NUMBER_OF_COLS - CURRENT_COL - 1);

            DISPLAY_BUFFER[CURRENT_ROW][MAX_NUMBER_OF_COLS - 1] = '\0';
        }
        }
    }
}


/*
------------------------------------

- This thread waits for user input at the
console

... Behavior of switch:

-> KEY_CHAR:

    . If CURRENT_COL < len_of_that_row
then, we have an 'insert-mid-line' condition
that arises due to use of KEY_ARROW_* and
KEY_ENTER
    
    . Else, we are solely appending at the end

-> KEY_ENTER:

    . IF CURRENT_COL < len_of_that_row
then, store in new_line the part of that row
from CURRENT_COL...len_of_that_row, then write
to the next line new_line and in the previous line
at CURRENT_COL write '\0'

    . Else, we are solely making a new line beyond
CURRENT_ROW

-> KEY_ARROW_UP:
    
    . If, CURRENT_ROW > 0, then CURRENT_ROW--

    . Else, nothing

-> KEY_ARROW_DOWN:

    . If, CURRENT_ROW < NUMBER_OF_ROWS, CURRENT_ROW++

    . Else, nothing

-> KEY_ARROW_LEFT:

    . If, CURRENT_COL > 0, CURRENT_COL--

    . Else, nothing

-> KEY_ARROW_RIGHT:

    . If, CURRENT_COL < MAX_NUMBER_OF_COLS - 1 && 
    CURRENT_COL < this_row_number_of_cols, CURRENT_COL++

    . Else, nothing
    
-> KEY_BACKSPACE:

    . If, 'mid-row', memmove 
    DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL].. to
    DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL++]..

    . Else, delete from end of row

..

- With CTRL + O, you append a new line above CURRENT_ROW

- To save, into a file, in the last line,
type, 
    =<filename>

------------------------------------
*/
void* buffer_display(void* unused) {
  (void)unused;
  while(true) {
    // lock the mutex acquired by current_char_lock and wait
    // for a signal to be broadcasted
    pthread_mutex_lock(&current_char_lock);
    while(key_queue_read == key_queue_write && !EXIT_FLAG) {
        struct timespec wake_time;
        clock_gettime(CLOCK_REALTIME, &wake_time);
        wake_time.tv_nsec += 100000000;
        if(wake_time.tv_nsec >= 1000000000) {
            wake_time.tv_sec++;
            wake_time.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&current_char_cond, &current_char_lock, &wake_time);
    }
    if(EXIT_FLAG && key_queue_read == key_queue_write) {
        pthread_mutex_unlock(&current_char_lock);
        check_EXIT("", !CALLED_THROUGH_SHORTCUT);
        break;
    }
    current_char = key_queue[key_queue_read];
    key_queue_read = (key_queue_read + 1) % KEY_QUEUE_LEN;
    pthread_cond_signal(&current_char_cond);
    pthread_mutex_unlock(&current_char_lock);
    u_int16_t old_row = CURRENT_ROW;
    u_int16_t old_view_start = VIEW_START_ROW;
    bool redraw_viewport = true;
    bool redraw_vertical = false;

    if(CONFIRM_EXIT) {
        if(current_char.type == KEY_CHAR && (current_char.ch == 'y' || current_char.ch == 'Y')) {
            if(INIT_FILE) {
                save_buffer_to_file(INIT_ARG_FNAME, CALLED_THROUGH_SHORTCUT);
                if(!BUFFER_DIRTY) {
                    CONFIRM_EXIT = false;
                    EXIT_FLAG = true;
                }
            }
        } else if(current_char.type == KEY_CHAR && (current_char.ch == 'n' || current_char.ch == 'N')) {
            CONFIRM_EXIT = false;
            BUFFER_DIRTY = false;
            EXIT_FLAG = true;
        } else if(current_char.type == KEY_CHAR && (current_char.ch == 'c' || current_char.ch == 'C')) {
            CONFIRM_EXIT = false;
        }
        render_current_row();
        continue;
    }

    if(current_char.type == KEY_CHAR || current_char.type == KEY_ENTER ||
       current_char.type == KEY_BACKSPACE ||
       (current_char.type == KEY_CTRL && strchr("OLDXTPUGKYV", current_char.ch))) {
        remember_for_undo();
    }

    if(SELECT_ACTIVE) {
        if(current_char.type == KEY_ENTER) {
            copy_selection_to_terminal();
            SELECT_ACTIVE = SELECT_VISIBLE = false;
            current_char.type = KEY_UNKNOWN;
        } else if(current_char.type == KEY_CHAR) {
            if(current_char.ch == 'd') {
                delete_selected_text();
                BUFFER_DIRTY = true;
            }
            current_char.type = KEY_UNKNOWN;
        } else if(current_char.type == KEY_BACKSPACE ||
                  (current_char.type == KEY_CTRL && current_char.ch != ' ')) {
            current_char.type = KEY_UNKNOWN;
        }
    }

    switch (current_char.type) {
        case KEY_CHAR:   
            {
                size_t len = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
                size_t added = current_char.ch == '\t'? TABSPACE: 1;
                if(len + added >= MAX_NUMBER_OF_COLS) break;

                // Check if we are inserting in the middle of a row
                // instead of appending to the end
                if (CURRENT_COL < len) {
                    memmove(&DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL + added],
                            &DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL],
                            len - CURRENT_COL + 1);
                }
                if(current_char.ch == '\t') {
                    for(int i = 0; i < TABSPACE; i++) {
                        DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] = ' ';
                        CURRENT_COL++;
                    }
                } else {
                    DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] = current_char.ch;
                    CURRENT_COL++;
                }
                BUFFER_DIRTY = true;
                redraw_viewport = false;
            }
            break;

        case KEY_ENTER:
            if(shortcut_goto_typed_line()) break;
            if(checkpoint()) break;
            if (NUMBER_OF_ROWS < MAX_NUMBER_OF_ROWS - 1) {

                // From bottom up, visit each row upto CURRENT_ROW, and shift it 
                // 1 row down. 
                for (int i = NUMBER_OF_ROWS; i > CURRENT_ROW; i--) {
                    strncpy(DISPLAY_BUFFER[i + 1], DISPLAY_BUFFER[i], MAX_NUMBER_OF_COLS);
                }

                // If, we are in the middle of the row, split that row, and save into 'new_line'
                // the part of the row beginning from CURRENT_COL..srtlen(DISPLAY_BUFFER[CURRENT_ROW])
                char new_line[MAX_NUMBER_OF_COLS] = {0};
                if (CURRENT_COL < strlen(DISPLAY_BUFFER[CURRENT_ROW])) {
                    strncpy(new_line, &DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL], MAX_NUMBER_OF_COLS - 1);
                    DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] = '\0';
                }

                strncpy(DISPLAY_BUFFER[CURRENT_ROW + 1], new_line, MAX_NUMBER_OF_COLS);

                NUMBER_OF_ROWS++;
                CURRENT_ROW++;
                CURRENT_COL = 0;
                BUFFER_DIRTY = true;
            } else {
                fprintf(stderr, "you have tried to extend, beyond MAX_NUMBER_OF_ROWS, which is an unrecoverable error\n");
                fprintf(stderr, "change MAX_NUMBER_OF_ROWS to a higher value(RARE CASE)\n");
                IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
                save_buffer_to_file(SCRATCH_FILE, !CALLED_THROUGH_SHORTCUT);
            }

            normalize_COL();
            break;

        case KEY_ARROW_UP:  
          if(CURRENT_ROW > 0) CURRENT_ROW -= 1;
          
          normalize_ROW();
          normalize_COL();
          selection_follows_cursor();
          redraw_viewport = false;
          redraw_vertical = true;
          break;

        case KEY_ARROW_DOWN:
          if(CURRENT_ROW < NUMBER_OF_ROWS) CURRENT_ROW += 1;
          
          normalize_ROW();
          normalize_COL();
          selection_follows_cursor();
          redraw_viewport = false;
          redraw_vertical = true;
          break;

        case KEY_ARROW_LEFT:
          if(CURRENT_COL > 0) CURRENT_COL -= 1;
          selection_follows_cursor();
          redraw_viewport = false;
          break;

        case KEY_ARROW_RIGHT:
          // you may not go beyond last colum
          if (CURRENT_COL < MAX_NUMBER_OF_COLS - 1 && CURRENT_COL < strlen(DISPLAY_BUFFER[CURRENT_ROW])) {
            CURRENT_COL++;
          }
          selection_follows_cursor();
          redraw_viewport = false;
          break;

        case KEY_WORD_LEFT:
          while(CURRENT_COL > 0 && !((DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] >= 'A' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] <= 'Z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] >= 'a' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] <= 'z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] >= '0' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] <= '9') ||
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] == '_')) CURRENT_COL--;
          while(CURRENT_COL > 0 && ((DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] >= 'A' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] <= 'Z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] >= 'a' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] <= 'z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] >= '0' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] <= '9') ||
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL - 1] == '_')) CURRENT_COL--;
          selection_follows_cursor();
          redraw_viewport = false;
          break;

        case KEY_WORD_RIGHT: {
          size_t row_len = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
          while(CURRENT_COL < row_len && ((DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] >= 'A' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] <= 'Z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] >= 'a' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] <= 'z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] >= '0' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] <= '9') ||
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] == '_')) CURRENT_COL++;
          while(CURRENT_COL < row_len && !((DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] >= 'A' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] <= 'Z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] >= 'a' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] <= 'z') ||
                (DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] >= '0' &&
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] <= '9') ||
                DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL] == '_')) CURRENT_COL++;
          selection_follows_cursor();
          redraw_viewport = false;
          break;
        }

        case KEY_HOME:
          CURRENT_COL = 0;
          selection_follows_cursor();
          redraw_viewport = false;
          break;

        case KEY_END:
          CURRENT_COL = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
          selection_follows_cursor();
          redraw_viewport = false;
          break;

        case KEY_PAGE_UP: {
          u_int16_t jump = TERM_ROW > 2? TERM_ROW - 2: 1;
          CURRENT_ROW = CURRENT_ROW > jump? CURRENT_ROW - jump: 0;
          normalize_COL();
          selection_follows_cursor();
          break;
        }

        case KEY_PAGE_DOWN: {
          u_int16_t jump = TERM_ROW > 2? TERM_ROW - 2: 1;
          CURRENT_ROW = CURRENT_ROW + jump < NUMBER_OF_ROWS? CURRENT_ROW + jump: NUMBER_OF_ROWS;
          normalize_COL();
          selection_follows_cursor();
          break;
        }

        case KEY_BACKSPACE: 
          if(CURRENT_COL > 0) {
            CURRENT_COL -= 1;

            memmove(&DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL],
                    &DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL + 1],
                    MAX_NUMBER_OF_COLS - CURRENT_COL - 1);

            DISPLAY_BUFFER[CURRENT_ROW][MAX_NUMBER_OF_COLS - 1] = '\0';
            BUFFER_DIRTY = true;
            redraw_viewport = false;
          } else if(CURRENT_ROW > 0) {
            size_t previous_len = strlen(DISPLAY_BUFFER[CURRENT_ROW - 1]);
            size_t current_len = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
            if(previous_len + current_len < MAX_NUMBER_OF_COLS) {
              memcpy(DISPLAY_BUFFER[CURRENT_ROW - 1] + previous_len,
                     DISPLAY_BUFFER[CURRENT_ROW], current_len + 1);
              shortcut_delete_curr_line('D');
              CURRENT_COL = previous_len;
              BUFFER_DIRTY = true;
            }
          }
          break;

        case KEY_MOUSE:
          shortcut_mouse(current_char);
          break;

        // With Ctrl, you have the ability to add Shortcuts
        // I define Shortcuts as, functions that take in a
        // character along with Ctrl, and update
        // the DISPLAY_BUFFER
        case KEY_CTRL:
            shortcut_newline_above(current_char.ch);
            shortcut_newline_below(current_char.ch);
            shortcut_clear_curr_line(current_char.ch);
            shortcut_delete_curr_line(current_char.ch);
            shortcut_end_of_line(current_char.ch);
            shortcut_beginning_of_line(current_char.ch);
            shortcut_add_tab(current_char.ch);
            shortcut_remove_tab(current_char.ch);
            shortcut_duplicate_line(current_char.ch);
            shortcut_cut_line(current_char.ch);
            shortcut_paste_line(current_char.ch);
            shortcut_goto_first_line(current_char.ch);
            shortcut_goto_last_line(current_char.ch);
            shortcut_save_file(current_char.ch);
            shortcut_delete_backwards(current_char.ch);
            shortcut_select(current_char.ch);
            shortcut_copy(current_char.ch);
            shortcut_paste_text(current_char.ch);
            shortcut_undo(current_char.ch);
            shortcut_quit(current_char.ch);
            if(strchr("BEWA", current_char.ch) != NULL) selection_follows_cursor();
            if(strchr("OLDXTPUGKYV", current_char.ch) != NULL) BUFFER_DIRTY = true;
            if(strchr("BEPTUX", current_char.ch) != NULL) redraw_viewport = false;
            break;

        default: break;
    }


    if(redraw_vertical) {
      render_vertical_move(old_row, old_view_start);
    } else if(redraw_viewport) {
      char* joined = join_display_buffer();
      char* resized = joined? resize_string(joined): NULL;
      // Synchronized output prevents the terminal from showing a half-drawn frame.
      printf("\033[?2026h\033[H%s", resized? resized: "");
      plugin_status_bar();
      printf("\033[?2026l");
      fflush(stdout);
      free(resized);
      free(joined);
    } else render_current_row();
   
  }

  return NULL;
}

int main(int argc, char* argv[]) {

    // Start by checking, if filename is provided, or buffer
    // is to be created from scratch. If filename, is provided 
    // construct a buffer using its contents, else zero out
    // memory of DISPLAY_BUFFER
    if (argc > 1) {
        char* open_file = argv[1];
        FILE* fd_open_file = fopen(open_file, "r+");
        if(fd_open_file == NULL && errno == ENOENT) fd_open_file = fopen(open_file, "w+");

        if (fd_open_file == NULL) {
            fprintf(stderr, "could, not open file: %s\n", open_file);
            INIT_FILE = false; 
        } else {
            char* temp_line = malloc(MAX_NUMBER_OF_COLS);
            if (!temp_line) {
                perror("malloc failed");
                exit(1);
            }

            if(strlen(argv[1]) >= sizeof(INIT_ARG_FNAME)) {
                fprintf(stderr, "File path is too long\n");
                free(temp_line);
                fclose(fd_open_file);
                return 1;
            }
            strcpy(INIT_ARG_FNAME, argv[1]);
            detect_language(INIT_ARG_FNAME);

            int i = 0;
            while (fgets(temp_line, MAX_NUMBER_OF_COLS, fd_open_file)) {
                if (i >= MAX_NUMBER_OF_ROWS) {
                    fprintf(stderr, "File too long, truncating at %d lines\n", MAX_NUMBER_OF_ROWS);
                    break;
                }
                // copy line safely
                strncpy(DISPLAY_BUFFER[i], temp_line, MAX_NUMBER_OF_COLS - 1);
                DISPLAY_BUFFER[i][MAX_NUMBER_OF_COLS-1] = '\0';

                size_t len = strlen(DISPLAY_BUFFER[i]);
                if (len > 0 && DISPLAY_BUFFER[i][len - 1] == '\n') {
                    DISPLAY_BUFFER[i][len - 1] = '\0';
                    BUFFER_ENDS_NEWLINE = true;
                } else {
                    BUFFER_ENDS_NEWLINE = false;
                }

                i++;
            }

            NUMBER_OF_ROWS = i > 0? i - 1: 0;
            INIT_FILE = true; 
            free(temp_line);
            fclose(fd_open_file);

            // Begin buffer at row number 0
            CURRENT_ROW = 0;  
        }
    }


  if(INIT_FILE == false) {
    // zero out DISPLAY_BUFFER
    for(int i=0; i<MAX_NUMBER_OF_ROWS; i++) memset(DISPLAY_BUFFER[i], 0, MAX_NUMBER_OF_COLS);
  }

  // current_char at the beginning is set to KEY_UNKNOWN
  current_char = (struct Key){ .type = KEY_UNKNOWN, .ch = 0 }; 

  // in case of SIGINT, set EXIT_FLAG
  struct sigaction interrupt_action = {0};
  interrupt_action.sa_handler = handler_SIGINT;
  sigemptyset(&interrupt_action.sa_mask);
  sigaction(SIGINT, &interrupt_action, NULL);

  // set terminal to raw mode
  set_terminal_raw_mode(true);

  // clear the screen and print the initial empty DISPLAY_BUFFER
  // or the file-content initialized DISPLAY_BUFFER: all the same, to me
  char* joined = join_display_buffer();
  char* resized = joined? resize_string(joined): NULL;
  printf("\033[?2026h\033[H\033[2J%s", resized? resized: "");
  plugin_status_bar();
  printf("\033[?2026l");
  free(resized);
  free(joined);

  // get terminal sizes
  get_terminal_size();

  // create two worker threads, one to check for input at
  // the terminal, and the other to manipulate the 
  // dipslay buffer and show it
  pthread_create(&get_input, NULL, input, NULL);
  pthread_create(&display_buffer, NULL, buffer_display, NULL);

  pthread_join(get_input, NULL);
  pthread_join(display_buffer, NULL);

  // set terminal to normal mode
  set_terminal_raw_mode(false);
}
