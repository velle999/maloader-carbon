// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// A sampling profiler for the game, for machines with no profiler
// installed: HLE_PROFILE=<file>. See profile.c.

#ifndef HLE_PROFILE_H_
#define HLE_PROFILE_H_

// Starts sampling, once, when HLE_PROFILE names a file. |game_code| is an
// address in the game's code; the mapping holding it is the game's.
void hle_profile_start(void* game_code);

// Lets the profiler sample the calling thread, which may block SIGPROF, as
// SDL's threads do. Cheap to call often.
void hle_profile_thread(void);

// Writes the profile now rather than at exit, for a crash, which skips the
// exit. Writes nothing twice.
void hle_profile_write(void);

// Ends a frame of the game's thread for HLE_LONG_FRAMES: |wall_ms| since the
// last swap, |swap_ms| of them in this one.
void hle_profile_frame(unsigned frame, double wall_ms, double swap_ms);

#endif  // HLE_PROFILE_H_
