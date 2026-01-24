/*   
    \----------------------------------------------------------/
    |                        light                             |
    /----------------------------------------------------------\
    |                                                          |
    | light is a terminal utility to buffer medium amount of   |
    | text.                                                    |
    x..........................................................x
    |                                                          |
    | It has all the familiar features to a text editor and is |
    | extremely fast and easy to use                           |
    |                                                          |
    |                                                          |
    |                (1) FASTER THAN VIM                       |
    |               (2) EASIER THAN NEOVIM                     |
    |                 (3) UNDER 1000LOC                        |
    |                                                          |
    | (1) is untested but remains conclusive due to (3)        |
    | (2) is untested but remains conclusive due to (3)        |
    |                                                          |
    |                                                          |
    x..........................................................x 
    |                       MECHANISMS                         |
    x..........................................................x 
    |                                                          |
    | As you dig into the code, you will realize that all      |
    | text is buffered in a 2D array <DISPLAY_BUFFER> suppo-   |
    | rting upto <MAX_NUMBER_OF_ROWS> and <MAX_NUMBER_OF_COLS> |
    |                                                          |
    | Two threads are spawned to :                             |
    | (a) listen for IO on the 'keyboard'                      |
    | (b) write IO to the buffer                               |
    |                                                          |
    | (a) is spawned through <input> function                  |                                      
    | (b) is spawned through <display_buffer> function         |
    |                                                          |
    | <display_buffer> is the core function and it defines     |
    |                                                          |
    | (a) plugins                                              |
    | (b) shortcuts                                            |
    |                                                          | 
    | (a) are supposed to alter the overall structure of       |
    | core <DISPLAY_BUFFER>.                                   |
    | For example, _line_numbers_ is a plugin,                 |
    |      _text_highlight_ under cursor is a plugin           |
    |                                                          | 
    | (b) are called with Ctrl+ prefix and are designed to     |
    | make writing text easier                                 |
    | For example, adding a _line_above_ or _line_below_       |
    |   the current line is a shortcut,                        |
    |   _saving_ the file is a shortcut,                       |
    |   _going_to_end_of_line_ is a shortcut.                  |
    |                                                          |
    x..........................................................x 
    |                       LICENSE                            |
    x..........................................................x 
    |                                                          |
    | This work is protected under GPL License, see ./LICENSE  |
    |                                                          |
    \----------------------------------------------------------/
    |                        light                             |
    /----------------------------------------------------------\
*/ 

#include<stdio.h>
#include<unistd.h>
#include<fcntl.h>
#include<signal.h>
#include<string.h>
#include<stdlib.h>

#include<malloc.h>
#include<memory.h>

#include<sys/uio.h>
#include<sys/ioctl.h>
#include<sys/types.h>

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
#define PATHMAX               0x64 
#define TABSPACE              4

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
bool      EXIT_FLAG      = false;
bool      SAVE_FILE      = false;
bool      IGN_FILE       = true;
bool      INIT_FILE      = false;
char      INIT_ARG_FNAME[PATHMAX];
char      DISPLAY_BUFFER[MAX_NUMBER_OF_ROWS][MAX_NUMBER_OF_COLS];
void      save_buffer_to_file(const char*, bool);
void      check_EXIT(char* filename, bool called_through_shortcut) {

  // if called_through_shortcut, just update that file, and continue  
  if(called_through_shortcut == CALLED_THROUGH_SHORTCUT) { return; }

  if(EXIT_FLAG) {
    system("clear");
    fflush(stdout);
    if(SAVE_FILE || INIT_FILE) {
        if(INIT_FILE) fprintf(stdout, "Saved clean buffer to %s.\n", INIT_ARG_FNAME);
        else fprintf(stdout, "Saved clean buffer to %s.\n", filename);
    } else { // if no filename is provided during initialization saves as .scratchfile
        char fname[] = SCRATCH_FILE;
        int fd = mkstemp(fname); // only required to get template
        save_buffer_to_file(fname, false);
        fprintf(stdout, "Saved dirty buffer to SCRATCH_FILE with template signature %s\n", fname);
    }

    // restore cursor visibility
    printf("\033[?25h"); 
    fflush(stdout);

    _exit(0);
  }

  return;
}
void      handler_SIGINT() {
  EXIT_FLAG = true;
  check_EXIT("", !CALLED_THROUGH_SHORTCUT);

  return;
}

