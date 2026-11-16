#pragma once
struct NxWindow;
typedef struct NxWindow NxWindow;
NxWindow *nx_front_window(void);
void nx_front_set_window(NxWindow *w);
