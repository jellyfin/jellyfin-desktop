---
name: Bug report
about: Create a report to help us improve
title: ''
labels: bug
assignees: ''

---

> [!IMPORTANT]
> Versions v1.12.0 and older are no longer supported. We will only be accepting issues found in the builds linked to in the README https://github.com/jellyfin/jellyfin-desktop?tab=readme-ov-file#downloads 

Four considerations:
 - Please do not open bug reports to ask questions. Use the Discussions feature instead.
 - Please make sure the issue only pertains to Jellyfin Desktop. If it also occurs in the web client, send the issue to jellyfin-web instead.
 - Please make sure that your issue is not being caused by errors in custom CSS or note that you are using custom CSS.
     - Notably, there have been instances of custom CSS breaking TV mode.
     - You can disable custom CSS under Display in the user settings.
 - Please provide logs. You can drag the log file into the issue to attach it.
     - Windows: `%LOCALAPPDATA%\Jellyfin Desktop\logs`
     - Linux: `~/.local/share/jellyfin-desktop/logs/`
     - Linux (Flatpak): `~/.var/app/org.jellyfin.JellyfinDesktop/data/jellyfin-desktop/logs/`
     - macOS: `~/Library/Logs/Jellyfin Desktop/`

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Go to '...'
2. Click on '....'
3. Scroll down to '....'
4. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Screenshots**
If applicable, add screenshots to help explain your problem.

**Desktop (please complete the following information):**
 - OS: [e.g. iOS]
 - Version [e.g. 22]
 - Installation Method [e.g. windows installer]

**Additional context**
Add any other context about the problem here.
