#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **sources;
    size_t source_count;
    double volume;
    int repeat_mode;
    bool shuffle;
    char *selected_path;
} PersistedState;

void state_init(PersistedState *state);
void state_free(PersistedState *state);
bool state_load(PersistedState *state, char *error, size_t error_size);
bool state_save(const PersistedState *state, char *error, size_t error_size);
bool state_replace_sources(PersistedState *state, char *const *paths,
                           size_t path_count);
bool state_add_source(PersistedState *state, const char *path);
bool state_set_selected(PersistedState *state, const char *path);

#endif