// Disable buffer and echo in terminal, to capture sequences
// like up, down, left, right, enter as characters
void set_terminal_raw_mode(bool yes) {
  static struct termios old_t, new_t; 

  if(yes) {
    tcgetattr(STDIN_FILENO, &old_t);
    new_t = old_t;
    new_t.c_lflag &= ~( ICANON | ECHO );
    tcsetattr(STDIN_FILENO, TCSANOW, &new_t);
  } else {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
  }

  // hide cursor
  printf("\033[?25l"); 
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
    KEY_BACKSPACE,
    KEY_ENTER,
    KEY_TAB,
    KEY_ESC
};

// Which key are you exactly pressing?
// 'ch' is valid for KEY_CHAR and KEY_CTRL
struct Key {
    enum KeyType type;
    char ch; 
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
pthread_mutex_t        current_char_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t         current_char_cond = PTHREAD_COND_INITIALIZER;
pthread_t              get_input, display_buffer; 

// Read input continously from terminal and 
// interpret it as any valid struct Key
void input() {
    
  while(true) { 
    int c = getchar();

    if (c == 27) {                        // ESC or arrow current_chars
        int c2 = getchar();
        if (c2 == '[') {
            switch (getchar()) {
                case 'A': current_char.type = KEY_ARROW_UP; break;
                case 'B': current_char.type = KEY_ARROW_DOWN; break;
                case 'C': current_char.type = KEY_ARROW_RIGHT; break;
                case 'D': current_char.type = KEY_ARROW_LEFT; break;
                default: break;
            }
        } else {
            current_char.type = KEY_ESC;      
      }
    } else if (c == 127 || c == 8) {      // Backspace (127 on Linux, 8 in some cases)
        current_char.type = KEY_BACKSPACE;
    }
    else if (c == 10 || c == 13) {        // Enter (LF=10, CR=13)
        current_char.type = KEY_ENTER;
    }
    else if (c == 9) {                    // Tab
        current_char.type = KEY_CHAR;
        current_char.ch = '\t';
    }
    else if (c >= 1 && c <= 26) {         // Ctrl+A (1) to Ctrl+Z (26)
        current_char.type = KEY_CTRL;
        current_char.ch = 'A' + c - 1;
    }
    else if (c >= 32 && c <= 126) {       // Printable ASCII
        current_char.type = KEY_CHAR;
        current_char.ch = c;
    }

    pthread_mutex_lock(&current_char_lock);
    pthread_cond_signal(&current_char_cond);
    pthread_mutex_unlock(&current_char_lock);
  }
  
  return;
}


// You can add plugins, by working with char* result
// in join_display_buffer function. 
// Make sure, to only concatenate to it, and not 
// overwrite it completely, because it stores past
// lines, too.
void plugin_show_line_colored(char* result, int line_no) {
   char* c_line_no = malloc(128);
   sprintf(c_line_no, "%3d; ", line_no); 
   strcat(result, c_line_no);

   free(c_line_no);
   return;
}

// Highlight current row, and current column
// Some style refactoring
// Instead of highlighting entire row, and seperately
// highlighting current col, only highlight curr col
// in curr row, looks much better
void plugin_highlight(char* result, char* row, int line_no) {
    char temp_line[MAX_NUMBER_OF_COLS << 8];
    char* ptr = temp_line;
    ptr[0] = '\0';

    if (line_no == CURRENT_ROW) {
        size_t len = strlen(row);
        for (size_t i = 0; i < len + 1; i++) {
            char ch = (i < len? row[i] : ' ');
            if ((int)i == CURRENT_COL) {
                ptr += sprintf(ptr, "\033[7m%c\033[0m", ch);
            } else {
                ptr += sprintf(ptr, "%c", ch);
            }
        }

        ptr += sprintf(ptr, "\n");
    } else {
        ptr += sprintf(ptr, "%s\n", row);
    }

    strcat(result, temp_line);
}


// plugins and shortcuts go hand in hand, this is an example
// where a plugin might call a shortcut 
void shortcut_delete_curr_line(char);

// This plugin helps you to go to certain rows,
// use as 
// :<row-number>
void plugin_goto_line(char* result, char* buffer, int i) {
    if(i == CURRENT_ROW-1) {
        if(buffer[0] == ':') {
            char line_num[12];

            int i;
            for(i=1; i<strlen(buffer); i++) {
                // fixes ':mango' ~ ':0' problem
                if(buffer[i] < '0' || buffer[i] > '9') return;
                line_num[i-1] = buffer[i];
            }
            line_num[i-1] = '\0';

		// fixed bad behavior ':0' and ':lastline' were not working
		// but now they do
		int target_row = atoi(line_num);
		if((target_row >= 0) && (target_row <= NUMBER_OF_ROWS)) {
		        // Remove the line in which, i type ':number'
		        shortcut_delete_curr_line('D');

		        // Then change rows
		        CURRENT_ROW = target_row;
            }

            buffer[0] = '\0';
        }
    }
}

// Concatenate strings in DISPLAY_BUFFER with newline character
// char* result iterates over all ROWS and COLUMNS, this is a 
// nice place to use your plugins
char* join_display_buffer() {

    size_t total_len = 0;
    for (int i = 0; i <= NUMBER_OF_ROWS && i < MAX_NUMBER_OF_ROWS; i++) {
        total_len += strlen(DISPLAY_BUFFER[i]) + 1;   
    }

    char *result = malloc(total_len << 8);
    if (!result) return NULL;

    result[0] = '\0'; 
    for (int i = 0; i <= NUMBER_OF_ROWS && i < MAX_NUMBER_OF_ROWS; i++) {
            
        // Add your plugins here
        plugin_show_line_colored(result, i);
        plugin_highlight(result, DISPLAY_BUFFER[i], i);
        plugin_goto_line(result, DISPLAY_BUFFER[i], i);
    }

    return result;
}

// normal IO compared to before scatter gather io
void save_buffer_to_file(const char* filename, bool called_through_shortcut) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("open");
        IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
        return;
    }

    for (int i = 0; i <= NUMBER_OF_ROWS; i++) {
        size_t len = strlen(DISPLAY_BUFFER[i]);

        ssize_t w = write(fd, DISPLAY_BUFFER[i], len);
        if (w != (ssize_t)len) {
            perror("write");
            close(fd);
            IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
            return;
        }

        w = write(fd, "\n", 1);
        if (w != 1) {
            perror("write");
            close(fd);
            IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
            return;
        }
    }

    close(fd);

    if (called_through_shortcut) {
        IGN_FILE = SAVE_FILE = EXIT_FLAG = false;
    } else {
        IGN_FILE = false;
        SAVE_FILE = true;
        EXIT_FLAG = true;
    }

    check_EXIT((char*)filename, called_through_shortcut); // type checker : discard const qualifier
}

