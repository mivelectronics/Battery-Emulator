#ifndef MIV_WEB_H
#define MIV_WEB_H

/*
 * MIV pages and JSON API, kept apart from the upstream web pages so that merges with upstream do not touch
 * them. One call from init_webserver(). Reads the datalayer, so it shows the same with the core on the S32K
 * (be_remote_active) or with the local core.
 *
 *   /miv/...               one page with five views: main (/miv/), settings, cells, events, update. The
 *                          script picks the view from the path; live views refresh every second
 *   /miv/api/status        flat JSON, see miv_web.cpp
 *   /miv/api/cells?b=N     cell voltages of pack N (1..3)
 *   /miv/api/settings      GET: the settings the emulator runs with; POST (form fields): saved like the
 *                          settings page does, answers 202 with the settings revision to wait for
 *   /miv/api/events        the upstream event log, newest first; POST /miv/api/events/clear clears it
 *   /miv/api/s32k/ota/...  update of the S32K's application through this ESP32: POST begin?size=N (body: the
 *                          first 32 bytes of zephyr.signed.bin), chunk?offset=N (4 KB pieces, 503 = send it
 *                          again), finish, abort; GET state. The update view drives them; s32k_ota.h
 *
 * The settings view edits the upstream form itself (fetched from /settings, posted to /saveSettings) and
 * the update view drives the ElegantOTA routes (/ota/start, /ota/upload): nothing of upstream is redone.
 * The global middleware of the server (web password or local network only) covers these routes too.
 */

class AsyncWebServer;

void init_miv_web(AsyncWebServer& server);

#endif
