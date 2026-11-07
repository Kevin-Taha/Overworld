# Publication readiness

Changing repository visibility and publishing release assets are separate
maintainer actions. A clean source tree, build, or automated scan does not by
itself authorize either action or prove that historical material is safe.

## Source, history, and rights

Review the complete tracked tree and every hosted branch/tag before changing
visibility, not only the latest diff. Exclude binaries, PDBs, compiler/dependency
caches, model weights, user mappings/settings, recordings, raw diagnostics,
screenshots of unrelated apps, personal paths, credentials, and internal URLs.
`.gitignore` prevents some accidental additions; it neither untracks files nor
scrubs historical commits.

Run a trusted secret scanner over the full history and manually review its
findings. Rotate/revoke any exposed credential before removing it; deleting a
file or branch does not revoke access. Pattern scans cannot certify absence of
secrets. Include commit messages, author/committer emails, tags, agent metadata,
issues, pull requests, attachments, releases, and Actions logs in the privacy
review. A public email or session ID is not necessarily a credential, but
publishing it is still a disclosure decision.

Confirm the rights to license original code/documentation under `LICENSE`.
Keep the vocabulary's CC BY-SA 4.0 attribution, modifications, and full license;
do not relabel it MIT. Keep mapping and voice/model notices under their own terms.
Confirm any new contributions can be distributed under their applicable license.

Preserve unrelated work and the dispositions of declined/parked experiments.
Delete hosted branches only with explicit authorization, including a decision
about unmerged work; never delete a branch or worktree currently being used.
Branch deletion is cleanup, not proof that old commits are inaccessible.
A clean-history publication, if required, needs a separately approved source
snapshot/new repository or coordinated history rewrite, not a visibility toggle.

## GitHub publication setup

Verify the intended default branch and review all remaining hosted refs and
collaboration surfaces. Review the public description and topic labels.
Private vulnerability reporting is a public-repository feature: enable and
verify it immediately after an approved visibility change so the link in
`SECURITY.md` accepts private reports. Until then, that policy's unavailable-form
guidance applies. Review available secret-scanning/push-protection settings and
Actions permissions for the repository's plan and visibility.
These settings are not changed by a source build or this document.

The checked-in Windows CI uses a commit-pinned official checkout action,
read-only repository permissions, no persisted Git credentials, and the default
voice-off deterministic tests. It does not use `pull_request_target`, publish
assets, upload logs, run the mapper, or claim ARM64/voice/device validation.
Confirm it succeeds on GitHub before relying on its status; consider requiring
that check on `main` according to the maintainer's contribution policy.

## Validate and distribute

Use [Building](BUILDING.md) and [Testing](TESTING.md) from the application root.
Separate compilation, deterministic fakes, prerecorded inference, render-only
probes, and authorized real-device results. Do not call a successful paint or
accepted transport proof of visible/delivered behavior.

Review README links, controls, known limits, privacy information, and licensing
against the actual shipping configuration. Make optional/default-off behavior
clear. Do not advertise retired experiments or the development harness as
product modes.

Distribute loose files only: matching `Overworld.exe`, root `LICENSE`, pinned
controller database/license, embedded-vocabulary notices/license, and verified
tiny.en model plus voice licenses for voice-enabled builds. Use the
[standalone release readme](RELEASE-README.md) for end users. Never include
Practice, probes/tests, fixture audio, PDBs, compilers, static libraries, caches,
or a developer's configuration. Verify the file hashes and architecture/flavor;
document any untested runtime instead of implying validation or a signature.

The ARM64 packager requires a committed matching build and stages `LICENSE`
with the other required notices. Its manifest records file hashes and explicitly
does not claim microphone or ARM64 runtime testing. For other flavors, follow
the same [release-file contract](BUILDING.md#release-files).
