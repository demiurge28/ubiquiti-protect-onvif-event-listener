# onvif-recorder APT repository (gh-pages)

This directory is a template for the contents of the `gh-pages` branch, which
is served as an APT repository at:

    https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/

## Bootstrap (one-time)

From `main`, copy the scaffolding into a fresh `gh-pages` branch:

```bash
git worktree add /tmp/gh-pages-bootstrap --orphan gh-pages
cp -r gh-pages/* /tmp/gh-pages-bootstrap/
cp -r gh-pages/.gitignore /tmp/gh-pages-bootstrap/ 2>/dev/null || true

# Export the public half of the release-signing key.
gpg --armor --export <KEYID> > /tmp/gh-pages-bootstrap/onvif-recorder.gpg

cd /tmp/gh-pages-bootstrap
git add -A
git commit -m "Bootstrap gh-pages apt repo (stable/rc/early-access)"
git push -u origin gh-pages
cd -
git worktree remove /tmp/gh-pages-bootstrap
```

Enable GitHub Pages for the repo: Settings → Pages → Source = `gh-pages` branch,
root directory.

## Layout after first release

```
/                                       apt root
  conf/
    distributions                       suite definitions (stable/rc/early-access)
    options
  dists/
    stable/         {InRelease, Release, Release.gpg, main/binary-arm64/Packages.gz}
    rc/             {…}
    early-access/   {…}
  pool/main/o/onvif-recorder/onvif-recorder_*_arm64.deb
  onvif-recorder.gpg                    public key (ASCII-armoured)
  install.sh                            one-line installer
```

## Secrets

The release workflow needs two GitHub Actions secrets:

- `GPG_SIGNING_KEY` — ASCII-armoured *private* half of the release signing key.
  Generate with `gpg --armor --export-secret-keys <KEYID>`.
- `GITHUB_TOKEN` is provided automatically.
