# Offline Switch 1 title catalog

This directory supplies **display metadata**, not a game compatibility list, an installed-title inventory or permission to launch a game. Application IDs and local communication IDs (LCIDs) are separate namespaces.

## Frozen inputs and license

The bulk source is the community-maintained, metadata-only [blawar/titledb](https://github.com/blawar/titledb), pinned to commit [`d424690446eec6f4484209b9a1c067c7f7511398`](https://github.com/blawar/titledb/tree/d424690446eec6f4484209b9a1c067c7f7511398). It declares the MIT license; the exact notice is included in `LICENSE.titledb`. This is not Nintendo attestation or endorsement.

`source-lock.json` pins the English storefront files, CN/HK Chinese files and Japanese/Korean original-name sources, with Git blob hashes, sanitized content hashes and the license hash. `provenance.json` records the source or publisher reference for each selected name.

`curated-labels.json` includes publisher references for labels such as Nintendo's [马力欧卡丁车8 豪华版](https://www.nintendoswitch.com.cn/software/70010000027090) and Larian's [神界：原罪2 - 终极版](https://store.steampowered.com/app/435150/Divinity_Original_Sin_2/?l=schinese). `lcid-observations.json` includes only selected application and communication IDs from recorded local metadata or public retail observations.

## Data contract

- `catalog.json` contains sorted base AppIDs and known English and Chinese names. Records without either name may carry `original` and `original_locale` from a Japanese or Korean storefront.
- `lcid-mappings.json` records observed local communication IDs, possible AppIDs and their sources. Title lookup is for display. Local launch still requires verification on the Switch.
- `raw_appid_catalog_candidates` is separate from observed NACP `appids`. Numeric equality alone is a **display-only catalog candidate**, never proof that AppID equals LCID.
- `curated-labels.json` and `lcid-observations.json` contain the readable evidence and limited public observations.
- `name-conflicts.json` retains differing regional/source names; it does not merge application identities. Selection is deterministic: base entry before demo, preferred source locale, then numeric NSUID and name.

For the two Violet-related LCIDs, ambiguity was observed and remains explicit. The Scarlet AppID-valued shared identifier has Violet in its observed NACP list and Scarlet only as a separate raw-ID catalog candidate. Unknown further applications remain possible. A single published candidate must not be treated as a globally unique LCID mapping.

The recorded Sword/Shield, Brilliant Diamond/Shining Pearl, Let's Go Pikachu/Eevee and Scarlet/Violet shared identifiers retain multiple candidates. A room with only a shared identifier displays the candidates. A host-provided game identity can resolve the edition.

## Coverage of the pinned sample

| Measure | Count |
|---|---:|
| Unique eligible Switch 1 base AppIDs | 25,049 |
| English regional names | 20,059 |
| Chinese names | 3,531 |
| Both languages | 2,629 |
| Original-name fallback | 4,088 |
| Explicit observed communication IDs | 9 |
| Known shared communication IDs | 5 |

`coverage.json` includes per-source raw/accepted/excluded counts. These figures describe the pinned input sample, **not every Switch game worldwide** and not supported multiplayer games. Base application IDs can include demos when a valid ID is supplied.

Filtering accepts only exact Switch 1 application IDs (`0100…`, base allocation alignment, application range) and base/demo NSUID product types. Update/subprogram IDs, DLC, bundles, Switch 2 IDs, missing IDs and invalid names are excluded. The persisted field allowlist is limited to identifiers and names. Keys, rights IDs, image URLs/files, descriptions, cheats, NCA/CNMT data and game content are not copied.

## Rebuild and verify

```sh
# Rebuild offline from the existing sanitized OS-temp cache:
python3 -B compatibility/title-catalog/generator.py

# Re-fetch the immutable pinned regional metadata when no cache is present:
python3 -B compatibility/title-catalog/generator.py --fetch

# Optional explicit cache directory outside the source tree:
python3 -B compatibility/title-catalog/generator.py --fetch --cache-dir /tmp/lanywhere-title-catalog-cache

python3 -B compatibility/title-catalog/test_generator.py
```

Raw downloaded metadata is streamed and never written to disk. Sanitized per-region rebuild caches are stored in the OS temporary directory (or the explicit `--cache-dir`), outside tracked/exported paths. No images are fetched. Both network and offline paths check pinned source/snapshot integrity. Output JSON is deterministically sorted; no current-time field changes output bytes. Tests cover namespace exclusions, prohibited-field removal, split UTF-8 parsing, deterministic output, source corruption rejection, partial LCID ambiguity, Sword labeling and absence of private inventory traces.

A catalog refresh is an explicit maintenance action. No recurring or monthly task is created.
