#include <assert.h>
#include <stddef.h>
#include "../../common/game_update_compatibility.h"

int main(void)
{
    const uint64_t mario = UINT64_C(0x0100152000022000);
    const uint64_t smash = UINT64_C(0x01006A800016E000);
    const uint64_t scarlet = UINT64_C(0x0100A3D008C5C000);
    const uint64_t violet = UINT64_C(0x01008F6008C5E000);
    const uint64_t splatoon = UINT64_C(0x0100C2500FC20000);
    const uint64_t mario3d = UINT64_C(0x010028600EBDA000);
    const uint64_t bayonetta = UINT64_C(0x01007960049A0000);
    const uint64_t divinity = UINT64_C(0x010027400CDC6000);
    assert(room_game_update_alternate(scarlet) == violet);
    assert(room_game_update_alternate(violet) == scarlet);
    assert(!room_game_update_alternate(mario));
    assert(room_game_updates_incompatible(mario, "4.0.1", "4.0.0"));
    assert(room_game_updates_incompatible(mario, "3.0.5", "4.0.1"));
    assert(room_game_updates_incompatible(mario, "1.4.0", "1.2.1"));
    assert(!room_game_updates_incompatible(mario, "1.4.0", "1.3.0"));
    assert(!room_game_updates_incompatible(mario, "4.0.2", "4.0.1"));
    assert(room_game_updates_incompatible(smash, "13.0.5", "13.0.4"));
    assert(!room_game_updates_incompatible(smash, "13.0.5", "v13.0.5"));
    assert(room_game_updates_incompatible(scarlet, "4.0.0", "3.0.1"));
    assert(room_game_updates_incompatible(violet, "3.0.1", "4.0.0"));
    assert(room_game_updates_incompatible(splatoon, "3.0.1", "3.1.0"));
    assert(room_game_updates_incompatible(splatoon, "6.0.0", "5.2.0"));
    assert(!room_game_updates_incompatible(splatoon, "6.0.1", "6.0.2"));
    assert(room_game_updates_incompatible(mario3d, "1.2.2", "1.0.0"));
    assert(!room_game_updates_incompatible(mario3d, "1.2.2", "1.2.1"));
    assert(!room_game_updates_incompatible(mario3d, "1.1.0", "1.2.2"));
    assert(room_game_updates_incompatible(bayonetta, "1.0.0", "1.2.0"));
    assert(!room_game_updates_incompatible(bayonetta, "1.2.0", "1.1.0"));
    assert(!room_game_updates_incompatible(divinity, "1.0.0", "2.0.0"));
    const char *unknown[] = {NULL, "", "v", "1.", "1.2.", "1.2.3-beta", "1.2.3.4", "99999.2.1"};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i)
        assert(!room_game_updates_incompatible(smash, unknown[i], "13.0.4"));
}
