// Copies the race state the audio thread needs out of Kinoko into a
// RaceAudioFrame, once per simulation tick on the main thread. Only the
// audio-only sound cues are taken (cleared); the simulation state is read.
#include <mkw/object_anim_clock.hpp>
#include <mkw/object_sound_assets.hpp>
#include <mkw/race_audio.hpp>
#include <mkw/race_item_internal.hpp>
#include <mkw/race_items.hpp>
#include <game/kart/CollisionGroup.hh>
#include <game/kart/KartDynamics.hh>
#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartParam.hh>
#include <game/kart/KartState.hh>
#include <game/kart/KartSuspensionPhysics.hh>
#include <game/kart/Status.hh>
#include <game/kart/KartAction.hh>
#include <game/field/ObjectDirector.hh>
#include <game/field/ObjectDrivableDirector.hh>
#include <game/field/obj/ObjectBase.hh>
#include <game/field/obj/ObjectBird.hh>
#include <game/field/obj/ObjectCow.hh>
#include <game/field/obj/ObjectCrab.hh>
#include <game/field/obj/ObjectHanachan.hh>
#include <game/field/obj/ObjectId.hh>
#include <game/field/obj/ObjectKoopaFigure.hh>
#include <game/field/obj/ObjectKuribo.hh>
#include <game/field/obj/ObjectPakkunF.hh>
#include <game/field/obj/ObjectPoihana.hh>
#include <game/field/obj/ObjectStarRing.hh>
#include <game/system/CourseMap.hh>
#include <game/system/RaceConfig.hh>
#include <game/system/RaceManager.hh>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace mkw {
namespace K = Kinoko;
namespace EGG = Kinoko::EGG;
// Kinoko numbers its status flags exactly as retail's KartStatus bitfields;
// the flag set is a plain array of those 32-bit words.
static_assert(sizeof(K::Kart::Status) == 5 * sizeof(std::uint32_t));
using Cue = K::Kart::KartMove::SoundCue;
static_assert(std::uint32_t(Cue::Boost) == cue::Boost && std::uint32_t(Cue::BoostRamp) == cue::BoostRamp &&
              std::uint32_t(Cue::JumpPad) == cue::JumpPad && std::uint32_t(Cue::JumpPadMushroom) == cue::JumpPadMushroom &&
              std::uint32_t(Cue::Hop) == cue::Hop && std::uint32_t(Cue::Cannon) == cue::Cannon &&
              std::uint32_t(Cue::Landing) == cue::Landing && std::uint32_t(Cue::Horn) == cue::Horn &&
              std::uint32_t(Cue::Bumped) == cue::Bumped && std::uint32_t(Cue::BushWall) == cue::BushWall &&
              std::uint32_t(Cue::BoostVoice) == cue::BoostVoice && std::uint32_t(Cue::Trick) == cue::Trick &&
              std::uint32_t(Cue::Respawn) == cue::Respawn && std::uint32_t(Cue::StartFail) == cue::StartFail &&
              std::uint32_t(Cue::Wheelie) == cue::Wheelie && std::uint32_t(Cue::PowerHit) == cue::PowerHit &&
              std::uint32_t(Cue::StarHit) == cue::StarHit && std::uint32_t(Cue::Bump) == cue::Bump &&
              std::uint32_t(Cue::Fall) == cue::Fall && std::uint32_t(Cue::MegaUse) == cue::MegaUse &&
              std::uint32_t(Cue::MegaEnd) == cue::MegaEnd && std::uint32_t(Cue::Grow) == cue::Grow &&
              std::uint32_t(Cue::Press) == cue::Press && std::uint32_t(Cue::PressEnd) == cue::PressEnd &&
              std::uint32_t(Cue::Spin) == cue::Spin && std::uint32_t(Cue::BlowSide) == cue::BlowSide &&
              std::uint32_t(Cue::SoundTrigger) == cue::SoundTrigger);
static_assert(std::uint32_t(K::Kart::eStatus::ZipperTrick) == 47);
// The presentation cues must not collide with Kinoko's.
static_assert(cue::JumpAction > std::uint32_t(Cue::SoundTrigger) && cue::ZipperTrick > cue::JumpAction && cue::Vanish > cue::ZipperTrick);
static_assert(std::uint32_t(K::Kart::eStatus::Vanishing) == 82);