// Check for overflow and underflow in CURRENT_ROW 
void normalize_ROW() {
    
    if(CURRENT_ROW < 0) {
        CURRENT_ROW = 0;
    } else if(CURRENT_ROW > NUMBER_OF_ROWS) {
        CURRENT_ROW = NUMBER_OF_ROWS;
    } else if(NUMBER_OF_ROWS > MAX_NUMBER_OF_ROWS) {
        fprintf(stderr, "you can not extend NUMBER_OF_ROWS beyond MAX_NUMBER_OF_ROWS(unrecoverable error)\n");
        fprintf(stderr, "you can try changing MAX_NUMBER_OF_ROWS\n");
        IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
        save_buffer_to_file(SCRATCH_FILE, !CALLED_THROUGH_SHORTCUT);
    }

    return;
}

// Check for overflow and underflow in CURRENT_COL
void normalize_COL() {

    unsigned int this_row_number_of_cols = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
    if(CURRENT_COL < 0) {
        CURRENT_COL = 0;
    } else if(CURRENT_COL > this_row_number_of_cols) {
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
        if(NUMBER_OF_ROWS == 0) return;
        strncpy(DISPLAY_BUFFER[CURRENT_ROW], "", strlen(DISPLAY_BUFFER[CURRENT_ROW]));
        CURRENT_COL = 0;
    }

    return;
}

