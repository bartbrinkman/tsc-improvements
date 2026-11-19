#pragma once

// Shows which tscfix features and experiments are active in the game window's title,
// so whoever watches the game can tell at a glance what is running. The title is used
// instead of a drawn overlay so that the status itself cannot affect rendering.
namespace tscfix::status {

// Sets a named field ("render", "ab", ...); empty text removes it. Game thread.
void Set(const char* field, const char* text);

}  // namespace tscfix::status
