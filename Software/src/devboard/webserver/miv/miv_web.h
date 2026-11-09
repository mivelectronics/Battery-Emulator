#ifndef MIV_WEB_H
#define MIV_WEB_H

/*
 * MIV pages and JSON API, kept apart from the upstream web pages so that merges with upstream do not touch
 * them. One call from init_webserver(). Reads the datalayer, so it shows the same with the core on the S32K
 * (be_remote_active) or with the local core.
 *
 *   /miv/                  one page, refreshes in place every second
 *   /miv/api/status        flat JSON, see miv_web.cpp
 *   /miv/api/cells?b=N     cell voltages of pack N (1..3)
 *   /miv/api/settings      GET: the settings the emulator runs with; POST (form fields): saved like the
 *                          settings page does, answers 202 with the settings revision to wait for
 *
 * The global middleware of the server (web password or local network only) covers these routes too.
 */

class AsyncWebServer;

void init_miv_web(AsyncWebServer& server);

#endif
