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


def test_summary_distinguishes_skipped_steps_from_executed_tests():
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "run_summary", ROOT / "scripts/ci/run_summary.py"
    )
    summary = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(summary)
    for code in ("false", "true", None):
        for result in ("success", "failure", "cancelled", "skipped"):
            context = {
                "changes": {"result": "success", "outputs": {"code": code}},
                "test": {"result": result},
            }
            expected = "skipped" if code == "false" and result == "success" else result
            assert summary.reported_result("test", context) == expected
    assert summary.reported_result("test", {"test": {"result": "success"}}) == "success"


def test_rendered_summary_reports_docs_skip(tmp_path):
    import json
    import subprocess
    import sys

    output = tmp_path / "summary.md"
    subprocess.run(
        [sys.executable, str(ROOT / "scripts/ci/run_summary.py")],
        check=True,
        env={
            "NEEDS_JSON": json.dumps(
                {
                    "changes": {"result": "success", "outputs": {"code": "false"}},
                    "test": {"result": "success"},
                }
            ),
            "GITHUB_STEP_SUMMARY": str(output),
        },
        cwd=ROOT,
    )
    rendered = output.read_text(encoding="utf-8")
    assert "| `test` | skipped |" in rendered
    assert "1 skipped, 1 success" in rendered
    assert "tests did not execute" in rendered
