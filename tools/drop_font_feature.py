"""Take one OpenType layout feature out of a font, and change nothing else.

    python tools/drop_font_feature.py IN.ttf OUT.ttf liga

How `assets/fonts/chivo-mono/ChivoMono-Medium.ttf` was made from Google Fonts' own static
Medium instance of Chivo Mono (the one the approved mockup rendered): the rule editor sets colour
codes in it, and "#20ff80" has to read f, f — but the font's `liga` feature joins them, and Slint
has no way to switch a feature off (HANDOFF §0.5, the rule editor locked 2026-09-30). So our copy
has no `liga`: its feature record goes, and every script's list of features is renumbered without
it. The lookups, the glyphs (the ligature glyphs among them, now unreachable) and every other
table are left as they were.

Needs fontTools (`pip install fonttools`), which only this tool uses; nothing in the build does.
The font is under the SIL Open Font License, which permits a modified version; Chivo's declares
no Reserved Font Name, so the modified copy keeps its name. See THIRD-PARTY-NOTICES.txt.
"""
import sys

from fontTools.ttLib import TTFont


def drop(font, tag):
    for table_tag in ("GSUB", "GPOS"):
        if table_tag not in font:
            continue
        table = font[table_tag].table
        records = table.FeatureList.FeatureRecord
        keep = [i for i, record in enumerate(records) if record.FeatureTag != tag]
        if len(keep) == len(records):
            continue
        renumber = {old: new for new, old in enumerate(keep)}
        table.FeatureList.FeatureRecord = [records[i] for i in keep]
        table.FeatureList.FeatureCount = len(keep)
        for script in table.ScriptList.ScriptRecord:
            systems = [script.Script.DefaultLangSys] if script.Script.DefaultLangSys else []
            systems += [lang.LangSys for lang in script.Script.LangSysRecord]
            for system in systems:
                system.FeatureIndex = [renumber[i] for i in system.FeatureIndex if i in renumber]
                system.FeatureCount = len(system.FeatureIndex)
                if system.ReqFeatureIndex != 0xFFFF:
                    system.ReqFeatureIndex = renumber.get(system.ReqFeatureIndex, 0xFFFF)
        if getattr(table, "FeatureVariations", None) is not None:
            sys.exit(f"{table_tag} has feature variations, which this does not renumber")
        print(f"{table_tag}: dropped {len(records) - len(keep)} '{tag}' feature record(s)")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, target, tag = sys.argv[1:]
    font = TTFont(source)
    drop(font, tag)
    font.save(target)


if __name__ == "__main__":
    main()
