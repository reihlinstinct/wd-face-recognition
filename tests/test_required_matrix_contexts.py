"""Required test names must exist even when documentation skips work."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_required_matrix_remains_instantiated_for_docs():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    job = workflow.split("  test:\n", 1)[1].split("  demo-scan-test:\n", 1)[0]
    assert not any(line.startswith("    if:") for line in job.splitlines())
    assert "    needs: [changes]\n" in job
    assert "python-version: ['3.11', '3.12', '3.13']" in job
    steps = job.split("    steps:\n", 1)[1].split("      - ")[1:]
    assert len(steps) == 4
    assert all("        if: needs.changes.outputs.code == 'true'\n" in s for s in steps)
    assert any("python -m pytest" in step for step in steps)
