#pragma once
namespace mkw {
// A course object's model clock: the CHR0 clip retail's object code plays on its model and that
// clip's frame (a looping clip's frame runs on past its length). src/race_audio_frame.cpp keeps
// it every simulation step for the object's BRASD sounds; the presenter draws the same clock.
// `object` is the Kinoko object, or a cannon's GOBJ entry (MapdataGeoObj). False when no clock
// is kept for it (before the race's first step, or an object without one).
struct ObjectAnimClock { const char* clip = nullptr; float frame = 0.0f; };
bool objectAnimClock(const void* object, ObjectAnimClock& out);
}
