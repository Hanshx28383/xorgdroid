# Commit Rules

These rules apply to every commit in the `xorgdroid` repository (product: `x11droid`). The goal is a history that is
easy to review, bisect, and reproduce on a host with the NVIDIA 470 driver.

## Before committing

1. Build the components affected by the change. For a full change:

   ```sh
   make clean
   make all
   make -C android/hwcomposer check
   ```

2. Run the relevant functional tests. At minimum, run `make host` and the
   Xvfb CPU-path test for changes in `host-daemon/`, `fb_proto.h`, or the
   shared-memory protocol. Run the NDK build for changes in `android/`.
3. Update `report.txt` when build results, test results, or environment limits
   change.
4. Run `git diff --check` and `git status`. Stage only the files that belong to
   the change.
5. Keep code comments, log messages, and user-facing strings in English. The
   README is the only bilingual file: keep the English and Indonesian sections
   in sync.

Do not commit:

- `build/` output, `.so`, `.a`, `.o`, or generated archives;
- `system.img`, `vendor.img`, rootfs tarballs, or sparse images;
- sockets, logs, PID files, or local test captures;
- credentials, tokens, private keys, or machine-specific paths.

## One logical change per commit

A commit must do one thing. Do not mix a refactor with a behavior change, or a
formatting pass with a bug fix. If a change touches the host daemon and the
Android HAL because the protocol changed, keep it in one commit and explain the
contract change in the body.

Split work when the parts can be reviewed or reverted independently. Do not
create empty commits or commits that only fix a previous commit in the same
unpublished series; amend the unpublished commit instead.

## Message format

Use Conventional Commits:

```text
<type>(<scope>): <imperative summary>

Why:
<the problem or constraint>

What:
- <the concrete change>
- <the concrete change>

Tested:
- <command and result>
- <command and result>

Refs: <issue, report section, or CVE>
```

Rules:

- Subject line: at most 72 characters, imperative mood, no trailing period.
- Use lowercase after the type unless the word is a proper noun.
- Body lines: wrap at 72 characters.
- Explain *why* in the body; the diff already shows *what* changed.
- Put test commands and outcomes in the `Tested:` block.
- Use `Refs:` for related issues or reports. Omit it if there is no reference.
- Write commit messages in English.

### Types

| Type | Use for |
| --- | --- |
| `feat` | New user-visible behavior or a new component |
| `fix` | A bug fix |
| `build` | Makefiles, NDK configuration, or packaging |
| `docs` | README, CONTRIBUTING, comments, or report text |
| `test` | Test harnesses, mock servers, or test scripts |
| `refactor` | A behavior-preserving restructure |
| `perf` | A measurable performance change |
| `chore` | Repository maintenance with no product behavior change |
| `revert` | Reverting a previous commit |

### Scopes

Use the narrowest scope that identifies the area:

`host`, `glx`, `shm`, `input`, `hwc`, `gles`, `proto`, `lxc`, `scripts`,
`build`, `docs`, `test`.

A scope is optional for changes that span the repository.

## Examples

Good:

```text
fix(glx): fall back to XPutImage when GLX is unavailable

Why:
Xvfb does not expose GLX. The daemon previously left the window blank when
glXChooseVisual failed.

What:
- try a double-buffered visual, then a single-buffered visual
- fall back to the X11 CPU path and log the selected renderer
- keep the GLES server disabled when no GLX context exists

Tested:
- make host
- Xvfb :99 720x1280x24; daemon --no-gl; 30 produced frames matched
- daemon with default flags reported the CPU fallback

Refs: report.txt, section 3
```

```text
build(hwc): add HWC_NAME for non-default hardware names

Why:
Some Android images read ro.hardware.hwcomposer from a different value.

What:
- parameterize the output and soname with HWC_NAME
- stage hwcomposer.$(HWC_NAME).so under VENDOR_LIBDIR/hw

Tested:
- make -C android/hwcomposer HWC_NAME=redroid
- readelf -d build/android/hwcomposer/hwcomposer.redroid.so
```

Bad:

```text
update files
```

The subject says nothing about the change, has no scope, and gives no test
evidence.

```text
fix stuff

also reformat the whole tree and add a new feature
```

This mixes a fix, formatting, and a feature, so it cannot be reviewed or
reverted cleanly.

## Branches and history

- Use a topic branch for work in progress:

  ```text
  feat/<scope>-<short-description>
  fix/<scope>-<short-description>
  docs/<short-description>
  ```

- Rebase onto the current base branch before opening a pull request.
- Do not amend or force-push a commit that other people have based work on.
- Do not rewrite published history unless the repository owner explicitly asks
  for it.

## Pull request checklist

- The commit series is atomic and follows the message format.
- `make all` passes for the affected components.
- The relevant host and/or NDK tests pass; paste the commands in the PR.
- `report.txt` is updated when results or limitations change.
- README changes keep the English and Indonesian sections aligned.
- No generated artifacts, images, logs, sockets, or secrets are included.
- The change preserves the AOSP header notices in
  `android/hwcomposer/include/NOTICE`.
