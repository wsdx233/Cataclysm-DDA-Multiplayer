#pragma once
#ifndef CATA_SRC_MULTIPLAYER_CLIENT_UI_H
#define CATA_SRC_MULTIPLAYER_CLIENT_UI_H

#include <string>

#include "cursesdef.h"
#include "multiplayer_client.h"

/** Runs the minimal graphical/curses semantic-scene multiplayer client loop. */
int run_multiplayer_client_ui( multiplayer_client_settings settings, std::string &error );

/** SDL renderer hook for the dedicated remote-scene window. */
bool multiplayer_client_is_remote_scene_window( const catacurses::window &window );
const multiplayer_scene_snapshot *multiplayer_client_scene_for_render();

#endif // CATA_SRC_MULTIPLAYER_CLIENT_UI_H
