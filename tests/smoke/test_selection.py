from scripts.run_smoke import select_missions
from scripts.smoke_lib.manifest import parse_manifest

SAMPLE = """
tier1 mc2_01
tier1 mc2_17
tier2 mc2_02
skip ai_glenn reason="dev leftover"
"""


def _entries(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text(SAMPLE)
    return parse_manifest(p)


def test_select_known_missions(tmp_path):
    selected, unknown = select_missions(_entries(tmp_path), ["mc2_01", "mc2_02"])
    assert [e.stem for e in selected] == ["mc2_01", "mc2_02"]
    assert unknown == []


def test_select_unknown_mission_reported(tmp_path):
    selected, unknown = select_missions(_entries(tmp_path), ["bogus_mission_xx"])
    assert selected == []
    assert unknown == ["bogus_mission_xx"]


def test_select_mixed_known_and_unknown(tmp_path):
    selected, unknown = select_missions(
        _entries(tmp_path), ["mc2_01", "bogus_a", "bogus_b"])
    assert [e.stem for e in selected] == ["mc2_01"]
    assert unknown == ["bogus_a", "bogus_b"]


def test_select_skip_tier_counts_as_unknown(tmp_path):
    # A skip-tier mission can't run; requesting it must not look like success.
    selected, unknown = select_missions(_entries(tmp_path), ["ai_glenn"])
    assert selected == []
    assert unknown == ["ai_glenn"]


def test_select_dedupes_requests(tmp_path):
    selected, unknown = select_missions(_entries(tmp_path), ["mc2_01", "mc2_01"])
    assert [e.stem for e in selected] == ["mc2_01"]
    assert unknown == []
