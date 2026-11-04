"""Required test names must exist even when documentation skips work."""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]


def test_required_matrix_remains_instantiated_for_docs():
    job = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text())["jobs"][
        "test"
    ]
    assert "if" not in job
    assert job["strategy"]["matrix"]["python-version"] == ["3.11", "3.12", "3.13"]
    assert job["needs"] == ["changes"]
    assert job["steps"]
    assert all(
        step.get("if") == "needs.changes.outputs.code == 'true'"
        for step in job["steps"]
    )
    assert any("pytest" in step.get("run", "") for step in job["steps"])
