// Knights' Domain: boss fights and arena locks for every player. See BossSync.h.
//
// Table entries (kFns) name a Blueprint function, what it runs on and who decides it:
//   Actor / Any    a level-placed actor (spawner, door, arena wall): whichever machine runs it first sends it; the
//                  others run it on their actor of the same path name. Encounter starts and arena doors.
//   Actor / Host   only the host runs it; a client's own call is skipped and the host's arrives instead (boss resets).
//   Enemy / Owner  a registered enemy: its owner sends, the followers run it on their copy (phase changes, attacks that
//                  damage through a trace instead of a hitbox, hitboxes). With kSuppress a follower's own call is
//                  skipped: its copy is driven by the owner's.
// Calls made while running a received call are not sent again, and neither are calls nested in a sent one (the
// receiver's copy of the outer call makes them itself).
//
// Hitboxes. A boss or elite deals melee and area damage through EnemyBase hitboxes that its attack Blueprint switches
// on from montage notifies ("Activate Hitbox Advanced" ... "Deactivate Hitbox"). Only the owner's copy gets those
// notifies (they are bound to its own PlayMontage proxy); the followers play the mirrored montage without them, so a
// boss's swings never hurt the players on the other machines. The owner now sends every hitbox switch of a boss-like
// enemy and the followers switch the same hitbox (by component name) on their copy, whose TickHitboxes then hits the
// local player. A follower's own switches of a boss-like enemy are skipped so a swing never counts twice.
//
// Arena locks pull nearby players in: when an encounter start arrives from a player within kPullInCm, the local player
// is moved next to where that player stood, so the doors that now close do not lock a teammate out of the fight.
//
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/EnemyBase_classes.hpp"
#include "SDK/FS_PlayerChar_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/WhaleKingPoisonManager_classes.hpp"

#include "qdcoop/sync/BossSync.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/EnemySync.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace qd::sync {
namespace {

constexpr const char* kTag = "bosssync";

enum class Tgt : uint8_t { Actor, Enemy };
enum class Org : uint8_t { Any, Host, Owner };
constexpr uint16_t kLock = 1;        // an encounter start: pull nearby players in
constexpr uint16_t kSuppress = 2;    // Enemy: a follower's own call is skipped
constexpr uint16_t kBossOnly = 4;    // Enemy: only for boss-like enemies (boss bar, elite / boss tier)
constexpr uint16_t kDedupe = 8;      // the same call on the same object within kDedupeSec runs once on each machine
constexpr uint16_t kNoPlayer = 16;   // player parameters arrive as None (an Interact replayed without its message box)
constexpr uint16_t kDirect = 32;     // only calls made outside the enemy's per-frame TickHitboxes
constexpr uint16_t kOnce = 64;       // an encounter start that is not safe to run twice: once started (here or sent by
                                     // another machine) a repeat is neither sent nor run until a kRearm call
constexpr uint16_t kRearm = 128;     // the encounter on this actor ended or reset: its kOnce calls may start it again
constexpr uint16_t kLocal = 256;     // only kRearm: never sent or skipped

struct Fn { const char* path; Tgt tgt; Org org; uint16_t flags; };

// Lane B1: Library and Mausoleum, plus what every boss uses.
constexpr Fn kFns[] = {
    // every boss and elite: the hitboxes of montage-driven attacks
    {"/Game/Enemies/EnemyBase.EnemyBase_C:Activate Hitbox Advanced", Tgt::Enemy, Org::Owner, kSuppress | kBossOnly},
    {"/Game/Enemies/EnemyBase.EnemyBase_C:Deactivate Hitbox", Tgt::Enemy, Org::Owner, kSuppress | kBossOnly},
    // arena doors and gates (Door_Base: Sword Shrine gates, graveyard gates, Executioner front door ...). Opening and
    // closing are never saved, so WorldSync cannot carry them.
    {"/Game/Blueprints/Door_Base.Door_Base_C:CloseDoor", Tgt::Actor, Org::Any, kDedupe},
    {"/Game/Blueprints/Door_Base.Door_Base_C:OpenDoor", Tgt::Actor, Org::Any, kDedupe},
    // a boss reset (the host's player left the fight and nobody else is in it): the clients' doors open again too
    {"/Game/Enemies/EnemySpawner.EnemySpawner_C:PlayerTeleportedOutOfBossArena", Tgt::Actor, Org::Host, 0},
    // Grave Guardian (Sword Shrine): its trigger closes the entrance gate and wakes it
    {"/Game/Enemies/Boner_00/Skeletons24/ShrineBoss/Boner_ShrineBoss_Spawner.Boner_ShrineBoss_Spawner_C:Activate",
     Tgt::Actor, Org::Any, kLock | kDedupe},
    {"/Game/Enemies/Boner_00/Skeletons24/ShrineBoss/Demo/Boner_Shrineboss_Demo_Spawner.Boner_Shrineboss_Demo_Spawner_C:Activate",
     Tgt::Actor, Org::Any, kLock | kDedupe},
    // Executioner: its owner's aggro closes the front door
    {"/Game/Enemies/Boner_00/Skeletons24/Standard/ExecutionerMiniboss_Spawner.ExecutionerMiniboss_Spawner_C:OnEnemyAggrod",
     Tgt::Actor, Org::Any, kLock | kDedupe},
    // Necronomicon: the lectern starts the fight (door, platforms, intro); the door; its forms
    {"/Game/Enemies/Necronomicon/Necronomicon_Boss_Spawner.Necronomicon_Boss_Spawner_C:Interact", Tgt::Actor, Org::Any,
     kLock | kDedupe | kNoPlayer},
    {"/Game/Enemies/Necronomicon/Necro_Boss_Door.Necro_Boss_Door_C:EnableDoor", Tgt::Actor, Org::Any, kDedupe},
    {"/Game/Enemies/Necronomicon/Necro_Boss_Door.Necro_Boss_Door_C:HideDoor", Tgt::Actor, Org::Any, kDedupe},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:TransitionToEater", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:TransitionToDemonForm", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:EnterEyeState", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:ChangeFromBookForm", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:Return From BOok Form", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:ExitEaterMode", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:ExitEyeMode", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:ExitDemonMode", Tgt::Enemy, Org::Owner, kSuppress},
    // the eye laser damages through a line trace, not a hitbox
    {"/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:DoEyeLaser", Tgt::Enemy, Org::Owner, kSuppress},

