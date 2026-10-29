# WD Face Recognition

On-device face detection and grouping for the WD My Cloud PR4100 NAS, packaged
as a My Cloud OS 5 app. This is a development project, not a finished photo
management product. MIT licensed.

## Current implementation and limits

- The C++ demo detects and groups faces from a selected folder. Its PHP pages
  support PIN-gated local runs, folder selection, progress and results.
- The C++ indexer stores incremental file state in SQLite. The shared pipeline
  and `wdf_faces` processing path add detection, alignment, embeddings and crops.
- `service/` contains a Rust service skeleton. Python code and tests are
  development scaffolding, not the intended on-device inference runtime.
- People management, NAS-user permissions and the remaining product milestones
  must not be inferred from the design documents. Check merged code and open
  milestone issues for their current status.

The target has no AVX/AVX2. Container benchmarks are not proof of performance or
compatibility on the NAS. Installation, startup, upgrades, retention and resource
budgets still need the real-device acceptance checks. Never run a development
build against an irreplaceable photo library as an acceptance shortcut.

## Documentation

| Document | Scope |
| --- | --- |
| [Design](docs/DESIGN.md) | Target architecture, privacy and resource budgets; draft, not a completion report |
| [Incremental scanning](docs/INCREMENTAL.md) | Index design and invalidation rules |
| [Demo](demo/README.md) | Demo build and usage |
| [Benchmarks](bench/README.md) | Development benchmark setup |
| [Language comparison](docs/BENCHMARK-LANGUAGE.md) | Runtime comparison and its limits |
| [Device acceptance](docs/DEVICE-TEST.md) | Hardware test run sheet; desired checks, not recorded passes |
| [Contributing](CONTRIBUTING.md) | Issue-first work, tests and review |
| [Security](SECURITY.md) | Reporting vulnerabilities |

## Local development checks

Every workflow ends with a `Run summary` job that writes a summary to the top of the run page: which jobs ran, their result and duration, what was skipped and why, the commits and files the run changed, and which checks cover those changes (`.github/ci-coverage-map.json` declares the coverage; files matching no mapping are flagged). The script is `scripts/ci/run_summary.py`.

Run from the repository root. Python 3.11+ and a C++17 compiler are required for
this subset. Use a virtual environment for the Python development tools:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-dev.txt
ruff check src tests
python -m pytest -q --cov=src --cov-report=term --cov-fail-under=80

g++ -std=c++17 -O2 -Wall -Wextra demo/tests/scan_test.cpp demo/src/scan.cpp -o /tmp/wdf_scan_test
/tmp/wdf_scan_test
```

These synthetic checks cover only the Python scaffold and recursive scanner.
They do not test inference accuracy, the complete indexer, packaging or the NAS.
[CI](.github/workflows/ci.yml) also defines C++ pipeline/indexer and PHP web
checks. The model-dependent demo and packaging have separate workflows and
prerequisites. Read those before downloading dependencies or building artifacts.

## Privacy and contribution rules

Keep photos, embeddings, names linked to real people and NAS diagnostics private.
Use synthetic fixtures only in this public repository, tests and CI artifacts.
One documented exception (#55): the CI end-to-end checks need faces a real
detector fires on, which synthetic pixels cannot provide. They download three
public-domain portraits of deceased historical figures (Lincoln, Hopper,
Einstein) from Wikimedia Commons, each pinned by URL and sha256, into /tmp for
the duration of the job only. They are never committed to the repository, and
CI artifacts carry no image files (logs only). Do not send photos or embeddings
to cloud services. The intended product keeps
processing on the NAS and selects no folders until the owner chooses them.

Open an issue before a focused PR. Include tests and limitations, use signed
commits, and obtain independent review plus green CI before merging. A successful
build is not a device acceptance result or a deployment approval.