// This is a shortcut to delete the whole line, 
// instead of just clearing it. To use it,
// use Ctrl + D
void shortcut_delete_curr_line(char ch) {
    if(ch == 'D') {
        if (NUMBER_OF_ROWS == 0) return;

        for (int i = CURRENT_ROW; i < NUMBER_OF_ROWS; i++) {
            memmove(DISPLAY_BUFFER[i], DISPLAY_BUFFER[i+1], MAX_NUMBER_OF_COLS);
        }

        memset(DISPLAY_BUFFER[NUMBER_OF_ROWS], 0, MAX_NUMBER_OF_COLS);

        NUMBER_OF_ROWS--;
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
        if(NUMBER_OF_ROWS == 0) return;
        char temp[MAX_NUMBER_OF_COLS];
        strcpy(temp, DISPLAY_BUFFER[CURRENT_ROW]);

        sprintf(DISPLAY_BUFFER[CURRENT_ROW], "    %s", temp);
        CURRENT_COL = strlen(DISPLAY_BUFFER[CURRENT_ROW]);
    }

    return;
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


// This function, pads the string output vertically, 
// because RAW_MODE disables scrolling
char* resize_string(const char* result) {
    char* res = malloc(4096);  
    int write_idx = 0;

    get_terminal_size();

    int n_above = TERM_ROW / 2;
    int n_below = TERM_ROW - n_above;

    if (CURRENT_ROW < TERM_ROW / 2) {
        n_above = CURRENT_ROW + 1; 
        n_below = TERM_ROW - n_above - 1;
    } else if (CURRENT_ROW >= TERM_ROW / 2) {
        n_above = TERM_ROW / 2;
        n_below = TERM_ROW - n_above - 1;
    }

    // Find line ranges
    int start_line = CURRENT_ROW - n_above;
    int end_line   = CURRENT_ROW + n_below;

    if (start_line < 0) start_line = 0;
    if (end_line > NUMBER_OF_ROWS) end_line = NUMBER_OF_ROWS;

    // Walk through result and copy only lines in the window
    int line = 0;
    for (int i = 0; result[i] != '\0'; i++) {
        if (line >= start_line && line <= end_line) {
            res[write_idx++] = result[i];
        }
        if (result[i] == '\n') {
            line++;
        }
        if (line > end_line) break; 
    }

    res[write_idx] = '\0'; 
    return res;
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
void buffer_display() {
  while(true) {
    // check EXIT_FLAG
    check_EXIT("", !CALLED_THROUGH_SHORTCUT);

    // lock the mutex acquired by current_char_lock and wait
    // for a signal to be broadcasted
    pthread_mutex_lock(&current_char_lock);
    pthread_cond_wait(&current_char_cond, &current_char_lock);
    pthread_mutex_unlock(&current_char_lock);

    switch (current_char.type) {
        case KEY_CHAR:   
            if (CURRENT_COL < MAX_NUMBER_OF_COLS - 1) {
                size_t len = strlen(DISPLAY_BUFFER[CURRENT_ROW]);

                // Check if we are inserting in the middle of a row
                // instead of appending to the end
                if (CURRENT_COL < len) {
                    int t = (current_char.ch == '\t'? 4: 1); // check if tab in middle of insertion
                    memmove(&DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL + t],
                            &DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL],
                            len - CURRENT_COL + t);  
                } else {
                    DISPLAY_BUFFER[CURRENT_ROW][len + 1] = '\0'; 
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
            } else {
                fprintf(stderr, "you have tried to extend, beyond MAX_NUMBER_OF_COLS, which is a grave error\n");
                fprintf(stderr, "change MAX_NUMBER_OF_COLS to a higher value(RARE CASE)\n");
                IGN_FILE = SAVE_FILE = EXIT_FLAG = true;
                save_buffer_to_file(SCRATCH_FILE, !CALLED_THROUGH_SHORTCUT);
            }
            break;

        case KEY_ENTER:
            // checkpoint();
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
          break;

        case KEY_ARROW_DOWN:
          if(CURRENT_ROW < NUMBER_OF_ROWS) CURRENT_ROW += 1;
          
          normalize_ROW();
          normalize_COL();
          break;

        case KEY_ARROW_LEFT:
          if(CURRENT_COL > 0) CURRENT_COL -= 1;
          break;

        case KEY_ARROW_RIGHT:
          // you may not go beyond last colum
          if (CURRENT_COL < MAX_NUMBER_OF_COLS - 1 && CURRENT_COL < strlen(DISPLAY_BUFFER[CURRENT_ROW])) {
            CURRENT_COL++;
          }
          break;

        case KEY_BACKSPACE: 
          if(CURRENT_COL > 0) {
            CURRENT_COL -= 1;

            memmove(&DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL],
                    &DISPLAY_BUFFER[CURRENT_ROW][CURRENT_COL + 1],
                    MAX_NUMBER_OF_COLS - CURRENT_COL - 1);

            DISPLAY_BUFFER[CURRENT_ROW][MAX_NUMBER_OF_COLS - 1] = '\0';
          }
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
            shortcut_goto_first_line(current_char.ch);
            shortcut_goto_last_line(current_char.ch);
            shortcut_save_file(current_char.ch);
            shortcut_delete_backwards(current_char.ch);
            break;

        default: break;
    }


    system("clear");
    printf("%s", resize_string(join_display_buffer()));
   
  }

  return;
}

