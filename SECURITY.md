# Security Policy

## Reporting a vulnerability

Please open a private security advisory on this repository
(Security -> Advisories) rather than a public issue:
https://github.com/reihlinstinct/wd-face-recognition/security/advisories/new

Languages: Hebrew or English. No personal email is published on purpose.

## Scope notes

- This app processes personal photos ON the owner's NAS only. No real photos,
  face embeddings, names of real people, or private NAS data belong in this
  repo, its issues/PRs, tests, CI artifacts, or logs - synthetic fixtures only.
- No credentials, tokens or personal data belong in this repo; it is scanned
  with GitHub secret scanning + push protection, and gitleaks runs in CI.
- The app must NEVER make outbound network connections - 100% on-device, no
  cloud calls, no telemetry, no exceptions. Treat any code introducing one as
  a security issue.
