# Publishing this project on GitHub

This repository is **source-first**. Do not commit compiled binaries or the
release zip; attach them to a GitHub Release instead.

## 1. Pre-flight checklist

- [ ] `argonmine-gpu.exe --selftest --batch 512` passes, including the mainnet
      block 652 vector
- [ ] `argonmine-gpu.exe --benchmark 5 --batch 1024` runs on your GPU
- [ ] No `argonmine-gpu.cfg`, `miner-status.json`, personal paths, or private
      keys are in the tree
- [ ] `LICENSE`, `NOTICE`, and `LICENSES/` are present
- [ ] `README.md` (English) and `README.zh-CN.md` (Chinese) are up to date
- [ ] `docs/dashboard.png` is the current dashboard screenshot
- [ ] The version in `res/app.rc` matches the release name

## 2. Create the repository

Create an empty repository on GitHub (no README, no .gitignore, no license
template, because this tree already has them). Suggested name:

```text
argonmine-gpu-miner
```

Then, from this directory:

```bat
git init
git add .
git commit -m "Initial commit: Argonmine GPU miner"
git branch -M main
git remote add origin https://github.com/<your-name>/argonmine-gpu-miner.git
git push -u origin main
```

If Git asks for an identity:

```bat
git config user.name "Your Name"
git config user.email "you@example.com"
```

## 3. Build the release zip

```bat
build.bat
```

The release folder is:

```text
dist\argonmine-gpu-1.0.1\
  argonmine-gpu.exe
  dashboard.bat / dashboard.py / dashboard.html
  setup.bat / run.bat / watchdog.bat / stop.bat
  README.md / README.zh-CN.md / 使用说明.txt
  LICENSE / NOTICE
  docs\dashboard.png
```

Zip that folder and upload it as a release asset. Do not commit the zip.

## 4. Create the release

Web UI:

1. Repository → **Releases** → **Draft a new release**
2. Tag: `v1.0.1`
3. Title: `Argonmine GPU Miner v1.0.1`
4. Upload `dist\argonmine-gpu-1.0.1-win64.zip`
5. Paste the release notes (features, hashrate, known explorer bug, checksums)

Or with GitHub CLI:

```bat
gh release create v1.0.1 dist\argonmine-gpu-1.0.1-win64.zip ^
  --title "Argonmine GPU Miner v1.0.1" ^
  --notes "Native CUDA Argon2id solo miner for Argonmine (ARG). See README."
```

## 5. Repository settings

Suggested description:

> Unofficial native CUDA GPU miner for Argonmine (ARG), with template verification and a local web dashboard.

Suggested topics:

```text
argonmine, arg, gpu-miner, cuda, argon2id, windows, solo-mining
```

Recommended:

- Social preview: `docs/dashboard.png` or the project icon
- Enable Issues
- Enable private vulnerability reporting (Security tab)
- Do not enable Actions that publish binaries automatically; build releases
  locally or in a controlled workflow

## 6. Versioning

When you bump the version:

1. `res/app.rc` — `FILEVERSION` / `PRODUCTVERSION` and strings
2. `README.md` / `README.zh-CN.md` — release name and screenshots
3. `dist-src/使用说明.txt` — version line
4. Build, self-test, re-zip
5. Tag and release

## 7. What not to commit

`.gitignore` already excludes:

```text
*.exe  *.obj  *.lib  *.exp  *.pdb  *.ilk  *.res
dist/
miner-status.json  miner-status.json.tmp
argonmine-gpu.cfg
__pycache__/
```

Never commit:

- recovery phrases, private keys, wallet passwords
- personal `argonmine-gpu.cfg` files
- build outputs or release zips
- anything that would silently mine without the user's knowledge