int main(int argc, char* argv[]) {

    // Start by checking, if filename is provided, or buffer
    // is to be created from scratch. If filename, is provided 
    // construct a buffer using its contents, else zero out
    // memory of DISPLAY_BUFFER
    if (argc > 1) {
        char* open_file = argv[1];
        FILE* fd_open_file = fopen(open_file, "a+");

        if (fd_open_file == NULL) {
            fprintf(stderr, "could, not open file: %s\n", open_file);
            INIT_FILE = false; 
        } else {
            char* temp_line = malloc(MAX_NUMBER_OF_COLS);
            if (!temp_line) {
                perror("malloc failed");
                exit(1);
            }

            strncpy(INIT_ARG_FNAME, argv[1], strlen(argv[1]));

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
                }

                i++;
            }

            NUMBER_OF_ROWS = i; 
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
  signal(SIGINT, (void*)&handler_SIGINT);

  // set terminal to raw mode
  set_terminal_raw_mode(true);

  // clear the screen and print the initial empty DISPLAY_BUFFER
  // or the file-content initialized DISPLAY_BUFFER: all the same, to me
  system("clear");
  printf("%s", resize_string(join_display_buffer()));

  // get terminal sizes
  get_terminal_size();

  // create two worker threads, one to check for input at
  // the terminal, and the other to manipulate the 
  // dipslay buffer and show it
  pthread_create(&get_input, NULL, (void*)&input, NULL);
  pthread_create(&display_buffer, NULL, (void*)&buffer_display, NULL);

  pthread_join(get_input, NULL);
  pthread_join(display_buffer, NULL);

  // set terminal to normal mode
  set_terminal_raw_mode(false);
}
