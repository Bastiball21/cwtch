#ifndef INPUT_H
#define INPUT_H

// Longest command the engine accepts. A `position ... moves ...` line for a
// very long game has to fit here.
#define INPUT_BUF_SIZE 65536

void input_init(void);
int input_get(char *buf, int size);

// Whether a reader thread is running, i.e. someone can still send `stop`.
int input_is_running(void);

// Bracket a search started from a `go` command. input_go_begin() must run after
// the time control is initialised: it applies a `stop` that already arrived.
void input_go_begin(void);
void input_go_end(void);

#endif
