# Contributing

Thanks for helping with on-device face clustering and naming for the WD PR4100 NAS (OS5 app).

## Ground rules

- Keep face processing and all photo data on the NAS. Do not add real photos, face embeddings, names linked to real people, or private NAS data to this public repository, issues, PRs, tests, CI artifacts, or logs.
- Use synthetic fixtures only. Do not send images or embeddings to external services, including for development or diagnostics.
- Keep dependencies, permissions, and network access minimal. Document any new dependency and its license.
- Do not include secrets, personal data, or private operational details in commits, logs, or examples.

## How work flows

1. Open an issue first (or comment on an existing one). Bugs need reproducible evidence that contains no real personal data.
2. Make one focused change per branch and pull request. Link the issue and explain purpose, tests, and limitations.
3. Run the relevant tests locally. CI must pass and a maintainer must approve before merge.
4. Squash-merge with a full commit description that explains why and references the issue.

## Tests and privacy

Tests must use synthetic images and identities. Never upload actual user photos to GitHub Actions or other hosted CI. Include automated regression tests for bug fixes; if automation is not feasible, explain why and open a test follow-up issue.
