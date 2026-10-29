# Automatic Windows builds on your own PC

`.github/workflows/local-windows.yml` builds the Windows package on your own
PC whenever a branch of the fork (`main`, `netcode-v2`) gets new commits, once
a night if something is new, and whenever you start it by hand. It uses
GitHub's **self-hosted runner**: a small program from GitHub that waits for
build jobs and runs them on your PC.

- Your ROM never leaves your PC. The build reads it from a path you set.
- Builds are not uploaded anywhere. Each one is a zip in a folder on your PC
  (`TroubleMakers-<date>-<branch>-<commit>.zip`).

## 1. Install the build tools (once)

All official installers:

- **Git for Windows**: https://git-scm.com/download/win (its Git Bash is used
  by some build steps).
- **CMake**: https://cmake.org/download/ (tick "Add CMake to the PATH").
- **Visual Studio 2022 Build Tools**:
  https://visualstudio.microsoft.com/downloads/ ("Tools for Visual Studio" >
  "Build Tools for Visual Studio 2022"). In the installer pick **Desktop
  development with C++**, and under Individual components also **C++ Clang
  Compiler for Windows** and **C++ CMake tools for Windows** (this one brings
  Ninja).

## 2. Set up the runner (once)

1. On GitHub: your fork > **Settings** > **Actions** > **Runners** > **New
   self-hosted runner** > **Windows**, x64.
2. Follow the commands GitHub shows there in PowerShell (they download the
   runner into a folder such as `C:\actions-runner` and register it). When
   `config.cmd` asks for **additional labels**, type `troublemakers`. When it
   asks whether to run as a service, answer **Y** if it should build while you
   are logged out, or **N** and start `run.cmd` yourself when you want builds.
3. In the runner folder create a file named `.env` with (your own paths):

   ```
   TM_ROM_PATH=C:\Games\N64\mischief_makers_us11.z64
   TM_STEAM_DLL=C:\TroubleMakersBuildAssets\steam_api64.dll
   TM_BUILDS_DIR=C:\Users\Max\TroubleMakersBuilds
   ```

   `TM_STEAM_DLL`: copy `steam_api64.dll` out of a preview zip into a folder of
   its own (the build checks its SHA-256). Leave the line out for builds
   without Steam netplay. Restart the runner (or the service) after editing
   `.env`.

## 3. Keep strangers' code off your PC (important)

The fork is public. GitHub's own advice is that a self-hosted runner on a
public repository can run code from other people's pull requests if the
repository allows it. The workflow never runs for pull requests, but a pull
request can bring its own workflow file, so also set:

- **Settings** > **Actions** > **General** > *Fork pull request workflows from
  outside collaborators*: **Require approval for all outside collaborators**.

Then nothing from someone else's pull request runs until you approve it, and
you never approve a run for a pull request you have not read.

## 4. Use it

- New commits on `main` or `netcode-v2` start a build; the nightly run builds
  only if that commit has no zip yet. **Actions** > **local-windows** > **Run
  workflow** starts one by hand.
- The finished zip lands in `TM_BUILDS_DIR`. The run's log on GitHub ends with
  its path.
- The runner's work folder (`C:\actions-runner\_work`) holds a copy of the
  source and generated code; the ROM copy in it sits in `input\`, which git
  ignores and nothing uploads.

The build is the same as the original project's Windows CI (clang-cl,
N64Recomp from the ROM, RT64 with `patches/rt64`), plus the Steam library.
