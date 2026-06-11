# Publishing a Release

This project uses automated workflows to manage releases. Follow these steps to publish a new version.

## Process

1. Update the version (use one of the two methods below):
    - Automated
      ```sh
      $ make bump-version
      Bumping version from 0.2.1 to 0.2.2
      Changed: {vsn, "0.2.1"} -> {vsn, "0.2.2"}

      Commit this change? [Y/n]

      [main 443a400] Bump version to 0.2.2
       2 files changed, 25 insertions(+), 2 deletions(-)

      **Push the change**
      $ git push origin main
      ```

    - Manual<br/>
      **Update the version** in `glazer.app.src`:
      ```elixir
      {vsn,"2.3.1"}
      ```
      **Commit the version bump and push the change**:
      ```bash
      $ git add src/glazer.app.src
      $ git commit -m "Bump version to 2.3.1"
      $ git push origin main
      ```

2. **Automatic release on push**: Pushing a commit to `main` triggers the
   `build` workflow (`erlang.yaml`) as usual. Once the full build matrix
   (Linux, macOS, Windows) succeeds, the workflow publishes to Hex.pm and
   retires the prior version as deprecated if either:
   - The `vsn` in `src/glazer.app.src` changed compared to the parent
     commit, or
   - The `vsn` is unchanged but this commit landed within an hour of the
     previous one — treated as a quick follow-up fix to the same release,
     and republished with `--replace`

   If the build fails, nothing is published. No git tag is created at this
   point — publishing to Hex.pm doesn't depend on one.

3. **Delayed tag creation**: An hourly scheduled run of `release.yaml` checks
   whether the latest **non-retired** version published on Hex.pm already has
   a matching git tag. If not, it locates the commit on `main` that
   introduced that version and, once that commit is **at least 6 hours old**,
   creates/pushes the tag for it. The age check leaves a window for amended
   re-publishes of the same version on Hex.pm before it gets tagged. This
   also keeps tag creation decoupled from publishing, so a flaky publish
   doesn't leave behind a tag for a version that never made it to Hex.pm.

## Key Features

- **Build-gated publishing**: Publishing to Hex.pm only happens after the
  full `build` matrix (Linux, macOS, Windows) succeeds for a push to `main`
  that bumps `vsn` (or quickly follows a bump, see above)
- **Idempotent publishing**: `publish-to-hex` checks whether the version is
  already on Hex.pm before publishing, so re-running the workflow is safe
- **Tags follow Hex.pm**: Git tags are created after the fact for whatever
  non-retired version is actually live on Hex.pm, not the other way around
- **Tagging delay**: Tags are only created for a version-bump commit once
  it's at least 6 hours old, allowing amended re-publishes of the same
  version before it's tagged

## Setup Requirements

The automated workflow requires proper GitHub token configuration:

### 1. Personal Access Token (PAT)

The scheduled job needs to push tags in a way that triggers other workflows.
The default `GITHUB_TOKEN` cannot trigger workflows for security reasons, so
a Personal Access Token is required.

**Create and configure `RELEASE_PAT`**:

1. Go to Profile → Settings → Developer settings → Personal access tokens → Tokens (classic)
2. Click "Generate new token (classic)"
3. Give it a name like "Release Workflow Token"
4. Select the following scopes:
   - `repo` — Full control of private repositories
   - `workflow` — Update GitHub Actions workflows
5. Click "Generate token" and copy the token (you can only see it once)
6. Add it to your repository secrets:
   - Go to your repo → Settings → Secrets and variables → Actions
   - Click "New repository secret"
   - Name: `RELEASE_PAT`
   - Value: Paste the token you just created

### 2. Hex API Key

For publishing to Hex.pm, configure your API key:

1. Go to your repository → Settings → Secrets and variables → Actions
2. Click "New repository secret"
3. Name: `HEX_API_KEY`
4. Value: Your Hex.pm API key (from https://hex.pm/users/account/keys)

## Troubleshooting

- **Release not published**: Confirm the `build` workflow (`erlang.yaml`)
  succeeded for the push — publishing only runs after `linux`, `macOS`, and
  `windows` all pass. Also confirm either the push changed the
  `{vsn, "..."}` line in `src/glazer.app.src` compared to the parent commit,
  or it landed within an hour of the commit that did, and verify the Hex API
  key is configured in repository secrets (`HEX_API_KEY`)
- **Tag not created**: The scheduled job only tags a version once it's
  visible via `mix hex.info glazer`, the version is not retired on Hex.pm,
  and the version-bump commit is at least 6 hours old — wait for a later
  hourly run, or check that the publish actually succeeded
- **Version mismatch**: Ensure the version in `src/glazer.app.src` matches the intended release version (without the `v` prefix)
- **API key revoked**: Getting `Failed to publish package glazer - 0.1.0 : API key revoked`
error. Ensure to call `rebar3 hex user deauth` and `rebar3 hex user auth` before `rebar3 hex publish`