    // Lane B2: Aqua Ruins and Stone Garden.
    // one-shot hitbox checks a boss runs itself (the Whaleking's stomps and tail slams), outside its hitbox tick
    {"/Game/Enemies/EnemyBase.EnemyBase_C:TickIndividualHitbox", Tgt::Enemy, Org::Owner, kBossOnly | kDirect},
    // Whaleking (and the King of Stone, its child class): the arena water, the poison pools and the horn
    {"/Game/Enemies/WhaleKing/Whaleking_Boss_BP.Whaleking_Boss_BP_C:DrainWater", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/WhaleKing/Whaleking_Boss_BP.Whaleking_Boss_BP_C:RefillWater", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/WhaleKing/Whaleking_Boss_BP.Whaleking_Boss_BP_C:RemovePoison", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/WhaleKing/Whaleking_Boss_BP.Whaleking_Boss_BP_C:KillPoisonCircles", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/WhaleKing/Whaleking_Boss_BP.Whaleking_Boss_BP_C:DoHornBreak", Tgt::Enemy, Org::Owner, kSuppress},
    // Evil Fishwife: her arena's trigger closes the temple door and raises her guardians; the guardians' resurrection
    {"/Game/Enemies/FishWifeEvil/FishWifeEvil_Spawner.FishWifeEvil_Spawner_C:Activate", Tgt::Actor, Org::Any, kLock | kDedupe},
    {"/Game/Enemies/PoisonFishMan/Guardian_Fishman_BP.Guardian_Fishman_BP_C:Resurrect", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/PoisonFishMan/Guardian_Fishman_BP.Guardian_Fishman_BP_C:FinalKill", Tgt::Enemy, Org::Owner, kSuppress},
    // White Tree Golem: using the big nut wakes it
    {"/Game/Enemies/Golem/BigNut_Interact.BigNut_Interact_C:Interact", Tgt::Actor, Org::Any, kLock | kDedupe},

    // Lane B3: secret zones. Encounters that start on the machine whose player set them off (and whose spawners then
    // stayed empty on the others, so the mod removed the other players' bosses as "dead on the owner").
    {"/Game/Enemies/Dragon/Dragon_ROS/ROSDragon_Spawner.ROSDragon_Spawner_C:Activate", Tgt::Actor, Org::Any, kLock | kDedupe},
    {"/Game/RuinsLevel/SecretZones/RoS/Dekanima/Dek_Egg/Dek_Egg_BP.Dek_Egg_BP_C:SpawnDragon", Tgt::Actor, Org::Any,
     kLock | kDedupe},
    {"/Game/Enemies/JosamiKnight_00/DesBoss/Josami_Des_BloodPoolSpawner.Josami_Des_BloodPoolSpawner_C:BeginFight",
     Tgt::Actor, Org::Any, kLock | kDedupe | kOnce},   // a second run raises a second set of knights
    {"/Game/Enemies/RoyalDuo/Fated_Wheeler_MinibossSpawner.Fated_Wheeler_MinibossSpawner_C:StartFight", Tgt::Actor,
     Org::Any, kLock | kDedupe},
    {"/Game/Enemies/BlackRobotDuo/LargeRobot_Miniboss_Spawner.LargeRobot_Miniboss_Spawner_C:StartFight", Tgt::Actor,
     Org::Any, kLock | kDedupe},
    {"/Game/Enemies/BloodPoolDuo/BloodPool_EnemySpawner.BloodPool_EnemySpawner_C:StartFight", Tgt::Actor, Org::Any,
     kLock | kDedupe},
    // Josami Des: the blood dome drops when the kneeling knight has handed over the key (only on the machine whose
    // player talked to it, so the other player stayed caged); its knights' blood revival and the knight that kneels
    {"/Game/Enemies/JosamiKnight_00/DesBoss/Josami_Des_BloodPoolSpawner.Josami_Des_BloodPoolSpawner_C:FinishFight",
     Tgt::Actor, Org::Any, kDedupe | kRearm},
    {"/Game/Enemies/JosamiKnight_00/DesBoss/Josami_Des_BloodPoolSpawner.Josami_Des_BloodPoolSpawner_C:PlayerTeleportedOutOfBossArena",
     Tgt::Actor, Org::Any, kRearm | kLocal},
    {"/Game/Enemies/JosamiKnight_00/JosamiKnight_Base.JosamiKnight_Base_C:BloodReviveImmediately", Tgt::Enemy, Org::Owner,
     kSuppress},
    {"/Game/Enemies/JosamiKnight_00/JosamiKnight_Base.JosamiKnight_Base_C:DoBloodAngelItemGive", Tgt::Enemy, Org::Owner,
     kSuppress},
    // Blood Pool duo: the Stalker and the Snail Bull sink into the pool and come back out
    {"/Game/Enemies/BloodPoolDuo/Stalker_Miniboss_BP.Stalker_Miniboss_BP_C:ForcedDissolve", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/BloodPoolDuo/Stalker_Miniboss_BP.Stalker_Miniboss_BP_C:Submerge", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/BloodPoolDuo/Stalker_Miniboss_BP.Stalker_Miniboss_BP_C:UnDissolve", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/BloodPoolDuo/SnailBull_Miniboss_BP.SnailBull_Miniboss_BP_C:ForcedDissolve", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/BloodPoolDuo/SnailBull_Miniboss_BP.SnailBull_Miniboss_BP_C:Submerge", Tgt::Enemy, Org::Owner, kSuppress},
    {"/Game/Enemies/BloodPoolDuo/SnailBull_Miniboss_BP.SnailBull_Miniboss_BP_C:UnDissolve", Tgt::Enemy, Org::Owner, kSuppress},
};
constexpr int kFnCount = static_cast<int>(sizeof(kFns) / sizeof(kFns[0]));

constexpr const char* kTickHitboxes = "/Game/Enemies/EnemyBase.EnemyBase_C:TickHitboxes";
constexpr double kDedupeSec = 3.0;
constexpr double kOnceSec = 900.0;     // a kOnce start rearms by itself after this long
constexpr float  kPullInCm = 4500.0f;    // a teammate this close when the arena locks is brought in
constexpr float  kPullInMinCm = 600.0f;  // ... unless already standing next to the sender
constexpr size_t kInboxMax = 512;

// FProperty::PropertyFlags
constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

// object parameters on the wire
constexpr uint8_t kRefNone = 0, kRefPlayer = 1, kRefEnemy = 2, kRefComponent = 3, kRefActor = 4, kRefSelf = 5;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Pending { net::MsgCall m; int fn = -1; };

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    std::vector<hooks::HookId> hooks;
    std::unordered_map<std::string, int> byPath;           // fnPath -> kFns index
    int replaying = 0;                                     // depth of received calls being run here
    int sending = 0;                                       // depth of local table calls (only the outermost is sent)
    std::vector<int> stack;                                // per pre-hook: 1 = counted in `sending`
    int tickHitboxes = 0;                                  // inside an enemy's TickHitboxes
    float poisonIn = 0;
    std::unordered_map<std::string, double> localAt;       // "<object key>|<fn>" -> last run here on our own (dedupe)
    std::unordered_map<std::string, double> remoteAt;      // ... -> last run here because another machine sent it
    std::unordered_map<std::string, double> started;       // actor path -> when its kOnce encounter started here
    std::unordered_map<std::string, SDK::UObject*> actors; // actor path -> live actor (cache)
    std::vector<net::MsgCall> outbox;
    std::vector<Pending> inbox;
    int sent = 0, run = 0, dropped = 0;
};
State& S() {
    static State s;
    return s;
}

