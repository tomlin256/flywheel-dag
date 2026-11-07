# Releasing flywheel-dag

A release is a commit, an annotated tag and a GitHub release. A consumer that fetches the engine pins the tag. A change a consumer can see (anything under `include/`, the install or the package config) needs a release; a docs, test or CI change does not.

1. **Scope.** `git log --oneline vX.Y.Z..HEAD` and `git diff vX.Y.Z..HEAD --stat -- include`, then the access (`public:` or
   `private:`) of every signature that changed. A release carries every commit since the last tag, not only the planned
   one, so the commit and the notes list each public change, the source-compatible ones too.
2. **Number.** A new public name is a minor release (v1.1.0, v1.2.0). A fix, or a change that only a class deriving from a
   CRTP base can see, is a patch. A break is a major. The maintainer confirms the number.
3. **Version, three places.** In `CMakeLists.txt`, the `GIT_TAG vX.Y.Z)` snippet in the header comment and
   `project(FLYWHEEL_DAG VERSION X.Y.Z …)`; in `README.md`, the `GIT_TAG vX.Y.Z)` snippet. `test_package_version` derives its
   table from `PROJECT_VERSION`, and `find_package(flywheel_dag 1 …)` keeps its request.
4. **Gate**, on the tree as committed, on macOS and in the Linux container: configure with
   `-Werror=author -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON -DFLYWHEEL_DAG_CLANG_TIDY=clang-tidy`, build, `ctest --preset …`
   (the `clang_tidy_` tests exist only with that last option),
   and `bench_hot_path --invariants` against `benchmarks/expected_invariants.txt`.
5. **Commit and tag.** The release commit is `build(#N): release vX.Y.Z`, where N is the last issue of the batch, and its
   body gives the scope, the access of each change and the results. Push it once, then
   `git tag -a vX.Y.Z <sha> -m "flywheel-dag vX.Y.Z"` and `git push origin vX.Y.Z`, then
   `gh release create vX.Y.Z --title vX.Y.Z --notes-file notes.md --verify-tag --latest`. The notes are a lead paragraph,
   then Added or Changed, Not covered, Tests and Not changed, with issues linked by full URL. No workflow runs on a tag.
6. **CI, afterwards.** The gate in step 4 is the release's check: do not wait for CI before the tag. Read the run on the
   release commit once it finishes. A failure the release carries gets a patch release; a pushed tag is not moved.
7. **Record.** The plan's status and Outcome go in a docs commit. Quote a commit subject there without its scope
   (`build: release vX.Y.Z`): plans and docs cite issues as `flywheel-dag#N`, and a number after a bare `#` reads as an
   issue of another repository.
