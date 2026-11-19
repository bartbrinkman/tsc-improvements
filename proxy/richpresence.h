#pragma once
#include <windows.h>

// Steam rich presence updates made off the game thread (see richpresence.cpp).
namespace tscfix::richpresence {

// Starts a thread that hooks ISteamFriends::SetRichPresence once the Steam API is up.
void Start();

}  // namespace tscfix::richpresence
