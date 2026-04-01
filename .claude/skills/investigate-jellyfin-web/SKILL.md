---
name: investigate-jellyfin-web
description: Use when researching features in jellyfin-web (the Jellyfin server's web UI rendered in CEF) for reference during implementation
---

# Investigating jellyfin-web

## Overview

jellyfin-web is the web UI served by the Jellyfin server, rendered in our CEF layer.

## Repo Location

Lives in `third_party/` (gitignored):
- `third_party/jellyfin-web`

Clone if not present:

```sh
git clone --recurse-submodules https://github.com/jellyfin/jellyfin-web.git third_party/jellyfin-web
```

## Key Areas

| Path | Purpose |
|------|---------|
| `src/controllers/` | Page controllers (playback, settings, etc) |
| `src/components/` | Reusable UI components |
| `src/plugins/` | Plugin architecture |
| `src/scripts/` | Core JS logic |

When investigating web UI behavior, search this repo.

## Workflow

1. Clone repo if not present
2. Use Explore agent or grep to find relevant code
3. Apply learnings to this CEF+mpv desktop app
