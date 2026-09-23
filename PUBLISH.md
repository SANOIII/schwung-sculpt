# Publishing Sculpt to your GitHub (one-time)

Replace `YOUR_NAME` below with your GitHub username.

1. On github.com click **+ → New repository**. Name it `schwung-sculpt`, make it **Public**, and leave README / .gitignore / licence **unticked**.
2. In Terminal on your Mac:

```bash
cd ~/Downloads && unzip -o sculpt-0.1.0-alpha-github.zip && cd schwung-sculpt
GH_USER=YOUR_NAME
grep -rl SANOIII . | xargs sed -i '' "s/SANOIII/$GH_USER/g"
git init -b main && git add -A && git commit -m "Sculpt v0.1.0-alpha"
git remote add origin https://github.com/$GH_USER/schwung-sculpt.git
git push -u origin main
git tag v0.1.0-alpha && git push origin v0.1.0-alpha
```

   If git asks for a password, use a **personal access token** (GitHub → Settings → Developer settings → Tokens, with the `repo` scope). Alternatively, install GitHub CLI (`brew install gh`) and run `gh auth login` first.

3. **Build:** pushing the tag starts GitHub Actions (the repo's **Actions** tab). In about 3 minutes, **Releases** will have `sculpt-module.tar.gz`, cross-compiled for the Move.
4. **Simulator page:** open the repo's **Settings → Pages**. Set Source to *Deploy from a branch*, choose `main` and the `/docs` folder, and save. About a minute later the simulator is live at `https://YOUR_NAME.github.io/schwung-sculpt/`.
5. **Later versions:** bump `"version"` in `src/module.json`, commit, and push a matching tag (e.g. `v0.1.1-alpha`).
