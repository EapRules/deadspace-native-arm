#pragma once

/*
 * The one way the game is allowed to end itself.
 *
 * On Android the engine leaves by calling MainActivity.finish(): the framework
 * tears the activity down and the process follows. Here there is no framework,
 * so a finish() that does nothing leaves the engine in a state it cannot come
 * back from - it has already released its world - while the loader keeps
 * calling NativeOnDrawFrame over a torn-down scene. From outside that is a
 * freeze, and the only way out is PortMaster's kill: reported as GitHub issue
 * #1, where the log shows the draw counter frozen at 9944 for 128 frames after
 * "Class MainActivity does not have method finish()V."
 *
 * The request is recorded here and consumed by the frame loop, never acted on
 * where it is raised. The engine calls finish() from inside its own call
 * stack, and on other builds from its own threads; unwinding the process from
 * under either is a crash on the way out rather than an exit.
 */

/* Ask the frame loop to stop. Safe from any thread, and idempotent: the
 * engine can call finish() more than once and only the first is announced. */
void android_app_request_exit(const char *reason);

/* True once android_app_request_exit() has been called. */
bool android_app_exit_requested(void);
