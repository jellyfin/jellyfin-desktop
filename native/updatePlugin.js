(function() {
// Compares two version strings as used in GitHub release tags ("1.12.0", "v2.1.0-pre1",
// "2.0.0-dev"). Returns > 0 when a is newer than b, < 0 when older, 0 when equal, NaN when
// either cannot be parsed. A suffix such as "-dev" or "-pre1" ranks below the plain release
// with the same numbers, so "2.0.0" is newer than "2.0.0-dev".
function compareVersions(a, b) {
    const parse = (v) => {
        const m = /^v?(\d+(?:\.\d+)*)(?:-([0-9A-Za-z.]+))?$/.exec(String(v).trim());
        if (!m) return null;
        return { nums: m[1].split('.').map(Number), pre: m[2] || '' };
    };
    const pa = parse(a), pb = parse(b);
    if (!pa || !pb) return NaN;
    const n = Math.max(pa.nums.length, pb.nums.length);
    for (let i = 0; i < n; i++) {
        const d = (pa.nums[i] || 0) - (pb.nums[i] || 0);
        if (d !== 0) return d;
    }
    if (pa.pre === pb.pre) return 0;
    if (!pa.pre) return 1;
    if (!pb.pre) return -1;
    return pa.pre.localeCompare(pb.pre, undefined, { numeric: true });
}

class updatePlugin {
    constructor({ confirm }) {
        this.name = 'Update Plugin';
        this.type = 'input';
        this.id = 'updatePlugin';

        (async () => {
            const api = await window.apiPromise;

            const onUpdateNotify = async (url) => {
                // Older native code emitted this placeholder when it could not do HTTPS itself and
                // expected the page to fetch github.com instead. That fetch is always blocked by
                // CORS (the web client runs on the server's origin), so there is nothing to do.
                if (!url || url === "SSL_UNAVAILABLE") return;

                // url is the redirect target of /releases/latest, e.g. .../releases/tag/v1.12.0
                const tag = url.split("/").pop();
                const version = tag.replace(/^v/, '');
                const currentVersion = jmpInfo.version;

                // Only offer versions that are actually newer than this build. "latest" on GitHub
                // is the newest stable release, so pre-release and -dev builds are never sent to
                // an older stable release and are offered the stable release once it passes them.
                const cmp = compareVersions(version, currentVersion);
                if (!(cmp > 0)) return;

                try {
                    // wait 3 seconds before showing the dialog to prevent race conditions
                    await new Promise(resolve => setTimeout(resolve, 3000));

                    await confirm({
                        title: "Update Available",
                        text: `Jellyfin Desktop version ${version} is available.`,
                        cancelText: "Ignore",
                        confirmText: "Download"
                    });

                    api.system.openExternalUrl(url);
                } catch (e) {
                    // User cancelled update
                }
            }

            api.system.updateInfoEmitted.connect(onUpdateNotify);
            api.system.checkForUpdates();
        })();
    }
}

window._updatePlugin = updatePlugin;
})();