namespace {
// Course objects' BRASD animation sounds. Retail loads brasd/<resource>/<clip>.brasd beside each
// CHR0 clip (fn_1_455C8, PAL 0x8055597C). Every frame the model's anim manager (0x805578D8)
// hands the frame of its slot-0 clip to the object's RaceAnimActor (ObjFlow sound mode 2) or
// ObjectActorAnim (mode 3): UpdateFrame (0x80700754) skips frames at or past the BRASD's frame
// count, else NW4R's AnimSoundImpl (DOL 0x80086D70) fires the events of every integer frame
// the clip passed. A clip change hands over its BRASD (actor vt+0x5c, 0x80700728): another file
// resets the cursor, the same one keeps it. Here each sounding object has such a cursor, fed
// every step with the clip and frame retail's object code gives its model (stepAnims, from
// Kinoko's state; a clip started in a step shows its frame advanced once by the model update),
// and the events reach the object's actor (race_audio.cpp objectAnim).
int soundClip(const char* resource, const char* clip) {
 for (std::size_t i = 0; i < std::size(ObjectSoundClips); ++i)
  if (!std::strcmp(ObjectSoundClips[i].resource, resource) && !std::strcmp(ObjectSoundClips[i].name, clip)) return int(i);
 return -1;
}
struct AnimCursor {
 int clip = -1;       // the BRASD in use (ObjectSoundClips), -1: none (SetFile(0))
 float frame = 0.0f;  // AnimSoundImpl +0xc
 bool fresh = false;  // +0x19 / +0x1a, set by a new file
 // RaceAnimActor::SetFile (0x80700728) and UpdateFrame (0x80700754 -> 0x80086D70, forward
 // 0x80086EA0): the first update after a reset includes frame 0; then every integer frame in
 // (floor(previous), floor(frame)], wrapping at the BRASD's frame count. Each cooked event is
 // a trigger played on every loop (trigger 0x80087340).
 template<class Emit> void update(int c, float f, Emit&& emit) {
  if (c != clip) { clip = c; frame = 0.0f; fresh = c >= 0; }
  if (clip < 0 || !(f >= 0.0f)) return;
  const ObjectSoundClip& k = ObjectSoundClips[clip];
  if (!(f < float(k.frames))) return;
  int g = int(std::floor(frame));
  const int target = int(std::floor(f));
  if (fresh && frame == float(g)) --g;
  fresh = false;
  frame = f;
  while (g != target) {
   if (++g == int(k.frames)) g = 0;
   for (unsigned i = 0; i < k.count; ++i)
    if (int(ObjectSoundEvents[k.first + i].frame) == g) emit(std::uint16_t(k.first + i));
  }
 }
};
// A course object's model clock (stepAnims): the CHR0 clip it plays and that clip's frame (a
// looping clip's frame runs on past its length; the cursor and the presenter wrap it).
struct ObjectAnim {
 enum Kind : std::uint8_t { None, Cannon, KoopaFigure, Crab, PuchiPakkun, PakkunF, WigglerHead, WigglerBody, Kuribo, Poihana,
                            PenguinM, CowLeader, CowFollower };
 Kind kind = None;
 const char* resource = "";                            // the model's BRRES, its BRASD folder
 const char* clip = nullptr;                           // the clip the model plays
 float frame = 0.0f, rate = 0.0f, speed = 0.0f;
 const K::Field::ObjectHanachan* wiggler = nullptr;   // a Wiggler part's owner
 std::uint8_t part = 0;                                // the part (0 head, 1-6 the body segments)
 bool flag = false;
 const char* soundOf = nullptr; int sound = -1;        // `clip`'s BRASD (ObjectSoundClips)
 AnimCursor cursor;
 // fn_1_472D0 (0x80557684): plays `name` from frame 0 unless it already plays.
 void play(const char* name) { if (!clip || std::strcmp(clip, name)) { clip = name; frame = 0.0f; } }
};
// Kinoko objects whose ObjFlow entry gives them a sound actor; a slot is the
// object's index here for the rest of the race. Course objects Kinoko does not
// build but retail gives a sound actor are kept by their GOBJ entry: the
// ObjectActor idle loops whose objects have no collision (flags, the Peach
// Beach fountain), the KMP sound_* emitters (positioned on their route) and the
// first cannon (a RaceAnimActor, sound mode 2).
struct ObjectRegistry {
 std::size_t scanned = 0, scannedDrivable = 0;
 bool courseScanned = false;
 std::uint8_t pocha = 0;
 std::uint8_t count = 0;
 std::array<const K::Field::ObjectBase*, 128> object{};
 std::array<const K::System::MapdataGeoObj*, 128> geo{};
 std::array<std::uint16_t, 128> id{};
 std::array<std::uint8_t, 128> mode{}, index{};
 std::array<ObjectAnim, 128> anim{};
 std::uint8_t animSetup = 0;     // slots whose clock is set up
 std::uint32_t animTick = 0;     // steps since the race's objects were built
 bool cannonStart = false;       // a racer's CannonStart after the last step (the next calc's)
 // In place: the arrays are too large for a temporary on the main thread's stack; slots past
 // `count` are rebuilt as they are added.
 void reset() { scanned = scannedDrivable = 0; courseScanned = false; pocha = count = animSetup = 0; animTick = 0; cannonStart = false; }
 int find(const K::Field::ObjectBase* o) const { for (int i = 0; i < count; ++i) if (object[i] == o) return i; return -1; }
 int add(const K::Field::ObjectBase* o, const K::System::MapdataGeoObj* g, std::uint16_t objectId, int m) {
  if (count >= object.size()) return -1;
  std::uint8_t same = 0;   // the object's index among those of its kind (bats alternate by it)
  for (int i = 0; i < count; ++i) same += id[i] == objectId;
  object[count] = o; geo[count] = g; id[count] = objectId; mode[count] = std::uint8_t(m); index[count] = same;
  return count++;
 }
};
ObjectRegistry registry;
// The object's sound actor slot (Object +0x18): its registry slot, registered
// now if its ObjFlow entry has an actor; -1 (0xff) without one.
std::uint8_t objectSlot(const K::Field::ObjectBase* o) {
 int slot = registry.find(o);
 if (slot < 0) {
  const auto* dir = K::Field::ObjectDirector::Instance();
  const int m = dir ? dir->flowTable().soundMode(o->id()) : 0;
  if (m == 1 || m == 3) slot = registry.add(o, nullptr, std::uint16_t(o->id()), m);
 }
 return slot < 0 ? 0xff : std::uint8_t(slot);
}
// ObjectActor::Update idle loops (PAL fn_1_1FDA94) on objects Kinoko leaves out.
bool idleLoopObject(std::uint16_t id) {
 switch (id) {
 case 0x141: case 0x152: case 0x153: case 0x154: case 0x155: case 0x156: case 0x180: case 0x182: return true;
 default: return false;
 }
}
// The sound_* emitters' table (PAL 0x808C8CB8): GOBJ ids 5-0x10 and 0x15.
bool emitterObject(std::uint16_t id) { return (id >= 5 && id <= 0x10 && id != 7) || id == 0x15; }
float dot(const EGG::Vector3f& a, const EGG::Vector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float dist(const EGG::Vector3f& a, const EGG::Vector3f& b) {
 const EGG::Vector3f d{a.x - b.x, a.y - b.y, a.z - b.z};
 return std::sqrt(dot(d, d));
}
// fn_1_1A43A8 (PAL 0x806B475C): the point of a POTI route nearest `p`, as
// retail finds it (a segment pair's XZ side test picks the segment; outside
// the pair's wedge the half-way split overwrites the best point unconditionally).
EGG::Vector3f nearestOnRoute(const K::System::MapdataPointInfo& route, const EGG::Vector3f& p) {
 const auto& pts = route.points();
 const std::size_t n = pts.size();
 auto P = [&](std::size_t i) -> const EGG::Vector3f& { return pts[i].pos; };
 auto lerp = [](const EGG::Vector3f& a, const EGG::Vector3f& b, float t) { return EGG::Vector3f{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
 auto param = [&](const EGG::Vector3f& a, const EGG::Vector3f& b) {
  const EGG::Vector3f ab{b.x - a.x, b.y - a.y, b.z - a.z}, ap{p.x - a.x, p.y - a.y, p.z - a.z};
  return dot(ab, ap) / dot(ab, ab);
 };
 if (n == 2) {
  const float t = param(P(0), P(1));
  if (t < 0.0f || t >= 1.0f) return dist(p, P(1)) < dist(p, P(0)) ? P(1) : P(0);
  return lerp(P(0), P(1), t);
 }
 float best = -1.0f, bt = 0.0f; int seg = -1, off = -1;
 for (std::size_t i = 0; i + 2 < n; ++i) {
  const EGG::Vector3f &A = P(i), &B = P(i + 1), &C = P(i + 2);
  const float cross = (C.x - A.x) * (B.z - A.z) - (B.x - A.x) * (C.z - A.z);
  const float s1 = (B.x - A.x) * (p.z - A.z) - (p.x - A.x) * (B.z - A.z);
  const float s2 = (C.x - B.x) * (p.z - B.z) - (p.x - B.x) * (C.z - B.z);
  const bool inside = cross > 0.0f ? (s1 > 0.0f || s2 > 0.0f) : (s1 <= 0.0f || s2 <= 0.0f);
  if (inside) {
   const float t1 = param(A, B), t2 = param(B, C);
   if (t1 < 0.0f || t1 >= 1.0f) {
    if (t2 < 0.0f || t2 >= 1.0f) {
     const float t3 = param(A, C);
     if (t3 >= 0.0f && t3 < 1.0f) {
      const float d = dist(p, B);
      if (best == -1.0f || d < best) { seg = int(i); off = 1; bt = 0.0f; best = d; }
     }
    } else {
     const float d = dist(p, lerp(B, C, t2));
     if (best == -1.0f || d < best) { seg = int(i); off = 1; bt = t2; best = d; }
    }
   } else {
    const float d = dist(p, lerp(A, B, t1));
    if (best == -1.0f || d < best) { seg = int(i); off = 0; bt = t1; best = d; }
   }
  } else {
   const float t = param(A, C);
   if (t < 0.0f || t >= 0.5f) {
    if (t >= 0.5f && t < 1.0f) { bt = 2.0f * (t - 0.5f); best = dist(p, lerp(B, C, bt)); seg = int(i); off = 1; }
   } else { bt = 2.0f * t; best = dist(p, lerp(A, B, bt)); seg = int(i); off = 0; }
  }
 }
 if (best < 0.0f) return dist(p, P(n - 1)) < dist(p, P(0)) ? P(n - 1) : P(0);
 return lerp(P(std::size_t(seg + off)), P(std::size_t(seg + off + 1)), bt);
}
// Effects::Player's GCN Peach Beach sea (PAL 0x8069C7F0 -> SetRoadState 0x8070B8DC),
// with the inputs the port's water effects use (main.cpp kartFx).
std::uint8_t seaState(const K::Kart::KartObject& kart) {
 const auto* dir = K::Field::ObjectDirector::Instance();
 if (!dir || !dir->psea() || !kart.status().onBit(K::Kart::eStatus::Diving)) return 0;
 const float sea = -dir->distAboveRisingWater(0);
 const float body = kart.pos().y + kart.pose()[1, 1] * 100.0f * kart.param()->stats().kclRot[30];
 const unsigned wheels = kart.tireCount(), rearL = kart.isBike() ? 1 : wheels - 2, rearR = wheels - 1;
 const float rear = std::min(kart.wheelEdgePos(std::uint16_t(rearL)).y, kart.wheelEdgePos(std::uint16_t(rearR)).y);
 bool deep = kart.pos().y - kart.bsp().initialYPos + 50.0f < sea;
 for (unsigned w : {rearL, rearR}) {
  const auto& c = kart.collisionData(std::uint16_t(w));
  deep |= c.bFloor && (std::uint32_t(c.closestFloorFlags) & (1u << 11));
 }
 if (body >= sea && rear < sea) return deep ? 2 : 1;
 return 3;
}
// The clock of registry slot `s`, once, when the slot is first seen.
void setupAnim(int s) {
 ObjectAnim& a = registry.anim[s];
 a = ObjectAnim{};
 const K::Field::ObjectBase* o = registry.object[s];
 if (!o) {
  if (registry.mode[s] == 2) {   // the first cannon (fillRaceAudioObjects' course scan)
   a.kind = ObjectAnim::Cannon;
   a.resource = a.clip = registry.id[s] == 0x25f ? "DonkyCannon_wii" : registry.id[s] == 0x259 ? "DonkyCannonGC" : "tree_cannon";
  }
  return;
 }
 using Id = K::Field::ObjectId;
 switch (o->id()) {
 case Id::KoopaFigure: a.kind = ObjectAnim::KoopaFigure; a.resource = "koopaFigure"; return;
 case Id::Crab: a.kind = ObjectAnim::Crab; a.resource = "crab"; return;
 case Id::PuchiPakkun: a.kind = ObjectAnim::PuchiPakkun; a.resource = "puchi_pakkun"; return;
 case Id::PakkunF: a.kind = ObjectAnim::PakkunF; a.resource = "pakkun_f"; return;
 case Id::Kuribo: a.kind = ObjectAnim::Kuribo; a.resource = "kuribo"; return;
 case Id::Poihana: a.kind = ObjectAnim::Poihana; a.resource = "poihana"; return;
 case Id::PenguinM: a.kind = ObjectAnim::PenguinM; a.resource = "penguin_m"; return;
 default: break;
 }
 const auto& objects = K::Field::ObjectDirector::Instance()->objects();
 if (o->id() == Id::Cow && o->mapObj()) {
  // The herd (ctor 0x806BEB54) builds its leader, then its followers, then itself: by
  // placement, the first object is the leader and the last the herd.
  std::size_t first = objects.size(), last = 0, at = 0;
  for (std::size_t k = 0; k < objects.size(); ++k)
   if (objects[k]->mapObj() == o->mapObj()) { first = std::min(first, k); last = k; if (objects[k] == o) at = k; }
  if (at != last) { a.kind = at == first ? ObjectAnim::CowLeader : ObjectAnim::CowFollower; a.resource = "cow"; }
  return;
 }
 if (o->id() != Id::Hanachan && std::uint16_t(o->id()) != 0xf2) return;
 // A Wiggler's parts: the head (BossHanachanHead) and the six body segments built with the
 // placement, whose last object is the ObjectHanachan owning them (ctor 0x806C8A5C).
 for (std::size_t k = 0; k < objects.size(); ++k) {
  const auto* w = objects[k];
  if (w->id() != Id::Hanachan || !w->mapObj()) continue;
  bool last = true;
  for (std::size_t j = k + 1; j < objects.size() && last; ++j) last = objects[j]->mapObj() != w->mapObj();
  if (!last) continue;
  const auto* owner = static_cast<const K::Field::ObjectHanachan*>(w);
  for (std::size_t i = 0; i < owner->partCount(); ++i)
   if (owner->part(i) == o) {
    a.kind = i ? ObjectAnim::WigglerBody : ObjectAnim::WigglerHead;
    a.resource = "hanachan"; a.wiggler = owner; a.part = std::uint8_t(i);
    return;
   }
 }
}
// An event reaches the object's actor (race_audio.cpp objectAnim).
void emitAnim(RaceAudioFrame& f, int s, const ObjectAnim& a, const K::Field::ObjectBase* o, std::uint16_t e) {
 const ObjectSoundEvent& ev = ObjectSoundEvents[e];
 std::uint16_t sound = ev.sound;
 // ObjectActorAnim::SetupSound (0x80703CF0): on GCN Peach Beach a Cataquack's steps
 // SE_CC_POIHANA_FN1/FN2 become FNS1/FNS2 (+2) at least 270 above the tide, else FNW1/FNW2
 // (+4) (0x8082B3D4, 0x808A16D0).
 if (a.kind == ObjectAnim::Poihana && (sound == 0x259 || sound == 0x25a)) {
  const auto* dir = K::Field::ObjectDirector::Instance();
  const auto* config = K::System::RaceConfig::Instance();
  if (dir && dir->psea() && config && config->raceScenario().course == K::Course::GCN_Peach_Beach)
   sound = std::uint16_t(sound + (dir->distAboveRisingWater(o->pos().y) < 270.0f ? 4 : 2));
 }
 if (f.objectSounds < f.objectSound.size())
  f.objectSound[f.objectSounds++] = {std::uint16_t(s), sound, std::uint8_t(RaceAudioObjectSound::Animation), ev.volume, ev.pitch, e};
}
// One step of every clocked object's model: the clip and frame retail's object code gives it
// (its calc, then the model update advancing the frame by the clip's rate), into its BRASD cursor.
void stepAnims(RaceAudioFrame& f) {
 for (; registry.animSetup < registry.count; ++registry.animSetup) setupAnim(registry.animSetup);
 const std::uint32_t tick = ++registry.animTick;
 const auto* race = K::System::RaceManager::Instance();
 const std::uint32_t timer = race ? race->timer() : 0;
 for (int s = 0; s < registry.count; ++s) {
  ObjectAnim& a = registry.anim[s];
  const K::Field::ObjectBase* o = registry.object[s];
  switch (a.kind) {
  case ObjectAnim::None: continue;
  case ObjectAnim::Cannon:
   // Calc 0x806BFED0: the shot clip rests on frame 0 at rate 0 (init 0x806BFDDC); a racer's
   // CannonStart (the karts' calc follows the objects': the last step's) plays it from 0 at
   // rate 1 while it rests; at frame count - 1 (+0xE4) it rests on 0 again.
   if (a.frame == 0.0f && registry.cannonStart) a.rate = 1.0f;
   if (a.sound >= 0 && a.frame >= float(ObjectSoundClips[a.sound].chrFrames) - 1.0f) { a.frame = 0.0f; a.rate = 0.0f; }
   a.frame += a.rate;
   break;
  case ObjectAnim::KoopaFigure: {
   // Kinoko's statue (calc 0x8077213C): vomit from 0, then wait from 0 (looping).
   const auto& k = *static_cast<const K::Field::ObjectKoopaFigure*>(o);
   a.clip = k.vomiting() ? "vomit" : "wait"; a.frame = float(k.animFrame() + 1);
   break;
  }
  case ObjectAnim::Crab: {
   // Calc 0x80883B98: walking starts walk_l / walk_r (the rail direction against setting 1)
   // from 0 at setting 0 / 9 (0x808DCBE0 / 0x808DCBDC); standing still plays wait from 0 at the
   // rate 0x809C49AC (never written: 0). Kinoko's crabs have no hit or sand states.
   const auto& k = *static_cast<const K::Field::ObjectCrab*>(o);
   if (k.state() == 1) { a.play("wait"); a.rate = 0.0f; }
   else if (!a.clip || !std::strcmp(a.clip, "wait")) {
    const bool forward = !k.railInterpolator() || k.railInterpolator()->isMovementDirectionForward();
    a.play(forward == k.backwards() ? "walk_r" : "walk_l");
    a.rate = float(std::int16_t(k.mapObj()->setting(0))) / 9.0f;
   }
   a.frame += a.rate;
   break;
  }
  case ObjectAnim::PuchiPakkun:
   // Init 0x80778830: puchi_pakkun from 0 at rate 1, never changed.
   a.clip = "puchi_pakkun"; a.frame = float(tick);
   break;
  case ObjectAnim::PakkunF: {
   // Init 0x807743E4 plays wait; enterAttack 0x80774CB0 plays attack from 0; calc 0x80774754
   // plays wait from 0 once attack (played once) reached its frame count.
   const auto& k = *static_cast<const K::Field::ObjectPakkunF*>(o);
   const bool attack = a.clip && !std::strcmp(a.clip, "attack");
   if (!a.clip || (attack && a.sound >= 0 && a.frame >= float(ObjectSoundClips[a.sound].chrFrames))) a.play("wait");
   if (k.attacking() && !a.flag) a.play("attack");
   a.flag = k.attacking();
   a.frame += 1.0f;
   if (!std::strcmp(a.clip, "attack") && a.sound >= 0) a.frame = std::min(a.frame, float(ObjectSoundClips[a.sound].chrFrames));
   break;
  }
  case ObjectAnim::WigglerHead: {
   // Walking: head_walk (initWalk 0x806C9BC0). Waiting (initWait 0x806C9EC8, calcWait
   // 0x806C9F98): head_wait from 0, head_wait2 from the wait's frame 60 (vt+0x118, 0x806CB588)
   // and head_wait again after its 180 frames (+0x104).
   const unsigned st = a.wiggler->stateId(), sf = a.wiggler->stateFrame();
   if (st == 0) { a.clip = "head_walk"; a.frame = float(sf + 1); }
   else if (sf < 60) { a.clip = "head_wait"; a.frame = float(sf + 1); }
   else if (sf < 240) { a.clip = "head_wait2"; a.frame = float(sf - 59); }
   else { a.clip = "head_wait"; a.frame = float(sf - 239); }
   break;
  }
  case ObjectAnim::WigglerBody: {
   // Body segment i walks at (80 + 6i) / 80 (initWalk 0x806C9BC0, 0x8089FAA8), from 0 when it
   // was waiting; in a wait it plays wait from 0 at rate 1 at the wait's frame 4i (calcWait
   // 0x806C9F98). Init (0x806CA72C) starts walk from 0.
   const unsigned i = a.part - 1u, st = a.wiggler->stateId(), sf = a.wiggler->stateFrame();
   if (!a.clip || (st == 0 && sf == 0)) { a.play("walk"); a.rate = float(80 + 6 * i) / 80.0f; }
   if (st == 1 && sf == 4 * i) { a.play("wait"); a.rate = 1.0f; }
   a.frame += a.rate;
   break;
  }
  case ObjectAnim::Kuribo: {
   // Kinoko's walk timer is the walk clip's frame; the clip holds while the Goomba is hit,
   // blinking or popping up (0x806DB5B0), and goes on from there.
   const auto& k = *static_cast<const K::Field::ObjectKuribo*>(o);
   a.clip = "walk_l";
   if (!k.walking()) { a.flag = true; continue; }
   if (a.flag) { a.flag = false; a.cursor.frame = k.animTimer(); }
   a.frame = k.animTimer();
   break;
  }
  case ObjectAnim::Poihana: {
   // Kinoko's Cataquack clock (walk loops; throw and overturn play once).
   const auto& k = *static_cast<const K::Field::ObjectPoihana*>(o);
   a.clip = k.animationName(); a.frame = k.animationFrame();
   break;
  }
  case ObjectAnim::PenguinM:
   // ObjectPenguin init 0x807756B0: walk from 0 at rate 1; the cave penguins never change it.
   a.clip = "walk"; a.frame = float(tick);
   break;
  case ObjectAnim::CowLeader: {
   // Calc 0x806BD388: eat until 60 frames before the herd's start (setting 2), then eat_ed;
   // after it wait (enterWait / calcWait 0x806BD6B0 / 0x806BD738), eat_st, eat from its frame
   // 40 and eat_ed past its eat frames (enterEat / calcEat 0x806BD7D8 / 0x806BD84C), walk at 0.3
   // (0x8089F3B0) x the rail speed (enterRoam / calcRoam 0x806BDA1C / 0x806BDA70); else rate 1.
   // Retail links no run clip (anim list 0x8089F360).
   const auto& k = *static_cast<const K::Field::ObjectCowLeader*>(o);
   const std::uint32_t start = k.mapObj()->setting(2);
   a.rate = 1.0f;
   if (timer < start - 60u) a.play("eat");
   else if (timer < start) a.play("eat_ed");
   else if (k.stateId() == 0) a.play("wait");
   else if (k.stateId() == 1) {
    const std::uint32_t sf = k.stateFrame();
    a.play(sf < 40 ? "eat_st" : sf <= std::uint16_t(k.eatFrames() + 40) ? "eat" : "eat_ed");
   } else { a.play("walk"); a.rate = 0.3f * k.railSpeed(); }
   a.frame += a.rate;
   break;
  }
  case ObjectAnim::CowFollower: {
   // Init 0x806BE060 and calc 0x806BE1A8: wait until the herd's start; then wait (state 0,
   // 0x806BE4E8 / 0x806BE580) or walk (free roam 0x806BE62C / 0x806BE794, following
   // 0x806BE930 / 0x806BE9CC) at 0.3 x its speed (+0xE4, as the calc found it: the last step's).
   const auto& k = *static_cast<const K::Field::ObjectCowFollower*>(o);
   if (timer < k.mapObj()->setting(2) || k.stateId() == 0) { a.play("wait"); a.rate = 1.0f; }
   else { a.play("walk"); a.rate = 0.3f * a.speed; }
   a.speed = k.xzSpeed();
   a.frame += a.rate;
   break;
  }
  }
  if (a.clip != a.soundOf) { a.soundOf = a.clip; a.sound = a.clip ? soundClip(a.resource, a.clip) : -1; }
  const float frame = a.sound >= 0 && ObjectSoundClips[a.sound].chrLoop ? std::fmod(a.frame, float(ObjectSoundClips[a.sound].chrFrames)) : a.frame;
  a.cursor.update(a.sound, frame, [&](std::uint16_t e) { emitAnim(f, s, a, o, e); });
 }
// What the next step's cannon calc reads of the karts.
 registry.cannonStart = false;
 if (const auto* karts = K::Kart::KartObjectManager::Instance()) {
  const unsigned n = std::min<unsigned>(K::System::RaceConfig::Instance()->raceScenario().playerCount, 12);
  for (unsigned i = 0; i < n; ++i)
   if (const auto* kart = karts->object(i)) registry.cannonStart |= kart->status().onBit(K::Kart::eStatus::CannonStart);
 }
}
}

bool objectAnimClock(const void* object, ObjectAnimClock& out) {
 for (int i = 0; i < registry.count; ++i) {
  const ObjectAnim& a = registry.anim[i];
  if (i >= registry.animSetup || a.kind == ObjectAnim::None || !a.clip) continue;
  if (registry.object[i] != object && (registry.object[i] || registry.geo[i] != object)) continue;
  out.clip = a.clip; out.frame = a.frame;
  return true;
 }
 return false;
}

void fillRaceAudioFrame(RaceAudioFrame& f, snd::Vec3f eye, snd::Vec3f target, snd::Vec3f up, bool preview) {
 f.eye = eye; f.target = target; f.up = up;
 // RaceMgr::UpdatePlayerMatrix (PAL fn_1_200DE4 0x80711198) scales eye -> target by
 // the camera mode RaceMgr::Init (0x80710A00) copies from RaceConfig +0xB74: 0.99
 // (Audio::Manager +0x8BC) in mode 0, 0.0 in 7-12, else 0.2 (0x808A1F00). Mode 5 is
 // only the Demo section (the course preview): RaceInfo_update (0x80533374) writes 0
 // before the race section's initRace, and Time Trial, Restart (0x8085A0F4 case 2)
 // and Watch Replay (GhostManagerPage_setupGhostReplay 0x805E1D5C) start in 0.
 f.listenerScale = preview ? 0.2f : 0.99f;
 const auto* manager = K::Kart::KartObjectManager::Instance();
 const auto& scenario = K::System::RaceConfig::Instance()->raceScenario();
 f.karts = std::uint8_t(std::min<unsigned>(scenario.playerCount, 12));
 for (unsigned i = 0; i < f.karts; ++i) {
  RaceAudioKart& k = f.kart[i];
  auto* kart = manager->object(i);
  const auto& player = scenario.players[i];
  k.present = kart != nullptr;
  if (!kart) continue;
  const auto p = kart->pos();
  k.position = {p.x, p.y, p.z};
  const auto& v = kart->dynamics()->velocity();
  k.velocity = {v.x, v.y, v.z};
  auto* move = kart->move();
  // EngineMgr::Init (PAL fn_1_1EAAAC 0x806FAE60) reads KartObjectProxy::getBaseSpeed
  // (0x805910B0, KartMove +0x14): the stat speed, never the AI speed (+0x1C) that
  // Kinoko's baseSpeed() returns for CPUs and for the player after the goal.
  k.speed = move->speed(); k.speedRatioCapped = move->speedRatioCapped(); k.baseSpeed = move->statBaseSpeed();
  k.startBoostCharge = kart->state()->startBoostCharge();
  // A half-pipe trick start (retail fn_1_64AAC, Kinoko KartHalfPipe::activateTrick
  // setting ZipperTrick): Kinoko cues no sound there, so take the rising edge.
  const bool zipperTrick = kart->status().onBit(K::Kart::eStatus::ZipperTrick);
  if (zipperTrick && !(k.status[47 / 32] & (1u << (47 % 32)))) k.cues |= cue::ZipperTrick;
  // fn_1_71034 (0x805813E8), the bit-82 step: on its first frame (KartMove +0x160
  // still 1.0) it plays SE_VCL_BTL_NO_BALLOON on the kart's KartActor, the frame
  // bit 82 is set. Kinoko sets it for a finished ghost 70 frames after its goal.
  const bool vanishing = kart->status().onBit(K::Kart::eStatus::Vanishing);
  if (vanishing && !(k.status[82 / 32] & (1u << (82 % 32)))) k.cues |= cue::Vanish;
  std::memcpy(k.status, &kart->status(), sizeof k.status);
  k.moveFlags = move->ssmtCharged() ? 4 : 0;
  k.driftState = std::uint8_t(move->driftState());
  k.canWheelie = move->canWheelie();
  k.wheels = std::uint8_t(std::min<unsigned>(kart->tireCount(), 4));
  for (unsigned w = 0; w < k.wheels; ++w) {
   const auto& c = kart->tirePhysics(std::uint16_t(w))->hitboxGroup()->collisionData();
   const std::uint32_t mask = std::uint32_t(c.closestFloorFlags);
   std::uint8_t type = 0;
   for (int b = 0; b < 32; ++b) if (mask & (1u << b)) { type = std::uint8_t(b); break; }
   k.floorType[w] = type; k.floorVariant[w] = std::uint16_t(c.closestFloorSettings);
  }
  // The body's wall contact as Effects::Player::UpdateFromKartCollision sees it.
  const auto& body = kart->collisionData();
  k.wallType = std::uint8_t(body.closestWallFlags); k.wallVariant = std::uint8_t(body.closestWallSettings);
  k.wallContact = (body.bWall || body.bWall3) && !body.bInvisibleWallOnly;
  const auto taken = move->takeSoundCues();
  k.cues |= taken.cues;
  k.bumpedBy |= taken.bumpedBy;
  if (taken.cues & cue::Bump) k.bump = taken.bump;
  if (taken.cues & cue::Fall) k.fallVoice = taken.fallVoice;
  if (taken.cues & cue::SoundTrigger) k.soundZone = taken.soundZone;
  // The object contact callbacks in order (ObjectDirector::checkKartObjectCollision).
  for (unsigned n = 0; n < taken.objectSoundCount && k.objectHits < 4; ++n) {
   const auto* o = static_cast<const K::Field::ObjectBase*>(taken.objectSounds[n].object);
   if (!o) continue;
   RaceAudioObjectHit& h = k.objectHit[k.objectHits++];
   h.id = std::uint16_t(o->id()); h.wall = taken.objectSounds[n].wall;
   h.variant = o->mapObj() ? o->mapObj()->setting(0) : 0;
   h.slot = objectSlot(o);
  }
  if (taken.fallBoundary != 0xff) k.fallBoundary = taken.fallBoundary;
  for (unsigned n = 0; n < taken.kartSoundCount && k.kartSounds < 4; ++n) k.kartSound[k.kartSounds++] = taken.kartSounds[n];
  k.seaState = seaState(*kart);
  k.boostRampType = kart->state()->boostRampType();
  k.airtime = std::uint16_t(std::min<std::uint32_t>(kart->state()->airtime(), 0xffff));
  k.slipCharging = move->slipstreamCharging(); k.slipBoost = move->slipstreamBoost();
  k.starTimer = std::uint16_t(std::max(0, int(move->starTimer())));
  k.actionCount = kart->action()->startCount(); k.action = std::int8_t(kart->action()->lastStarted());
  if (const auto* race = K::System::RaceManager::Instance()) {   // one RaceManager player per scenario racer
   const auto& p = race->player(i);
   k.finished = p.finished(); k.rank = std::uint8_t(p.finishOrder());
  }
  k.vehicle = std::uint8_t(player.vehicle); k.character = std::uint8_t(player.character);
  // Watch the Replay: Kinoko drives player 0 with the ghost, but for sound it is retail's
  // replayed racer. RaceMenuPage's Watch Replay (PAL 0x8085A0F4 case 4/10) only sets the
  // camera mode to 1; the racer stays a type-0 Local one and Scenario::postInitControllers
  // (0x8052ED28: mode 1 after mode 0) gives it a ghost controller. So its KartActor and
  // CharacterActor see a local racer without status 134 (KartState ctor 0x805943B4).
  const bool replayDriver = i == 0 && player.type == K::System::RaceConfig::Player::Type::Ghost;
  k.ghost = player.type == K::System::RaceConfig::Player::Type::Ghost && !replayDriver;
  k.local = player.type == K::System::RaceConfig::Player::Type::Local || replayDriver;
 }
}

void fillRaceAudioItems(RaceAudioFrame& f, const RaceItems& items) {
 f.items = 0;
 const auto& actors = items.actors();
 for (unsigned i = 0; i < actors.size() && f.items < f.item.size(); ++i) {
  const ItemActor& a = actors[i];
  if (!a.active) continue;
  RaceAudioItem& o = f.item[f.items++];
  const Vec3 p = a.visualPosition();
  o.position = {p.x, p.y, p.z}; o.index = std::uint8_t(i); o.kind = std::uint8_t(a.kind);
  o.object = std::uint8_t(items::retailObjectId(a.kind));
  o.phase = a.guidance.phase; o.target = a.target;
  o.explosion = a.explosion; o.scattered = a.scattered; o.settled = a.settled;
 }
 for (const ItemSound& s : items.sounds()) {
  if (f.itemSounds >= f.itemSound.size()) break;
  RaceAudioItemSound& o = f.itemSound[f.itemSounds++];
  o.position = {s.position.x, s.position.y, s.position.z}; o.cue = std::uint8_t(s.cue);
  o.player = s.player; o.actor = s.actor; o.kind = std::uint8_t(s.kind); o.flag = s.flag; o.value = s.value;
  o.object = std::uint8_t(items::retailObjectId(s.kind));
 }
 f.roulette = 0;
 for (unsigned i = 0; i < 12 && i < f.karts; ++i) {
  const ItemHud& h = items.hud(i);
  if (h.roulette) f.roulette |= std::uint16_t(1u << i);
  const bool spin = h.deployed && (h.heldKind == ItemKind::TripleGreenShell || h.heldKind == ItemKind::TripleRedShell);
  f.orbit[i] = spin ? h.heldCharges : 0;
  f.cloud[i] = h.cloud;
  f.kart[i].bulletEnd = h.bulletEnd;
 }
}

void resetRaceAudioObjects() {
 registry.reset();
 K::Field::ObjectBase::SoundCue drop[96];
 while (K::Field::ObjectBase::TakeSoundCues(drop, 96) == 96) {}
}

void fillRaceAudioObjects(RaceAudioFrame& f) {
 const auto* dir = K::Field::ObjectDirector::Instance();
 if (!dir) { f.objects = 0; return; }
 // Both directors: drivable objects (escalators, belts, bridges) live apart.
 const auto& objects = dir->objects();
 const auto* drivables = K::Field::ObjectDrivableDirector::Instance();
 const std::size_t drivableCount = drivables ? drivables->objects().size() : 0;
 if (objects.size() < registry.scanned || drivableCount < registry.scannedDrivable) registry.reset();
 auto scan = [&](const K::Field::ObjectBase* o) {
  const int m = dir->flowTable().soundMode(o->id());
  if ((m == 1 || m == 3) && registry.find(o) < 0) registry.add(o, nullptr, std::uint16_t(o->id()), m);
 };
 for (std::size_t i = registry.scanned; i < objects.size(); ++i) scan(objects[i]);
 for (std::size_t i = registry.scannedDrivable; i < drivableCount; ++i) scan(drivables->objects()[i]);
 registry.scanned = objects.size();
 registry.scannedDrivable = drivableCount;
 // The course's own GOBJ list, once (objects of the race's single local player).
 const auto* map = K::System::CourseMap::Instance();
 if (!registry.courseScanned && map) {
  registry.courseScanned = true;
  bool cannon = false;
  for (std::uint16_t i = 0; i < map->getGeoObjCount(); ++i) {
   const auto* g = map->getGeoObj(i);
   if (!g || !(g->presenceFlag() & 1)) continue;
   const std::uint16_t id = g->id();
   // EffectsMgr (PAL 0x8067C8F8): the splash effects the course's objects bring.
   if (id == 0x7) registry.pocha |= 1; else if (id == 0x11) registry.pocha |= 2; else if (id == 0x12) registry.pocha |= 4;
   const bool idle = idleLoopObject(id) && K::Field::IsObjectBlacklisted(id);
   const bool emitter = emitterObject(id) && g->pathId() >= 0 && map->getPointInfo(std::uint16_t(g->pathId())) &&
                        map->getPointInfo(std::uint16_t(g->pathId()))->pointCount() > 0;
   if (idle || emitter) registry.add(nullptr, g, id, 1);
   // DonkyCannon_wii, DonkyCannonGC, tree_cannon: a RaceAnimActor (ObjFlow sound mode 2) whose
   // shot clip only the first constructed cannon plays (ctor 0x806BFB14 numbers them, +0xe8).
   else if ((id == 0x25f || id == 0x259 || id == 0x261) && !cannon) { cannon = true; registry.add(nullptr, g, id, 2); }
  }
 }
 f.pocha = registry.pocha;
 K::Field::ObjectBase::SoundCue cues[96];
 const std::size_t n = K::Field::ObjectBase::TakeSoundCues(cues, 96);
 for (std::size_t i = 0; i < n; ++i) {
  const auto& c = cues[i];
  int slot = registry.find(c.object);
  if (slot < 0) { const int m = dir->flowTable().soundMode(c.object->id()); slot = registry.add(c.object, nullptr, std::uint16_t(c.object->id()), m == 3 ? 3 : 1); }
  if (slot < 0 || f.objectSounds >= f.objectSound.size()) continue;
  f.objectSound[f.objectSounds++] = {std::uint16_t(slot), c.sound, std::uint8_t(c.call), c.value};
 }
 // The models' BRASD events follow the objects' calc (the global model update, 0x80563640).
 stepAnims(f);
 // Rainbow Road's star rings: this tick's SE_CN_RBW_STAR_RING calls.
 for (const auto* o : objects) {
  if (o->id() != K::Field::ObjectId::StarRing) continue;
  const auto& c = static_cast<const K::Field::ObjectStarRing*>(o)->soundCalls();
  if ((!c.karts && !c.ring) || f.starRings >= f.starRing.size()) continue;
  RaceAudioStarRing& r = f.starRing[f.starRings++];
  r.slot = objectSlot(o); r.karts = c.karts; r.ring = c.ring; r.ringCount = c.ringCount;
  std::copy(c.kartCount.begin(), c.kartCount.end(), r.kartCount);
 }
 // A seagull flock's leader holds SE_CC_KAMOME_SING every frame: ObjectBirdLeader::calc (PAL
 // 0x8077C504, vtable 0x808D0BE8 +0x14) ends with Object::HoldSoundLimited (fn_1_310168, value
 // 0.0) on its own actor (base LoadSound, slot +0x54); the followers' calc (0x8077C7F0) has no
 // sound. Kinoko adds the leader, then the followers, then the ObjectBird (createObjects), so
 // the last object of a seagull placement is the ObjectBird that names its leader.
 for (std::size_t k = 0; k < objects.size(); ++k) {
  const auto* o = objects[k];
  if (o->id() != K::Field::ObjectId::Seagull) continue;
  bool last = true;
  for (std::size_t j = k + 1; j < objects.size() && last; ++j) last = objects[j]->mapObj() != o->mapObj();
  if (!last) continue;
  const auto* leader = static_cast<const K::Field::ObjectBird*>(o)->leader();
  const int m = leader ? dir->flowTable().soundMode(leader->id()) : 0;
  if ((m != 1 && m != 3) || f.objectSounds >= f.objectSound.size()) continue;
  const std::uint8_t slot = objectSlot(leader);
  if (slot != 0xff) f.objectSound[f.objectSounds++] = {slot, 0x240, std::uint8_t(RaceAudioObjectSound::Hold), 0.0f};
 }
 // The emitters follow the listener (RaceMgr::UpdatePlayerMatrix's position, RaceMgr +0x144).
 const float scale = f.listenerScale;
 const EGG::Vector3f listener{f.eye.x + (f.target.x - f.eye.x) * scale, f.eye.y + (f.target.y - f.eye.y) * scale,
                              f.eye.z + (f.target.z - f.eye.z) * scale};
 f.objects = registry.count;
 for (int i = 0; i < registry.count; ++i) {
  if (const auto* o = registry.object[i]) {
   const auto& p = o->getPosition();
   f.object[i] = {{p.x, p.y, p.z}, o->scale().x, registry.id[i], registry.mode[i], registry.index[i]};
   continue;
  }
  const auto* g = registry.geo[i];
  EGG::Vector3f p = g->pos();
  if (emitterObject(registry.id[i]) && map) {
   if (const auto* route = map->getPointInfo(std::uint16_t(g->pathId()))) p = nearestOnRoute(*route, listener);
  }
  f.object[i] = {{p.x, p.y, p.z}, g->scale().x, registry.id[i], registry.mode[i], registry.index[i]};
 }
}
}