// ------------------------------------------------------------------ marshalling

struct Writer {
    std::vector<uint8_t>& o;
    void U8(uint8_t v) { o.push_back(v); }
    void Raw(const void* p, size_t n) { o.insert(o.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void Str(const std::string& s) {
        const uint16_t n = static_cast<uint16_t>(s.size() < 250 ? s.size() : 250);
        Raw(&n, 2);
        Raw(s.data(), n);
    }
};
struct Reader {
    const std::vector<uint8_t>& i;
    size_t at = 0;
    bool Raw(void* p, size_t n) {
        if (at + n > i.size()) return false;
        std::memcpy(p, i.data() + at, n);
        at += n;
        return true;
    }
    bool U8(uint8_t& v) { return Raw(&v, 1); }
    bool U64(uint64_t& v) { return Raw(&v, 8); }
    bool Str(std::string& s) {
        uint16_t n = 0;
        if (!Raw(&n, 2) || at + n > i.size()) return false;
        s.assign(reinterpret_cast<const char*>(i.data() + at), n);
        at += n;
        return true;
    }
};

std::string FieldType(const SDK::FField* f) { return f && f->ClassPrivate ? ue::ToString(f->ClassPrivate->Name) : std::string(); }

bool IsPlayerLike(SDK::UObject* o) {
    if (!o || !o->IsA(SDK::AActor::StaticClass())) return false;
    if (o == ue::LocalCharacter()) return true;
    if (o->IsA(SDK::AFS_PlayerChar_C::StaticClass())) return true;
    // EnemySync's invisible stand-ins of remote players are plain engine Characters
    return o->Class == SDK::ACharacter::StaticClass();
}

bool WriteRef(Writer& w, SDK::UObject* o, SDK::AActor* self, bool noPlayer) {
    if (!o || !ue::IsValid(o)) { w.U8(kRefNone); return true; }
    if (o == self) { w.U8(kRefSelf); return true; }
    if (IsPlayerLike(o)) { w.U8(noPlayer ? kRefNone : kRefPlayer); return true; }
    if (o->IsA(SDK::AActor::StaticClass())) {
        if (const uint64_t id = EnemyIdOfActor(static_cast<SDK::AActor*>(o))) {
            w.U8(kRefEnemy);
            w.U64(id);
            return true;
        }
        w.U8(kRefActor);
        w.Str(ue::PathName(o));
        return true;
    }
    if (self && o->IsA(SDK::UActorComponent::StaticClass()) && o->Outer == self) {
        w.U8(kRefComponent);
        w.Str(ue::Name(o));
        return true;
    }
    w.U8(kRefNone);
    return true;
}

// Parameters of `fn` (plain values and object references) as found in `parms`. False: a parameter of a kind that
// cannot travel (the call is then not sent).
bool Marshal(SDK::UFunction* fn, const uint8_t* parms, SDK::AActor* self, bool noPlayer, std::vector<uint8_t>& out) {
    Writer w{out};
    for (SDK::FField* f = fn->ChildProperties; f; f = f->Next) {
        auto* p = static_cast<SDK::FProperty*>(f);
        if (!(p->PropertyFlags & CPF_Parm) || (p->PropertyFlags & (CPF_OutParm | CPF_ReturnParm))) continue;
        if (!parms) return false;
        const std::string type = FieldType(f);
        const uint8_t* at = parms + p->Offset;
        if (type == "BoolProperty") {
            auto* b = static_cast<SDK::FBoolProperty*>(f);
            w.U8((parms[p->Offset + b->ByteOffset] & b->ByteMask) ? 1 : 0);
        } else if (type == "IntProperty" || type == "FloatProperty") {
            w.Raw(at, 4);
        } else if (type == "ByteProperty" || (type == "EnumProperty" && p->ElementSize == 1)) {
            w.Raw(at, 1);
        } else if (type == "NameProperty") {
            w.Str(ue::ToString(*reinterpret_cast<const SDK::FName*>(at)));
        } else if (type == "StructProperty") {
            auto* sp = static_cast<SDK::FStructProperty*>(f);
            const std::string sn = sp->Struct ? ue::Name(sp->Struct) : std::string();
            if ((sn != "Vector" && sn != "Rotator") || p->ElementSize != 12) return false;
            w.Raw(at, 12);
        } else if (type == "ObjectProperty") {
            SDK::UObject* o = nullptr;
            std::memcpy(&o, at, 8);
            WriteRef(w, o, self, noPlayer);
        } else {
            return false;
        }
    }
    return true;
}

SDK::UObject* ComponentByName(SDK::AActor* owner, const std::string& name) {
    if (!owner) return nullptr;
    for (SDK::UActorComponent* c : owner->BlueprintCreatedComponents)
        if (c && ue::Name(c) == name) return c;
    for (SDK::UActorComponent* c : owner->InstanceComponents)
        if (c && ue::Name(c) == name) return c;
    auto& objs = SDK::UObject::GObjects;
    for (int i = 0; i < objs->Num(); ++i) {
        SDK::UObject* o = objs->GetByIndex(i);
        if (o && o->Outer == owner && !o->IsDefaultObject() && o->IsA(SDK::UActorComponent::StaticClass()) && ue::Name(o) == name)
            return o;
    }
    return nullptr;
}

SDK::UObject* ActorByPath(const std::string& path) {
    auto& s = S();
    if (auto it = s.actors.find(path); it != s.actors.end()) {
        if (ue::IsValid(it->second) && ue::PathName(it->second) == path) return it->second;
        s.actors.erase(it);
    }
    SDK::UObject* o = ue::FindObject(path);
    if (!o || !ue::IsValid(o) || !o->IsA(SDK::AActor::StaticClass())) return nullptr;
    auto* a = static_cast<SDK::AActor*>(o);
    if (a->bActorIsBeingDestroyed || !a->RootComponent) return nullptr;   // its sublevel is not loaded here
    if (s.actors.size() > 512) s.actors.clear();
    s.actors[path] = o;
    return o;
}

bool Unmarshal(SDK::UFunction* fn, const std::vector<uint8_t>& in, SDK::AActor* self, uint8_t* parms) {
    Reader r{in};
    for (SDK::FField* f = fn->ChildProperties; f; f = f->Next) {
        auto* p = static_cast<SDK::FProperty*>(f);
        if (!(p->PropertyFlags & CPF_Parm) || (p->PropertyFlags & (CPF_OutParm | CPF_ReturnParm))) continue;
        const std::string type = FieldType(f);
        uint8_t* at = parms + p->Offset;
        if (type == "BoolProperty") {
            auto* b = static_cast<SDK::FBoolProperty*>(f);
            uint8_t v = 0;
            if (!r.U8(v)) return false;
            if (v) parms[p->Offset + b->ByteOffset] |= b->ByteMask;
            else parms[p->Offset + b->ByteOffset] &= static_cast<uint8_t>(~b->ByteMask);
        } else if (type == "IntProperty" || type == "FloatProperty") {
            uint8_t raw[4];
            if (!r.Raw(raw, 4)) return false;
            if (type == "FloatProperty") {
                float v;
                std::memcpy(&v, raw, 4);
                if (!std::isfinite(v)) return false;
            }
            std::memcpy(at, raw, 4);
        } else if (type == "ByteProperty" || (type == "EnumProperty" && p->ElementSize == 1)) {
            if (!r.Raw(at, 1)) return false;
        } else if (type == "NameProperty") {
            std::string n;
            if (!r.Str(n)) return false;
            *reinterpret_cast<SDK::FName*>(at) = ue::MakeName(n);
        } else if (type == "StructProperty") {
            float v[3];
            if (!r.Raw(v, 12)) return false;
            for (float x : v)
                if (!std::isfinite(x)) return false;
            std::memcpy(at, v, 12);
        } else if (type == "ObjectProperty") {
            uint8_t kind = 0;
            if (!r.U8(kind)) return false;
            SDK::UObject* o = nullptr;
            if (kind == kRefPlayer) {
                o = ue::LocalCharacter();
            } else if (kind == kRefEnemy) {
                uint64_t id = 0;
                if (!r.U64(id)) return false;
                o = EnemyActorOfId(id);
            } else if (kind == kRefComponent) {
                std::string n;
                if (!r.Str(n)) return false;
                o = ComponentByName(self, n);
            } else if (kind == kRefActor) {
                std::string path;
                if (!r.Str(path)) return false;
                o = ActorByPath(path);
            } else if (kind == kRefSelf) {
                o = self;
            }
            // only hand a Blueprint an object of the class it declared
            auto* op = static_cast<SDK::FObjectPropertyBase*>(f);
            if (o && op->PropertyClass && !o->IsA(op->PropertyClass)) o = nullptr;
            std::memcpy(at, &o, 8);
        } else {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ local calls

std::string Key(const net::MsgCall& m, int fn) {
    return (m.target == net::kCallEnemy ? std::to_string(m.enemyId) : m.actorPath) + "|" + std::to_string(fn);
}

net::Vec3f LocalPos() {
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!ch) return {};
    const SDK::FVector p = ch->K2_GetActorLocation();
    return {p.X, p.Y, p.Z};
}

bool OnPre(hooks::CallInfo& ci, int idx) {
    auto& s = S();
    const Fn& f = kFns[idx];
    int counted = 0;
    bool skip = false;
    do {
        if (!coop::InSession() || !ci.object || s.replaying > 0) break;
        if (!ci.object->IsA(SDK::AActor::StaticClass())) break;
        auto* self = static_cast<SDK::AActor*>(ci.object);
        net::MsgCall m;
        if ((f.flags & kDirect) && s.tickHitboxes > 0) break;
        if (f.tgt == Tgt::Enemy) {
            m.target = net::kCallEnemy;
            m.enemyId = EnemyIdOfActor(self);
            if (m.enemyId == 0) break;
            if ((f.flags & kBossOnly) && !EnemyIsBossLike(m.enemyId)) break;
            if (!EnemyOwnedHere(m.enemyId)) {
                skip = (f.flags & kSuppress) != 0;   // the owner's call drives this copy
                break;
            }
        } else {
            m.target = net::kCallActor;
            m.actorPath = ue::PathName(self);
            if (m.actorPath.empty()) break;
            if (f.flags & kRearm) s.started.erase(m.actorPath);
            if (f.flags & kLocal) break;
            if (f.org == Org::Host && !coop::IsHost()) {
                skip = true;   // the host decides; its call arrives
                break;
            }
        }
        const std::string key = Key(m, idx);
        const double now = Now();
        if (f.flags & kDedupe) {
            // this machine already ran it because another machine sent it: the game's own late call is the same one
            if (auto it = s.remoteAt.find(key); it != s.remoteAt.end() && now - it->second < kDedupeSec) {
                skip = true;
                break;
            }
        }
        if (f.flags & kOnce) {
            // the encounter is already running here (another player's trigger, a repeat): starting it again would
            // spawn its enemies a second time
            if (auto it = s.started.find(m.actorPath); it != s.started.end() && now - it->second < kOnceSec) {
                QD_INFO(kTag, "{} on {}: the encounter already started here, not run again",
                        std::strrchr(f.path, ':') + 1, ue::Name(self));
                skip = true;
                break;
            }
            s.started[m.actorPath] = now;
        }
        s.localAt[key] = now;
        if (s.sending > 0) break;   // nested in a call that is being sent: the receivers' copy of that call runs it
        counted = 1;
        m.fnPath = f.path;
        m.senderPos = LocalPos();
        if (const auto slot = coop::LocalSlot()) m.ownerSlot = *slot;
        if (!Marshal(ci.function, static_cast<const uint8_t*>(ci.params), self, (f.flags & kNoPlayer) != 0, m.params)) {
            static int warned = 0;
            if (warned++ < 10) QD_WARN(kTag, "{}: a parameter cannot be sent, call stays local", f.path);
            counted = 0;
            break;
        }
        s.outbox.push_back(std::move(m));
    } while (false);
    s.sending += counted;
    s.stack.push_back(counted);
    return skip;
}

void OnPost(hooks::CallInfo&) {
    auto& s = S();
    if (s.stack.empty()) return;
    s.sending -= s.stack.back();
    s.stack.pop_back();
}

// ------------------------------------------------------------------ received calls

void OnCall(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    net::MsgCall m;
    if (!net::Decode(data, size, m)) return;
    if (coop::IsHost()) {
        if (fromSlot == 0) return;
        m.ownerSlot = fromSlot;
        const std::vector<uint8_t> bytes = net::Encode(m);
        for (const coop::Member& mem : coop::Members())
            if (mem.present && !mem.isLocal && mem.slot != fromSlot) coop::SendToSlot(mem.slot, net::Channel::Reliable, bytes);
    } else if (fromSlot != 0) {
        return;
    }
    auto it = s.byPath.find(m.fnPath);
    if (it == s.byPath.end()) return;
    const Fn& f = kFns[it->second];
    if ((f.tgt == Tgt::Enemy) != (m.target == net::kCallEnemy)) return;
    if (f.org == Org::Host && m.ownerSlot != 0) return;
    if (s.inbox.size() >= kInboxMax) return;
    s.inbox.push_back({std::move(m), it->second});
}

void PullIn(const net::MsgCall& m) {
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!ch) return;
    if (auto* pc = static_cast<SDK::AFS_PlayerChar_C*>(ch); pc->Health <= 0) return;
    const SDK::FVector me = ch->K2_GetActorLocation();
    const float dx = m.senderPos.x - me.X, dy = m.senderPos.y - me.Y, dz = m.senderPos.z - me.Z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < kPullInMinCm || d > kPullInCm) return;
    // beside the sender, on the side we came from
    const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    const SDK::FVector at(m.senderPos.x - dx / len * 150.0f, m.senderPos.y - dy / len * 150.0f, m.senderPos.z + 20.0f);
    ch->K2_SetActorLocation(at, false, nullptr, true);
    QD_INFO(kTag, "arena closing: moved next to P{} ({:.0f} m away)", m.ownerSlot + 1, d / 100.0f);
}

void Run(const Pending& p) {
    auto& s = S();
    const Fn& f = kFns[p.fn];
    const net::MsgCall& m = p.m;
    SDK::AActor* self = nullptr;
    if (m.target == net::kCallEnemy) {
        if (EnemyOwnedHere(m.enemyId)) return;   // ownership moved to us: our copy runs it itself
        self = EnemyActorOfId(m.enemyId);
    } else {
        self = static_cast<SDK::AActor*>(ActorByPath(m.actorPath));
    }
    if (!self) {
        ++s.dropped;
        return;   // not loaded here
    }
    const std::string key = Key(m, p.fn);
    const double now = Now();
    if (f.flags & kDedupe) {   // ran it already: on our own, or for another sender
        if (auto it = s.localAt.find(key); it != s.localAt.end() && now - it->second < kDedupeSec) return;
        if (auto it = s.remoteAt.find(key); it != s.remoteAt.end() && now - it->second < kDedupeSec) return;
    }
    if (f.flags & kOnce) {
        if (auto it = s.started.find(m.actorPath); it != s.started.end() && now - it->second < kOnceSec) {
            QD_INFO(kTag, "{} on {} from P{}: the encounter already started here, not run again",
                    m.fnPath.substr(m.fnPath.rfind(':') + 1), ue::Name(self), m.ownerSlot + 1);
            return;
        }
    }
    const std::string name = m.fnPath.substr(m.fnPath.rfind(':') + 1);
    SDK::UFunction* fn = ue::FindFunction(self, name);   // the most derived override, as a Blueprint call would
    if (!fn) return;
    const size_t size = static_cast<size_t>(fn->Size > 0 ? fn->Size : 0);
    std::vector<uint8_t> parms(size + 16, 0);
    if (!Unmarshal(fn, m.params, self, parms.data())) {
        QD_WARN(kTag, "{}: parameters did not match", m.fnPath);
        return;
    }
    if (m.target == net::kCallEnemy) {
        auto* en = static_cast<SDK::AEnemyBase_C*>(self);
        if (en->IsA(SDK::AEnemyBase_C::StaticClass())) {
            if (en->bDead) return;
            // TickHitboxes only runs while the enemy has a target; a followed copy may never have got one
            if (!ue::IsValid(en->TargetPlayer))
                en->TargetPlayer = static_cast<SDK::AFS_PlayerChar_C*>(ue::LocalCharacter());
        }
    }
    s.remoteAt[key] = now;
    if (f.flags & kOnce) s.started[m.actorPath] = now;
    if (f.flags & kRearm) s.started.erase(m.actorPath);
    if (f.flags & kLock) PullIn(m);
    ++s.replaying;
    ue::CallFunction(self, fn, parms.data());
    --s.replaying;
    // hitbox switches come several times a second in a fight: only the first few; everything else every time
    if (!(f.flags & kBossOnly) || ++s.run <= 20 || s.run % 200 == 0)
        QD_INFO(kTag, "ran {} on {} from P{}", name, ue::Name(self), m.ownerSlot + 1);
}

// Whaleking poison: its manager damages the local player standing in any pool of its PoisonCircles list, which only
// the owner's copy of the boss fills (the pools reach the others through HazardSync, the list does not). On a machine
// that follows the boss, the manager's list is every pool in the world; the owner's RemovePoison / KillPoisonCircles
// (mirrored above) switch them off. The engine fills the array itself (GetAllActorsOfClass on the list), so its memory
// stays the engine's.
constexpr size_t kPoisonListOffset = offsetof(SDK::AWhaleKingPoisonManager_C, PoisonCircles);
constexpr const char* kPoisonManager = "/Game/Enemies/WhaleKing/WhaleKingPoisonManager.WhaleKingPoisonManager_C";
constexpr const char* kPoisonCircle = "/Game/Enemies/WhaleKing/WhaleKingPoisonCircle.WhaleKingPoisonCircle_C";
struct RawArray { void* data; int32_t num; int32_t max; };

void FollowPoison() {
    SDK::UClass* mgrCls = ue::FindClass(kPoisonManager);
    SDK::UClass* circleCls = mgrCls ? ue::FindClass(kPoisonCircle) : nullptr;
    if (!mgrCls || !circleCls) return;
    std::vector<SDK::AActor*> managers;
    ActorsOfClass(mgrCls, managers);
    SDK::UObject* lib = SDK::UGameplayStatics::GetDefaultObj();
    static SDK::UFunction* fn = nullptr;
    if (!ue::IsValid(fn)) fn = ue::FindFunction(lib, "GetAllActorsOfClass");
    if (!fn) return;
    for (SDK::AActor* mgr : managers) {
        if (!mgr || !mgr->Owner) continue;
        const uint64_t boss = EnemyIdOfActor(mgr->Owner);
        if (boss == 0 || EnemyOwnedHere(boss)) continue;
        auto* list = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(mgr) + kPoisonListOffset);
        SDK::Params::GameplayStatics_GetAllActorsOfClass p{};
        p.WorldContextObject = mgr;
        p.ActorClass = circleCls;
        std::memcpy(&p.OutActors, list, sizeof(RawArray));
        ue::CallFunction(lib, fn, &p);
        std::memcpy(list, &p.OutActors, sizeof(RawArray));
    }
}

void Tick(float dt) {
    auto& s = S();
    if (coop::InSession() && (s.poisonIn -= dt) <= 0) {
        s.poisonIn = 0.5f;
        FollowPoison();
    }
    if (!coop::InSession()) {
        s.outbox.clear();
        s.inbox.clear();
        s.localAt.clear();
        s.remoteAt.clear();
        s.started.clear();
        return;
    }
    if (!s.outbox.empty()) {
        std::vector<net::MsgCall> out;
        out.swap(s.outbox);
        for (const net::MsgCall& m : out) {
            coop::Broadcast(net::Channel::Reliable, net::Encode(m));
            const auto it = s.byPath.find(m.fnPath);
            const bool hitbox = it != s.byPath.end() && (kFns[it->second].flags & kBossOnly);
            if (!hitbox || ++s.sent <= 20 || s.sent % 200 == 0)
                QD_INFO(kTag, "sent {} ({})", m.fnPath.substr(m.fnPath.rfind(':') + 1),
                        m.target == net::kCallEnemy ? "enemy " + std::to_string(m.enemyId) : m.actorPath);
        }
    }
    if (!s.inbox.empty()) {
        std::vector<Pending> in;
        in.swap(s.inbox);
        for (const Pending& p : in) Run(p);
    }
    for (auto* m : {&s.localAt, &s.remoteAt}) {
        if (m->size() <= 2048) continue;
        const double now = Now();
        for (auto it = m->begin(); it != m->end();) it = now - it->second > kDedupeSec ? m->erase(it) : std::next(it);
    }
}

}  // namespace

bool BossSyncReplaying() { return S().replaying > 0; }

void BossSyncInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    coop::On(net::MsgType::Call, &OnCall);
    for (int i = 0; i < kFnCount; ++i) {
        s.byPath[kFns[i].path] = i;
        s.hooks.push_back(hooks::AddPre(kFns[i].path, [i](hooks::CallInfo& ci) { return OnPre(ci, i); }));
        s.hooks.push_back(hooks::AddPost(kFns[i].path, OnPost));
    }
    s.hooks.push_back(hooks::AddPre(kTickHitboxes, [](hooks::CallInfo&) { ++S().tickHitboxes; return false; }));
    s.hooks.push_back(hooks::AddPost(kTickHitboxes, [](hooks::CallInfo&) { if (S().tickHitboxes > 0) --S().tickHitboxes; }));
    s.ticker = gamethread::AddTicker("bosssync", [](float dt) { Tick(dt); });
    QD_INFO(kTag, "initialized ({} functions)", kFnCount);
}

void BossSyncShutdown() {
    auto& s = S();
    if (!s.inited) return;
    for (hooks::HookId id : s.hooks) hooks::Remove(id);
    s.hooks.clear();
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    s.inited = false;
}

}  // namespace qd::sync
