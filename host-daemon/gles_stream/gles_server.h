/* language: C11, file: host-daemon/gles_stream/gles_server.h
 * Server-side GL command executor (runs on a separate host-daemon thread).
 * Receives commands from /tmp/x11droid_gl.sock and executes them in the GLX context.
 */
#ifndef GLES_SERVER_H
#define GLES_SERVER_H

#include <stdbool.h>

/* Initialize the GL streaming server.
 * Must be called AFTER glx_create() succeeds (the GLX context is ready).
 * Starts a background listener thread. */
bool gles_server_init(void);

/* Stop the server and join the thread. */
void gles_server_stop(void);

/* Check whether the server is running. */
bool gles_server_running(void);

#endif /* GLES_SERVER_H */