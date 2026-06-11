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

2. **Automatic release on push**: Pushing a commit to `main` that changes the
   `vsn` in `src/glazer.app.src` immediately triggers the `release.yaml`
   workflow, which:
   - Creates a GitHub release
   - Publishes the package to Hex.pm
   - Retires the prior version on Hex.pm as deprecated

   No git tag is created at this point — publishing to Hex.pm doesn't depend
   on one.

3. **Delayed tag creation**: An hourly scheduled run of `release.yaml` checks
   whether the latest version published on Hex.pm already has a matching git
   tag. If not, it locates the commit on `main` that introduced that version
   and creates/pushes the tag for it. This keeps tag creation decoupled from
   publishing, so a flaky publish doesn't leave behind a tag for a version
   that never made it to Hex.pm.

## Key Features

- **Push-driven publishing**: No separate tagging step is needed to trigger a
  release — a version bump commit on `main` is the trigger
- **Idempotent publishing**: `publish-to-hex` checks whether the version is
  already on Hex.pm before publishing, so re-running the workflow is safe
- **Tags follow Hex.pm**: Git tags are created after the fact for whatever
  version is actually live on Hex.pm, not the other way around

## Manual Release (if needed)

To (re)publish a specific version manually, trigger `release.yaml` via
`workflow_dispatch` from the GitHub Actions UI or:

```bash
gh workflow run release.yaml --field tag=0.4.0
```

Leaving `tag` empty publishes whatever version is currently in
`src/glazer.app.src` on the default branch. `workflow_dispatch` runs do not
create a git tag — that still happens via the scheduled job once the version
is visible on Hex.pm.

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

- **Release not triggered on push**: Confirm the push to `main` actually
  changed the `{vsn, "..."}` line in `src/glazer.app.src` — `release.yaml`
  only runs on pushes that touch that file, and only proceeds if the version
  differs from the parent commit's
- **Release not published**: Verify the Hex API key is configured in repository secrets (`HEX_API_KEY`)
- **Tag not created**: The scheduled job only tags a version once it's
  visible via `mix hex.info glazer` — wait for the next hourly run after a
  successful publish, or check that the publish actually succeeded
- **Version mismatch**: Ensure the version in `src/glazer.app.src` matches the intended release version (without the `v` prefix)
- **API key revoked**: Getting `Failed to publish package glazer - 0.1.0 : API key revoked`
error. Ensure to call `rebar3 hex user deauth` and `rebar3 hex user auth` before `rebar3 hex publish`
