'use strict';

// Use the first usable LCID from the ordered NACP arrays, as the Switch
// appointment client does. These are the two app/LCID pairs observed in
// compatibility/title-catalog/lcid-observations.json (local_nacp_20261107).
// Labels come from the corresponding records in curated-labels.json.
// This list is partial. An observed LCID can be shared by other apps and
// does not establish launch compatibility. Keep the AppID and LCID separate.
const games = Object.freeze([
    Object.freeze({
        game: '01008C8012920000',
        app: '01008C8012920000',
        name: 'Dying Light: Platinum Edition',
        zh: '《消逝的光芒：白金版》',
    }),
    Object.freeze({
        game: '0100A3D008C5C000',
        app: '01008F6008C5E000',
        name: 'Pokémon Violet',
        zh: '宝可梦 紫',
    }),
]);

module.exports = { games };
