// Knights' Domain: boss fights and arena locks for every player.
//
// Boss encounters are run by Blueprint code that only runs on one machine: the spawner whose trigger the local player
// crossed closes the arena doors, a boss's owner changes its form, an enemy's owner switches the hitboxes of a
// montage-driven attack on. BossSync keeps a table of those Blueprint functions. When one runs on the machine that
// decides it (any machine for the encounter and door functions, the enemy's owner for enemy functions), the call is
// sent (MsgCall) and every other machine runs the same function on its own copy of the same object.
#pragma once

namespace qd::sync {

void BossSyncInit();       // after EnemySyncInit (uses enemy ids and ownership)
void BossSyncShutdown();

// True while this machine runs a call another machine sent. HazardSync removes attack actors a followed enemy spawns
// during such a replay: the owner's own copies of them arrive through HazardSync.
bool BossSyncReplaying();

}  // namespace qd::sync
